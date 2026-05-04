// Hypersion — Stockfish 18 NNUE inference.
//
// Self-contained NNUE module that reads SF18's binary .nnue files and runs
// forward propagation. The architecture (SFNNv10: HalfKAv2_hm features +
// FullThreats inputs + 8 PSQT buckets + 8 layer stacks + paired clipped
// ReLU) is implemented directly here — no Stockfish source is linked.
//
// Adapted from HybridChess_v17's NNUE module (single-file engine, GPL).
// The lookup-table layout, feature indexing, LEB128 weight decoder, and
// SIMD primitives are all ported verbatim because they encode the SF18
// binary format. The position-side surface is rewritten to use Hypersion's
// own Position / Bitboards API.

#include "nnue.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "bitboard.h"
#include "position.h"
#include "types.h"

#if defined(__AVX2__)
    #include <immintrin.h>
    #define HC_SIMD_AVX2 1
#elif defined(__SSE2__)
    #include <emmintrin.h>
    #define HC_SIMD_SSE2 1
#endif

namespace hypersion::NNUE {

namespace {

// ─────────────────────────────────────────────────────────────────────────
// SF18 SFNNv10 constants (must match the binary file format exactly)
// ─────────────────────────────────────────────────────────────────────────
constexpr std::uint32_t NNUE_VERSION = 0x7AF32F20u;
constexpr int PSQT_BUCKETS = 8;
constexpr int LAYER_STACKS = 8;
constexpr int OUTPUT_SCALE = 16;
constexpr int WSCALE       = 6;
// Eval-output divisor: attenuates raw NNUE eval to match Hypersion's
// search-margin magnitude. Phase 9 set this to 1 (no attenuation) and
// instead scaled the margins themselves by 3 — see search.cpp constexpr
// block. Tunable at build time via -DNNUE_DIVISOR=N.
//   Empirical tournament history at the OLD margin scale (margins NOT *3):
//     /2 vs /3:  -79 ELO  (eval too large, search over-prunes)
//     /3 vs /4:  +61 ELO  (best at the OLD margin scale)
//   Phase 9 (this build): NNUE_DIVISOR=1 + margins *3 in search.cpp.
#ifndef NNUE_DIVISOR
#define NNUE_DIVISOR 1
#endif
constexpr int PS_NB        = 11 * 64;
constexpr int PSQ_DIM      = 64 * PS_NB / 2;   // 22528
constexpr int THREAT_DIM   = 79856;

// ─────────────────────────────────────────────────────────────────────────
// HalfKAv2_hm feature mapping (from SF18 half_ka_v2_hm.{h,cpp})
// PSI[persp][piece] gives the per-piece offset within a king-bucket block.
// piece is a Piece enum value: 1..6 white, 9..14 black.
// ─────────────────────────────────────────────────────────────────────────
constexpr int PSI[2][16] = {
    {0,   0, 128, 256, 384, 512, 640, 0,
     0,  64, 192, 320, 448, 576, 640, 0},
    {0,  64, 192, 320, 448, 576, 640, 0,
     0,   0, 128, 256, 384, 512, 640, 0},
};

#define KB(v) ((v) * PS_NB)
constexpr int KING_BUCKETS[64] = {
    KB(28),KB(29),KB(30),KB(31),KB(31),KB(30),KB(29),KB(28),
    KB(24),KB(25),KB(26),KB(27),KB(27),KB(26),KB(25),KB(24),
    KB(20),KB(21),KB(22),KB(23),KB(23),KB(22),KB(21),KB(20),
    KB(16),KB(17),KB(18),KB(19),KB(19),KB(18),KB(17),KB(16),
    KB(12),KB(13),KB(14),KB(15),KB(15),KB(14),KB(13),KB(12),
    KB( 8),KB( 9),KB(10),KB(11),KB(11),KB(10),KB( 9),KB( 8),
    KB( 4),KB( 5),KB( 6),KB( 7),KB( 7),KB( 6),KB( 5),KB( 4),
    KB( 0),KB( 1),KB( 2),KB( 3),KB( 3),KB( 2),KB( 1),KB( 0),
};
#undef KB

// PSQ orientation: files a–d -> 7, files e–h -> 0
constexpr int PSQ_ORIENT[64] = {
    7,7,7,7,0,0,0,0, 7,7,7,7,0,0,0,0, 7,7,7,7,0,0,0,0, 7,7,7,7,0,0,0,0,
    7,7,7,7,0,0,0,0, 7,7,7,7,0,0,0,0, 7,7,7,7,0,0,0,0, 7,7,7,7,0,0,0,0,
};

inline int psq_index(int persp, int sq, int pc, int ksq) {
    int flip = 56 * persp;
    return (sq ^ PSQ_ORIENT[ksq] ^ flip)
         + PSI[persp][pc]
         + KING_BUCKETS[ksq ^ flip];
}

// ─────────────────────────────────────────────────────────────────────────
// FullThreats feature set (SF18 full_threats.{h,cpp})
// Threat orientation is OPPOSITE of PSQ: files a–d -> 0, files e–h -> 7.
// ─────────────────────────────────────────────────────────────────────────
constexpr int THR_ORIENT[64] = {
    0,0,0,0,7,7,7,7, 0,0,0,0,7,7,7,7, 0,0,0,0,7,7,7,7, 0,0,0,0,7,7,7,7,
    0,0,0,0,7,7,7,7, 0,0,0,0,7,7,7,7, 0,0,0,0,7,7,7,7, 0,0,0,0,7,7,7,7,
};
constexpr int numValidTargets[16] = {
    0, 6, 12, 10, 10, 12, 8, 0,
    0, 6, 12, 10, 10, 12, 8, 0
};
constexpr int threat_map[6][6] = {
    {0, 1,-1, 2,-1,-1},
    {0, 1, 2, 3, 4, 5},
    {0, 1, 2, 3,-1, 4},
    {0, 1, 2, 3,-1, 4},
    {0, 1, 2, 3, 4, 5},
    {0, 1, 2, 3,-1,-1},
};

struct ThreatLUT {
    std::uint32_t lut1[16][16][2];
    std::uint32_t off [16][64];
    std::uint8_t  lut2[16][64][64];
    struct H { int pieceOff, cumOff; } helpers[16];
    bool ok = false;
};
ThreatLUT g_thr;

// Hypersion-side pseudo-attacks for a given piece/square. Pawns need color;
// pieces use Hypersion's PseudoAttacks / attacks_bb<>.
Bitboard pseudo_atk(Piece pc, Square s) {
    PieceType pt = type_of(pc);
    Color c = color_of(pc);
    switch (pt) {
        case PAWN:   return pawn_attacks_bb(c, s);
        case KNIGHT: return PseudoAttacks[KNIGHT][s];
        case BISHOP: return attacks_bb<BISHOP>(s, 0);
        case ROOK:   return attacks_bb<ROOK>  (s, 0);
        case QUEEN:  return attacks_bb<QUEEN> (s, 0);
        case KING:   return PseudoAttacks[KING][s];
        default:     return 0;
    }
}

void build_threat_tables() {
    if (g_thr.ok) return;
    // void* cast: ThreatLUT is POD-layout but g++ classifies it as
    // non-trivially-copyable because of the bool default-init.
    std::memset(static_cast<void*>(&g_thr), 0, sizeof(g_thr));
    static const Piece AP[12] = {
        W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
        B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    };
    // lut2: popcount of pseudo-attacks below 'to'
    for (Piece pc : AP) {
        for (int from = 0; from < 64; ++from) {
            Bitboard attacks = pseudo_atk(pc, Square(from));
            if (type_of(pc) == PAWN && (from < 8 || from > 55)) attacks = 0;
            for (int to = 0; to < 64; ++to)
                g_thr.lut2[int(pc)][from][to] = popcount(((1ULL << to) - 1) & attacks);
        }
    }
    // offsets + helpers
    int cumOff = 0;
    for (Piece pc : AP) {
        int pi = int(pc), cumPieceOff = 0;
        for (int from = 0; from < 64; ++from) {
            g_thr.off[pi][from] = cumPieceOff;
            Bitboard attacks = pseudo_atk(pc, Square(from));
            if (type_of(pc) == PAWN && (from < 8 || from > 55)) attacks = 0;
            cumPieceOff += popcount(attacks);
        }
        g_thr.helpers[pi].pieceOff = cumPieceOff;
        g_thr.helpers[pi].cumOff   = cumOff;
        cumOff += numValidTargets[pi] * cumPieceOff;
    }
    // lut1
    for (Piece atk : AP) for (Piece def : AP) {
        bool enemy = ((int(atk) ^ int(def)) == 8);
        int apt = int(type_of(atk)) - 1;
        int dpt = int(type_of(def)) - 1;
        int m = threat_map[apt][dpt];
        bool semi = (type_of(atk) == type_of(def)) && (enemy || type_of(atk) != PAWN);
        bool excl = (m < 0);
        std::uint32_t feat = g_thr.helpers[int(atk)].cumOff
            + (int(color_of(def)) * (numValidTargets[int(atk)] / 2) + m)
              * g_thr.helpers[int(atk)].pieceOff;
        g_thr.lut1[int(atk)][int(def)][0] = excl ? THREAT_DIM : feat;
        g_thr.lut1[int(atk)][int(def)][1] = (excl || semi) ? THREAT_DIM : feat;
    }
    g_thr.ok = true;
}

inline int threat_index(int persp, int attacker, int from, int to,
                        int attacked, int ksq) {
    int orient = THR_ORIENT[ksq] ^ (56 * persp);
    int from_o = from ^ orient;
    int to_o   = to   ^ orient;
    int swap   = 8 * persp;
    int a_o    = attacker ^ swap;
    int d_o    = attacked ^ swap;
    return g_thr.lut1[a_o][d_o][from_o < to_o]
         + g_thr.off [a_o][from_o]
         + g_thr.lut2[a_o][from_o][to_o];
}

// ─────────────────────────────────────────────────────────────────────────
// LEB128 weight decompression (SF18 stores most weights this way)
// ─────────────────────────────────────────────────────────────────────────
template<typename T>
bool read_leb128(std::ifstream& f, T* out, std::size_t n) {
    char mag[17];
    f.read(mag, 17);
    if (!f || std::memcmp(mag, "COMPRESSED_LEB128", 17) != 0) return false;
    std::uint32_t bc;
    f.read(reinterpret_cast<char*>(&bc), 4);
    if (!f) return false;
    std::vector<std::uint8_t> buf(bc);
    f.read(reinterpret_cast<char*>(buf.data()), bc);
    if (!f) return false;
    std::size_t p = 0;
    for (std::size_t i = 0; i < n; ++i) {
        T r = 0;
        std::size_t sh = 0;
        std::uint8_t b;
        do {
            if (p >= buf.size()) return false;
            b = buf[p++];
            r |= T(b & 0x7f) << sh;
            sh += 7;
        } while (b & 0x80);
        if (sh < sizeof(T) * 8 && (b & 0x40)) r |= ~T(0) << sh;
        out[i] = r;
    }
    return true;
}

template<typename T>
bool read_leb128_2(std::ifstream& f, T* a, std::size_t na, T* b2, std::size_t nb) {
    char mag[17];
    f.read(mag, 17);
    if (!f || std::memcmp(mag, "COMPRESSED_LEB128", 17) != 0) return false;
    std::uint32_t bc;
    f.read(reinterpret_cast<char*>(&bc), 4);
    if (!f) return false;
    std::vector<std::uint8_t> buf(bc);
    f.read(reinterpret_cast<char*>(buf.data()), bc);
    if (!f) return false;
    std::size_t p = 0;
    auto decode = [&](T* out, std::size_t n) -> bool {
        for (std::size_t i = 0; i < n; ++i) {
            T r = 0;
            std::size_t sh = 0;
            std::uint8_t b;
            do {
                if (p >= buf.size()) return false;
                b = buf[p++];
                r |= T(b & 0x7f) << sh;
                sh += 7;
            } while (b & 0x80);
            if (sh < sizeof(T) * 8 && (b & 0x40)) r |= ~T(0) << sh;
            out[i] = r;
        }
        return true;
    };
    return decode(a, na) && decode(b2, nb);
}

inline int pad32(int n) { return ((n + 31) / 32) * 32; }

// ─────────────────────────────────────────────────────────────────────────
// SIMD primitives (AVX2 / SSE2 / scalar)
// ─────────────────────────────────────────────────────────────────────────
inline std::int32_t simd_dot_i8_u8(const std::int8_t* w, const std::uint8_t* x, int n) {
#if defined(__AVXVNNI__) || defined(__AVX512VNNI__)
    // VNNI fast path (Stockfish src/nnue/simd.h vec_dpbusd_32). One
    // _mm256_dpbusd_epi32 replaces the maddubs+madd+add chain — cuts the
    // latency-bound FC dot product roughly in half on Alder Lake+.
    __m256i acc = _mm256_setzero_si256();
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i));
        __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i));
        acc = _mm256_dpbusd_epi32(acc, vx, vw);   // x is unsigned, w signed
    }
    __m128i lo = _mm256_castsi256_si128(acc);
    __m128i hi = _mm256_extracti128_si256(acc, 1);
    __m128i sum128 = _mm_add_epi32(lo, hi);
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, 0x4E));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, 0xB1));
    std::int32_t result = _mm_cvtsi128_si32(sum128);
    for (; i < n; ++i) result += std::int32_t(w[i]) * std::int32_t(x[i]);
    return result;
#elif defined(HC_SIMD_AVX2)
    __m256i acc = _mm256_setzero_si256();
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + i));
        __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i));
        __m256i prod = _mm256_maddubs_epi16(vx, vw);
        prod = _mm256_madd_epi16(prod, _mm256_set1_epi16(1));
        acc = _mm256_add_epi32(acc, prod);
    }
    __m128i lo = _mm256_castsi256_si128(acc);
    __m128i hi = _mm256_extracti128_si256(acc, 1);
    __m128i sum128 = _mm_add_epi32(lo, hi);
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, 0x4E));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, 0xB1));
    std::int32_t result = _mm_cvtsi128_si32(sum128);
    for (; i < n; ++i) result += std::int32_t(w[i]) * std::int32_t(x[i]);
    return result;
#elif defined(HC_SIMD_SSE2)
    __m128i acc0 = _mm_setzero_si128();
    __m128i acc1 = _mm_setzero_si128();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i vw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w + i));
        __m128i vx = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
        __m128i w_lo = _mm_srai_epi16(_mm_unpacklo_epi8(vw, vw), 8);
        __m128i w_hi = _mm_srai_epi16(_mm_unpackhi_epi8(vw, vw), 8);
        __m128i x_lo = _mm_unpacklo_epi8(vx, _mm_setzero_si128());
        __m128i x_hi = _mm_unpackhi_epi8(vx, _mm_setzero_si128());
        acc0 = _mm_add_epi32(acc0, _mm_madd_epi16(w_lo, x_lo));
        acc1 = _mm_add_epi32(acc1, _mm_madd_epi16(w_hi, x_hi));
    }
    __m128i sum128 = _mm_add_epi32(acc0, acc1);
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, 0x4E));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, 0xB1));
    std::int32_t result = _mm_cvtsi128_si32(sum128);
    for (; i < n; ++i) result += std::int32_t(w[i]) * std::int32_t(x[i]);
    return result;
#else
    std::int32_t r = 0;
    for (int i = 0; i < n; ++i) r += std::int32_t(w[i]) * std::int32_t(x[i]);
    return r;
#endif
}

inline void simd_acc_add_i16(std::int16_t* acc, const std::int16_t* col, int n) {
#if defined(HC_SIMD_AVX2)
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + i));
        __m256i vc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(col + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc + i),
                            _mm256_add_epi16(va, vc));
    }
    for (; i < n; ++i) acc[i] += col[i];
#elif defined(HC_SIMD_SSE2)
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + i));
        __m128i vc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(col + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(acc + i),
                         _mm_add_epi16(va, vc));
    }
    for (; i < n; ++i) acc[i] += col[i];
#else
    for (int i = 0; i < n; ++i) acc[i] += col[i];
#endif
}

// Subtract a column. Mirror of simd_acc_add_i16 — used by incremental
// updates when a piece is removed from the board.
inline void simd_acc_sub_i16(std::int16_t* acc, const std::int16_t* col, int n) {
#if defined(HC_SIMD_AVX2)
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + i));
        __m256i vc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(col + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc + i),
                            _mm256_sub_epi16(va, vc));
    }
    for (; i < n; ++i) acc[i] -= col[i];
#elif defined(HC_SIMD_SSE2)
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + i));
        __m128i vc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(col + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(acc + i),
                         _mm_sub_epi16(va, vc));
    }
    for (; i < n; ++i) acc[i] -= col[i];
#else
    for (int i = 0; i < n; ++i) acc[i] -= col[i];
#endif
}

inline void simd_acc_add_i8_to_i16(std::int16_t* acc, const std::int8_t* col, int n) {
#if defined(HC_SIMD_AVX2)
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i v8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(col + i));
        __m256i v16 = _mm256_cvtepi8_epi16(v8);
        __m256i va  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc + i),
                            _mm256_add_epi16(va, v16));
    }
    for (; i < n; ++i) acc[i] += std::int16_t(col[i]);
#else
    for (int i = 0; i < n; ++i) acc[i] += std::int16_t(col[i]);
#endif
}

// PSQT bucket helpers — there are exactly PSQT_BUCKETS=8 int32 entries per
// accumulator, which fits in one AVX2 ymm register. Replaces the
// `for (int i=0; i<8; ++i) psqt[i] += p[i];` scalar loops scattered across
// refresh_psq / refresh_threats / cached_refresh / apply_dirty.
inline void simd_psqt_add(std::int32_t* psqt, const std::int32_t* col) {
#if defined(HC_SIMD_AVX2)
    __m256i p = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(psqt));
    __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(col));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(psqt), _mm256_add_epi32(p, c));
#else
    for (int i = 0; i < PSQT_BUCKETS; ++i) psqt[i] += col[i];
#endif
}
inline void simd_psqt_sub(std::int32_t* psqt, const std::int32_t* col) {
#if defined(HC_SIMD_AVX2)
    __m256i p = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(psqt));
    __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(col));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(psqt), _mm256_sub_epi32(p, c));
#else
    for (int i = 0; i < PSQT_BUCKETS; ++i) psqt[i] -= col[i];
#endif
}

// ─────────────────────────────────────────────────────────────────────────
// Finny tables — refresh cache.
//
// When a king move triggers a full PSQ-accumulator refresh, walking all
// 32 pieces is expensive. The Finny cache stores, per (king-bucket-orient,
// color) key, the accumulator state plus the per-piece-type bitboards it
// represents. On refresh we just diff the cached piece set against the
// current pos, applying sub/add for the changed pieces — usually a handful
// instead of 32. Reference: Stockfish nnue_accumulator.cpp's RefreshCache.
//
// Key derivation: HalfKAv2_hm features depend on KING_BUCKETS[ksq^flip]
// (32 buckets) AND PSQ_ORIENT[ksq^flip] (2 orientations) — same bucket
// with different orient gives different feature indices, so we need 64
// distinct keys per color.
//
// Memory per thread: big = 64*2 entries × ~2.2KB ≈ 280KB; small ≈ 53KB.
// ─────────────────────────────────────────────────────────────────────────
constexpr int FINNY_KEYS = 64;

inline int finny_key(int persp, int ksq) {
    int flip = 56 * persp;
    int kb = KING_BUCKETS[ksq ^ flip] / PS_NB;          // 0..31
    int orient = PSQ_ORIENT[ksq ^ flip] == 0 ? 0 : 1;   // 0..1
    return kb * 2 + orient;
}

template<int L1>
struct alignas(64) FinnyEntry {
    alignas(64) std::int16_t acc[L1];
    std::int32_t psqt[PSQT_BUCKETS];
    Bitboard pieces_bb[16];     // [Piece] -> squares occupied by that piece (12 used, 16 for index)
    bool valid = false;
};

struct FinnyCache {
    FinnyEntry<1024> big  [FINNY_KEYS][COLOR_NB];
    FinnyEntry<128>  small[FINNY_KEYS][COLOR_NB];

    void invalidate() {
        for (int k = 0; k < FINNY_KEYS; ++k)
            for (int c = 0; c < COLOR_NB; ++c) {
                big  [k][c].valid = false;
                small[k][c].valid = false;
            }
    }
};

// One cache per search thread. Initialised "all-invalid" — the first refresh
// for any (key, color) bootstraps from FT bias.
thread_local FinnyCache g_finny;

// ─────────────────────────────────────────────────────────────────────────
// NNUE network
// ─────────────────────────────────────────────────────────────────────────
struct Network {
    int  l1 = 0, l2 = 0, l3 = 0;
    bool loaded = false;
    bool is_big = false;
    std::vector<std::int16_t> ft_bias;       // [l1]
    std::vector<std::int16_t> ft_w;          // [PSQ_DIM * l1]
    std::vector<std::int8_t>  ft_thr_w;      // [THREAT_DIM * l1]   (big only)
    std::vector<std::int32_t> ft_psqt;       // [PSQ_DIM * 8]
    std::vector<std::int32_t> ft_thr_psqt;   // [THREAT_DIM * 8]    (big only)
    struct FC {
        int pi = 0, out = 0;
        std::vector<std::int32_t> bias;
        std::vector<std::int8_t>  weight;
    };
    FC fc[LAYER_STACKS][3];

    static int bucket(const Position& pos) {
        int n = popcount(pos.pieces());
        return (std::max(1, std::min(32, n)) - 1) / 4;
    }

    // Apply a single DirtyPiece transition to the running accumulator. The
    // `ksq` is the king square FOR PERSPECTIVE c — same value for the whole
    // chain because king moves trigger a full refresh elsewhere. Used by
    // the lazy incremental updater.
    void apply_dirty(const DirtyPiece& d, int persp, int ksq,
                     std::int16_t* acc, std::int32_t* psqt) const {
        if (d.from != SQ_NONE) {
            int idx = psq_index(persp, int(d.from), int(d.pc), ksq);
            if (idx >= 0 && idx < PSQ_DIM) {
                simd_acc_sub_i16(acc, &ft_w[std::size_t(idx) * l1], l1);
                simd_psqt_sub(psqt, &ft_psqt[std::size_t(idx) * PSQT_BUCKETS]);
            }
        }
        if (d.to != SQ_NONE) {
            int idx = psq_index(persp, int(d.to), int(d.pc), ksq);
            if (idx >= 0 && idx < PSQ_DIM) {
                simd_acc_add_i16(acc, &ft_w[std::size_t(idx) * l1], l1);
                simd_psqt_add(psqt, &ft_psqt[std::size_t(idx) * PSQT_BUCKETS]);
            }
        }
    }

    // PSQ accumulator refresh — iterates all pieces, sums their FT columns.
    // Used as the slow-path / validation reference; production refresh goes
    // through cached_refresh below.
    void refresh_psq(const Position& pos, std::int16_t* acc, std::int32_t* psqt_acc,
                     int persp) const {
        for (int i = 0; i < l1; ++i) acc[i] = ft_bias[i];
        for (int i = 0; i < PSQT_BUCKETS; ++i) psqt_acc[i] = 0;
        Color c = (persp == 0) ? WHITE : BLACK;
        int ksq = int(pos.square<KING>(c));
        for (int s = 0; s < 64; ++s) {
            Piece p = pos.piece_on(Square(s));
            if (p == NO_PIECE) continue;
            int idx = psq_index(persp, s, int(p), ksq);
            if (idx < 0 || idx >= PSQ_DIM) continue;
            simd_acc_add_i16(acc, &ft_w[std::size_t(idx) * l1], l1);
            simd_psqt_add(psqt_acc, &ft_psqt[std::size_t(idx) * PSQT_BUCKETS]);
        }
    }

    // Finny-cached refresh. Diffs the cached piece set for this
    // (king-bucket-orient, color) key against the current pos and applies
    // only the differences. Falls back to full refresh on first use.
    template<int L1>
    void cached_refresh_impl(const Position& pos, FinnyEntry<L1>& entry,
                             std::int16_t* dst_acc, std::int32_t* dst_psqt,
                             int persp) const {
        Color c = (persp == 0) ? WHITE : BLACK;
        int ksq = int(pos.square<KING>(c));

        if (!entry.valid) {
            // Bootstrap entry from FT bias + zero piece set.
            for (int i = 0; i < l1; ++i) entry.acc[i] = ft_bias[i];
            for (int i = 0; i < PSQT_BUCKETS; ++i) entry.psqt[i] = 0;
            for (int i = 0; i < 16; ++i) entry.pieces_bb[i] = 0;
            entry.valid = true;
        }

        // Diff: walk all 12 piece types, sub removed pieces, add new ones.
        static const Piece AP[12] = {
            W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
            B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
        };
        for (Piece pc : AP) {
            Bitboard cur    = pos.pieces(color_of(pc), type_of(pc));
            Bitboard cached = entry.pieces_bb[int(pc)];
            Bitboard remove = cached & ~cur;
            Bitboard add    = cur & ~cached;
            while (remove) {
                Square s = pop_lsb(remove);
                int idx = psq_index(persp, int(s), int(pc), ksq);
                if (idx >= 0 && idx < PSQ_DIM) {
                    simd_acc_sub_i16(entry.acc, &ft_w[std::size_t(idx) * l1], l1);
                    simd_psqt_sub(entry.psqt, &ft_psqt[std::size_t(idx) * PSQT_BUCKETS]);
                }
            }
            while (add) {
                Square s = pop_lsb(add);
                int idx = psq_index(persp, int(s), int(pc), ksq);
                if (idx >= 0 && idx < PSQ_DIM) {
                    simd_acc_add_i16(entry.acc, &ft_w[std::size_t(idx) * l1], l1);
                    simd_psqt_add(entry.psqt, &ft_psqt[std::size_t(idx) * PSQT_BUCKETS]);
                }
            }
            entry.pieces_bb[int(pc)] = cur;
        }

        // Publish cache state into the StateInfo's accumulator slot.
        std::memcpy(dst_acc,  entry.acc,  l1 * sizeof(std::int16_t));
        std::memcpy(dst_psqt, entry.psqt, PSQT_BUCKETS * sizeof(std::int32_t));
    }

    void cached_refresh(const Position& pos, std::int16_t* dst_acc,
                        std::int32_t* dst_psqt, int persp) const {
        Color c = (persp == 0) ? WHITE : BLACK;
        int ksq = int(pos.square<KING>(c));
        int key = finny_key(persp, ksq);
        if (is_big)
            cached_refresh_impl(pos, g_finny.big  [key][int(c)], dst_acc, dst_psqt, persp);
        else
            cached_refresh_impl(pos, g_finny.small[key][int(c)], dst_acc, dst_psqt, persp);
    }

    // Threat accumulator (big net only).
    void refresh_threats(const Position& pos, std::int16_t* acc, std::int32_t* psqt_acc,
                         int persp) const {
        if (!is_big || ft_thr_w.empty()) return;
        Color persp_c = (persp == 0) ? WHITE : BLACK;
        int ksq = int(pos.square<KING>(persp_c));
        Bitboard occ = pos.pieces();
        for (int color = 0; color <= 1; ++color) {
            Color c = Color(persp ^ color);
            for (int pt = PAWN; pt <= KING; ++pt) {
                Piece attacker = make_piece(c, PieceType(pt));
                Bitboard bb = pos.pieces(c, PieceType(pt));
                if (pt == PAWN) {
                    int fwd_r = (c == WHITE) ?  9 : -7;
                    int fwd_l = (c == WHITE) ?  7 : -9;
                    Bitboard atk_r = (c == WHITE) ? ((bb & ~FileHBB) << 9)
                                                   : ((bb & ~FileHBB) >> 7);
                    Bitboard atk_l = (c == WHITE) ? ((bb & ~FileABB) << 7)
                                                   : ((bb & ~FileABB) >> 9);
                    atk_r &= occ; atk_l &= occ;
                    while (atk_r) {
                        Square to = pop_lsb(atk_r);
                        int from = int(to) - fwd_r;
                        Piece attacked = pos.piece_on(to);
                        if (attacked == NO_PIECE) continue;
                        int idx = threat_index(persp, int(attacker), from, int(to),
                                               int(attacked), ksq);
                        if (idx < 0 || idx >= THREAT_DIM) continue;
                        simd_acc_add_i8_to_i16(acc, &ft_thr_w[std::size_t(idx) * l1], l1);
                        if (!ft_thr_psqt.empty())
                            simd_psqt_add(psqt_acc, &ft_thr_psqt[std::size_t(idx) * PSQT_BUCKETS]);
                    }
                    while (atk_l) {
                        Square to = pop_lsb(atk_l);
                        int from = int(to) - fwd_l;
                        Piece attacked = pos.piece_on(to);
                        if (attacked == NO_PIECE) continue;
                        int idx = threat_index(persp, int(attacker), from, int(to),
                                               int(attacked), ksq);
                        if (idx < 0 || idx >= THREAT_DIM) continue;
                        simd_acc_add_i8_to_i16(acc, &ft_thr_w[std::size_t(idx) * l1], l1);
                        if (!ft_thr_psqt.empty())
                            simd_psqt_add(psqt_acc, &ft_thr_psqt[std::size_t(idx) * PSQT_BUCKETS]);
                    }
                } else {
                    while (bb) {
                        Square from = pop_lsb(bb);
                        Bitboard attacks = 0;
                        switch (pt) {
                            case KNIGHT: attacks = PseudoAttacks[KNIGHT][from]; break;
                            case BISHOP: attacks = attacks_bb<BISHOP>(from, occ); break;
                            case ROOK:   attacks = attacks_bb<ROOK>  (from, occ); break;
                            case QUEEN:  attacks = attacks_bb<QUEEN> (from, occ); break;
                            case KING:   attacks = PseudoAttacks[KING][from];   break;
                            default: break;
                        }
                        attacks &= occ;
                        while (attacks) {
                            Square to = pop_lsb(attacks);
                            Piece attacked = pos.piece_on(to);
                            if (attacked == NO_PIECE) continue;
                            int idx = threat_index(persp, int(attacker), int(from), int(to),
                                                   int(attacked), ksq);
                            if (idx < 0 || idx >= THREAT_DIM) continue;
                            simd_acc_add_i8_to_i16(acc, &ft_thr_w[std::size_t(idx) * l1], l1);
                            if (!ft_thr_psqt.empty())
                                simd_psqt_add(psqt_acc, &ft_thr_psqt[std::size_t(idx) * PSQT_BUCKETS]);
                        }
                    }
                }
            }
        }
    }

    // Forward propagation. Reads the precomputed PSQ accumulator from
    // `pos.state()->nnue.{big,small}_{acc,psqt}` — the caller (NNUE::evaluate)
    // must have invoked make_valid() for both colors first to populate it.
    // Threat features are still recomputed every call (non-incremental in SF18
    // too: a single piece move can change many threat indices, making
    // incremental tracking lose vs. an outright refresh).
    int forward(const Position& pos) const {
        if (!loaded) return 0;
        StateInfo* st = pos.state();

        // PSQ accumulator views — these were populated by make_valid().
        const std::int16_t* aw_acc;
        const std::int16_t* ab_acc;
        const std::int32_t* pw_psq;
        const std::int32_t* pb_psq;
        if (is_big) {
            aw_acc = st->nnue.big_acc [0];
            ab_acc = st->nnue.big_acc [1];
            pw_psq = st->nnue.big_psqt[0];
            pb_psq = st->nnue.big_psqt[1];
        } else {
            aw_acc = st->nnue.small_acc [0];
            ab_acc = st->nnue.small_acc [1];
            pw_psq = st->nnue.small_psqt[0];
            pb_psq = st->nnue.small_psqt[1];
        }

        // Threats remain non-incremental — refreshed each forward() call.
        alignas(64) std::int16_t tw[1024], tb2[1024];
        alignas(64) std::int32_t tpw[8] = {}, tpb[8] = {};
        if (is_big) {
            std::memset(tw,  0, l1 * sizeof(std::int16_t));
            std::memset(tb2, 0, l1 * sizeof(std::int16_t));
            refresh_threats(pos, tw,  tpw, 0);
            refresh_threats(pos, tb2, tpb, 1);
        }

        int bkt = bucket(pos);
        Color stm = pos.side_to_move();
        int persp[2] = { int(stm), int(~stm) };
        std::int32_t psqt_stm  = (persp[0] == WHITE) ? pw_psq[bkt] : pb_psq[bkt];
        std::int32_t psqt_nstm = (persp[0] == WHITE) ? pb_psq[bkt] : pw_psq[bkt];
        std::int32_t psqt_raw  = psqt_stm - psqt_nstm;
        if (is_big) {
            std::int32_t* tp_stm  = (persp[0] == WHITE) ? tpw : tpb;
            std::int32_t* tp_nstm = (persp[0] == WHITE) ? tpb : tpw;
            psqt_raw = (psqt_raw + tp_stm[bkt] - tp_nstm[bkt]) / 2;
        } else {
            psqt_raw /= 2;
        }

        const std::int16_t* accs [2] = { (persp[0] == WHITE) ? aw_acc : ab_acc,
                                         (persp[0] == WHITE) ? ab_acc : aw_acc };
        std::int16_t*       taccs[2] = { (persp[0] == WHITE) ? tw  : tb2,
                                         (persp[0] == WHITE) ? tb2 : tw  };
        int clamp_max = is_big ? 255 : (127 * 2);
        alignas(64) std::uint8_t transformed[1024];
        for (int p = 0; p < 2; ++p) {
            int offset = (l1 / 2) * p;
            for (int j = 0; j < l1 / 2; ++j) {
                int s0 = int(accs[p][j]);
                int s1 = int(accs[p][j + l1 / 2]);
                if (is_big) { s0 += int(taccs[p][j]); s1 += int(taccs[p][j + l1 / 2]); }
                s0 = std::clamp(s0, 0, clamp_max);
                s1 = std::clamp(s1, 0, clamp_max);
                transformed[offset + j] = std::uint8_t(unsigned(s0 * s1) / 512);
            }
        }

        const FC* L = fc[bkt];
        int fc0_out = l2 + 1;
        alignas(64) std::int32_t o0[64];
        for (int o = 0; o < fc0_out; ++o)
            o0[o] = L[0].bias[o] + simd_dot_i8_u8(&L[0].weight[o * L[0].pi], transformed, l1);
        std::int32_t skip = o0[l2] * (600 * OUTPUT_SCALE) / (127 * (1 << WSCALE));

        int fc1_in = 2 * l2;
        alignas(64) std::uint8_t f1in[128];
        std::memset(f1in, 0, sizeof(f1in));
        for (int i = 0; i < l2; ++i) {
            f1in[i]      = std::uint8_t(std::min<std::int64_t>(127,
                              (std::int64_t(o0[i]) * o0[i]) >> 19));
            f1in[l2 + i] = std::uint8_t(std::clamp(o0[i] >> WSCALE, 0, 127));
        }
        alignas(64) std::int32_t o1[64];
        for (int o = 0; o < l3; ++o)
            o1[o] = L[1].bias[o] + simd_dot_i8_u8(&L[1].weight[o * L[1].pi], f1in, fc1_in);

        alignas(64) std::uint8_t f2in[64];
        std::memset(f2in, 0, sizeof(f2in));
        for (int i = 0; i < l3; ++i)
            f2in[i] = std::uint8_t(std::clamp(o1[i] >> WSCALE, 0, 127));
        std::int32_t raw = L[2].bias[0]
            + simd_dot_i8_u8(L[2].weight.data(), f2in, l3);

        std::int32_t positional = raw + skip;
        int pv = int(psqt_raw / OUTPUT_SCALE);
        int pp = int(positional / OUTPUT_SCALE);
        int nnue = (125 * pv + 131 * pp) / 128;

        // Material scaling matching SF18 evaluate.cpp
        int mat = 534 * popcount(pos.pieces(PAWN));
        static const int mv[] = { 0, 0, 776, 825, 1276, 2538, 0 };
        for (int pt = KNIGHT; pt <= QUEEN; ++pt)
            mat += mv[pt] * (popcount(pos.pieces(WHITE, PieceType(pt)))
                           + popcount(pos.pieces(BLACK, PieceType(pt))));
        int v = (nnue * (77871 + mat)) / 77871;
        // Attenuate raw NNUE output to match Hypersion's classical-eval
        // magnitude (search params are Texel-tuned to that scale). Measured:
        // raw NNUE avg|v|=636, classical avg|v|=220, so divisor ~= 636/220 =
        // 2.9. Empirical A/B test selected this constant.
        v /= NNUE_DIVISOR;
        // Clamp scaled to NNUE_DIVISOR — Phase 9 with divisor=1 reaches up
        // to ~9000 cp on lopsided endgames, the old 3000 truncated those.
        const int CLAMP = 3000 * (3 / NNUE_DIVISOR > 0 ? 3 / NNUE_DIVISOR : 1);
        return std::clamp(v, -CLAMP, CLAMP);
    }

    bool load(const std::string& path, bool big) {
        loaded = false; is_big = big;
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::cerr << "info string nnue: cannot open " << path << '\n';
            return false;
        }
        f.seekg(0, std::ios::end);
        std::size_t fsz = f.tellg();
        f.seekg(0);

        std::uint32_t ver, ah, dlen;
        f.read(reinterpret_cast<char*>(&ver),  4);
        f.read(reinterpret_cast<char*>(&ah),   4);
        f.read(reinterpret_cast<char*>(&dlen), 4);
        if (!f || ver != NNUE_VERSION) {
            std::cerr << "info string nnue: version mismatch\n";
            return false;
        }
        if (dlen > 4096) return false;
        std::string desc(dlen, '\0');
        f.read(&desc[0], dlen);
        std::cerr << "info string nnue arch: " << desc << '\n';

        if (big) { l1 = 1024; l2 = 15; l3 = 32; }
        else     { l1 =  128; l2 = 15; l3 = 32; }

        std::uint32_t fth;
        f.read(reinterpret_cast<char*>(&fth), 4);
        std::cerr << "info string nnue: FT hash 0x"
                  << std::hex << fth << std::dec << '\n';

        ft_bias.resize(l1);
        if (!read_leb128(f, ft_bias.data(), l1)) {
            std::cerr << "info string nnue: bias fail\n";
            return false;
        }

        if (big) {
            std::size_t tsz = std::size_t(THREAT_DIM) * l1;
            ft_thr_w.resize(tsz);
            f.read(reinterpret_cast<char*>(ft_thr_w.data()), tsz);
            if (!f) {
                std::cerr << "info string nnue: threat weight fail\n";
                return false;
            }
            std::cerr << "info string nnue: threat weights loaded ("
                      << tsz / (1024 * 1024) << "MB)\n";
            ft_w.resize(std::size_t(PSQ_DIM) * l1);
            if (!read_leb128(f, ft_w.data(), std::size_t(PSQ_DIM) * l1)) {
                std::cerr << "info string nnue: PSQ weight fail\n";
                return false;
            }
            ft_thr_psqt.resize(std::size_t(THREAT_DIM) * PSQT_BUCKETS);
            ft_psqt.resize    (std::size_t(PSQ_DIM)    * PSQT_BUCKETS);
            if (!read_leb128_2(f,
                ft_thr_psqt.data(), std::size_t(THREAT_DIM) * PSQT_BUCKETS,
                ft_psqt.data(),     std::size_t(PSQ_DIM)    * PSQT_BUCKETS)) {
                std::cerr << "info string nnue: PSQT fail\n";
                return false;
            }
        } else {
            ft_w.resize(std::size_t(PSQ_DIM) * l1);
            if (!read_leb128(f, ft_w.data(), std::size_t(PSQ_DIM) * l1)) {
                std::cerr << "info string nnue: weight fail\n";
                return false;
            }
            ft_psqt.resize(std::size_t(PSQ_DIM) * PSQT_BUCKETS);
            if (!read_leb128(f, ft_psqt.data(), std::size_t(PSQ_DIM) * PSQT_BUCKETS)) {
                std::cerr << "info string nnue: PSQT fail\n";
                return false;
            }
            // Small net: ×2 scale on FT weights / biases (matches SF18).
            for (auto& v : ft_bias) v *= 2;
            for (auto& v : ft_w)    v *= 2;
        }
        std::cerr << "info string nnue: FT loaded\n";

        // FC layer stacks (one per bucket).
        int fc0o  = l2 + 1;
        int fc0pi = pad32(l1);
        int fc1i  = 2 * l2;
        int fc1pi = pad32(fc1i);
        int fc2pi = pad32(l3);
        for (int bk = 0; bk < LAYER_STACKS; ++bk) {
            std::uint32_t sh;
            f.read(reinterpret_cast<char*>(&sh), 4);
            if (!f) return false;
            fc[bk][0].out = fc0o; fc[bk][0].pi = fc0pi;
            fc[bk][0].bias.resize(fc0o);
            f.read(reinterpret_cast<char*>(fc[bk][0].bias.data()), fc0o * 4);
            fc[bk][0].weight.resize(fc0o * fc0pi);
            f.read(reinterpret_cast<char*>(fc[bk][0].weight.data()), fc0o * fc0pi);
            fc[bk][1].out = l3; fc[bk][1].pi = fc1pi;
            fc[bk][1].bias.resize(l3);
            f.read(reinterpret_cast<char*>(fc[bk][1].bias.data()), l3 * 4);
            fc[bk][1].weight.resize(l3 * fc1pi);
            f.read(reinterpret_cast<char*>(fc[bk][1].weight.data()), l3 * fc1pi);
            fc[bk][2].out = 1; fc[bk][2].pi = fc2pi;
            fc[bk][2].bias.resize(1);
            f.read(reinterpret_cast<char*>(fc[bk][2].bias.data()), 4);
            fc[bk][2].weight.resize(fc2pi);
            f.read(reinterpret_cast<char*>(fc[bk][2].weight.data()), fc2pi);
            if (!f) {
                std::cerr << "info string nnue: FC fail bucket " << bk << '\n';
                return false;
            }
        }

        loaded = true;
        std::cerr << "info string nnue evaluation using " << path
                  << " (" << fsz / (1024 * 1024) << "MiB, ("
                  << (big ? PSQ_DIM + THREAT_DIM : PSQ_DIM) << ", "
                  << l1 << ", " << l2 << ", " << l3 << ", 1))\n";
        return true;
    }
};

Network g_big, g_small;

// ─────────────────────────────────────────────────────────────────────────
// Lazy incremental update of the per-StateInfo accumulator.
//
// Walks back through the StateInfo chain looking for the nearest ancestor
// whose accumulator is already valid for color c. If none exists, or if a
// king move for c appears anywhere in the chain (which would invalidate the
// per-piece feature indices the ancestors were computed against), trigger a
// full refresh from the current position. Otherwise copy the ancestor's
// accumulator forward and apply each generation's DirtyPiece deltas.
//
// `big` selects which network's accumulator slot we're servicing.
//
// References: Stockfish nnue_accumulator.cpp::evaluate and the chess-
// programming wiki "Incrementally Updated Accumulator" article.
// ─────────────────────────────────────────────────────────────────────────
void make_valid(const Position& pos, Color c, bool big) {
    StateInfo* st = pos.state();
    if (big ? st->nnue.valid_big[c] : st->nnue.valid_small[c]) return;

    Network& net = big ? g_big : g_small;
    if (!net.loaded) return;

    // Walk backwards collecting StateInfos whose accumulator we'll need to
    // forward-apply. Stop when we either find a valid ancestor or notice a
    // king move for c (which forces a full refresh).
    // Bound chosen to match the search stack (search.cpp uses Stack[MAX_PLY+10]).
    constexpr int CHAIN_MAX = MAX_PLY + 10;
    StateInfo* chain[CHAIN_MAX];
    int n = 0;
    StateInfo* cur = st;
    bool need_refresh = false;
    while (true) {
        // Check king move FOR THIS COLOR in this StateInfo's transition.
        for (int i = 0; i < cur->dirtyCount; ++i) {
            const DirtyPiece& d = cur->dirtyPiece[i];
            if (type_of(d.pc) == KING && color_of(d.pc) == c) {
                need_refresh = true;
                break;
            }
        }
        if (need_refresh) break;
        chain[n++] = cur;
        if (n >= CHAIN_MAX) { need_refresh = true; break; }
        if (!cur->previous) { need_refresh = true; break; }   // root
        cur = cur->previous;
        if (big ? cur->nnue.valid_big[c] : cur->nnue.valid_small[c]) break;
    }

    std::int16_t* acc;
    std::int32_t* psqt;
    if (big) {
        acc  = st->nnue.big_acc[c];
        psqt = st->nnue.big_psqt[c];
    } else {
        acc  = st->nnue.small_acc[c];
        psqt = st->nnue.small_psqt[c];
    }

    if (need_refresh) {
        // King moved (or we ran out of room) — refresh through Finny cache:
        // diffs the cached piece set for this (king-bucket-orient, color) key
        // and applies only the changed-piece sub/add. ~4-16x faster than
        // walking all 32 pieces in the typical case.
        net.cached_refresh(pos, acc, psqt, int(c));
    } else {
        // Copy from the valid ancestor `cur`, then apply chain deltas oldest-
        // first. (chain[n-1] is the OLDEST entry whose acc we still need to
        // apply; chain[0] is the current StateInfo.)
        const std::int16_t* src_acc;
        const std::int32_t* src_psqt;
        if (big) {
            src_acc  = cur->nnue.big_acc[c];
            src_psqt = cur->nnue.big_psqt[c];
        } else {
            src_acc  = cur->nnue.small_acc[c];
            src_psqt = cur->nnue.small_psqt[c];
        }
        std::memcpy(acc,  src_acc,  net.l1 * sizeof(std::int16_t));
        std::memcpy(psqt, src_psqt, PSQT_BUCKETS * sizeof(std::int32_t));

        int ksq = int(pos.square<KING>(c));
        for (int i = n - 1; i >= 0; --i) {
            for (int j = 0; j < chain[i]->dirtyCount; ++j)
                net.apply_dirty(chain[i]->dirtyPiece[j], int(c), ksq, acc, psqt);
        }
    }

    if (big) st->nnue.valid_big[c]   = true;
    else     st->nnue.valid_small[c] = true;

#ifdef NNUE_VALIDATE
    // Cross-check: re-compute the same accumulator from scratch and compare.
    // Any divergence means a bug in the incremental update (DirtyPiece tracking,
    // king-move detection, chain ordering, etc.). Aborts on first mismatch.
    {
        alignas(64) std::int16_t ref_acc[1024];
        alignas(64) std::int32_t ref_psqt[PSQT_BUCKETS];
        net.refresh_psq(pos, ref_acc, ref_psqt, int(c));
        for (int i = 0; i < net.l1; ++i) {
            if (acc[i] != ref_acc[i]) {
                std::cerr << "info string NNUE_VALIDATE FAIL acc[" << i
                          << "] inc=" << acc[i] << " ref=" << ref_acc[i]
                          << " color=" << int(c) << " big=" << big
                          << " fen=" << pos.fen() << '\n';
                std::abort();
            }
        }
        for (int i = 0; i < PSQT_BUCKETS; ++i) {
            if (psqt[i] != ref_psqt[i]) {
                std::cerr << "info string NNUE_VALIDATE FAIL psqt[" << i
                          << "] inc=" << psqt[i] << " ref=" << ref_psqt[i]
                          << " color=" << int(c) << " big=" << big
                          << " fen=" << pos.fen() << '\n';
                std::abort();
            }
        }
    }
#endif
}

// SF18 small-net selection: switch to small when material is lopsided.
// Mirrors SF's `simple_eval()` (src/evaluate.cpp:38-42):
//     pawn_value*(pawns_us - pawns_them) + non_pawn_material_us - non_pawn_material_them
// SF's threshold is 962 cp in its INTERNAL piece-value scale (PawnValue=208,
// non_pawn_material from incrementally-tracked StateInfo). Hypersion's
// PieceValue array uses SF-style magnitudes too (P=126, N=781, B=825, R=1276,
// Q=2538) — see src/position.cpp:20. Earlier this function used classical
// 100/305/333/500/900 against the 962 threshold, which was a magnitude
// mismatch and made the small-net trigger fire at the wrong material gaps.
bool use_small(const Position& pos) {
    Color c = pos.side_to_move();
    // PieceValue table is file-local in position.cpp; PieceValueMG in
    // evaluate.h has the same pawn entry. Use that + the incrementally-tracked
    // non_pawn_material() the StateInfo already maintains.
    constexpr int PAWN_VAL = 126;   // = PieceValueMG[PAWN]
    int se = PAWN_VAL * (popcount(pos.pieces( c, PAWN))
                       - popcount(pos.pieces(~c, PAWN)))
           + int(pos.non_pawn_material(c)) - int(pos.non_pawn_material(~c));
    // Threshold raised from SF18's default 962 to 1500. The big net is
    // more accurate at finding endgame conversion plans (passed-pawn
    // races, K+R conversions, etc.) — exactly the situations a
    // user-reported bullet bug surfaces. The cost is ~5 % NPS for
    // those positions, but the eval improvement is worth it for the
    // accuracy in winning-but-conversion-required scenarios.
    //
    // SF18 chose 962 because at that scale, small net is "good enough"
    // to recognize the obvious winner. But the SF eval target is
    // win-rate, which doesn't penalize sub-optimal but still-winning
    // moves — exactly the moves that cause the bullet conversion
    // bug for human-time-pressure opponents.
    return std::abs(se) > 1500;
}

}  // namespace


// ─────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────
void init() {
    build_threat_tables();
}

bool load_big(const std::string& path) {
    build_threat_tables();
    return g_big.load(path, true);
}

bool load_small(const std::string& path) {
    return g_small.load(path, false);
}

bool is_loaded() { return g_big.loaded || g_small.loaded; }

void unload() {
    g_big = Network{};
    g_small = Network{};
    g_finny.invalidate();
}

void new_game() {
    // Called on UCI `ucinewgame`: stale Finny entries from a different
    // game's piece distribution are still correct (diff is a no-op when
    // pieces match), but invalidating frees the cost of a few extra
    // sub/adds on the first refresh of the new game.
    g_finny.invalidate();
}

Value evaluate(const Position& pos) {
    // SF18-style net selection (src/evaluate.cpp:43-70):
    //   1) When material is lopsided ( use_small() ), try the cheap small net.
    //   2) If the small net says |v| < SMALL_FALLBACK_TH, the position is
    //      tactically balanced despite the material gap — recompute with the
    //      more accurate big net.
    //   3) Otherwise default to the big net.
    // SF uses 277 cp in raw NNUE units. Our values are post-/NNUE_DIVISOR.
    constexpr int SMALL_FALLBACK_TH = 277 / NNUE_DIVISOR;
    static_assert(SMALL_FALLBACK_TH >= 90, "small-net fallback threshold sanity");

    bool useSmall = use_small(pos);
    if (useSmall && g_small.loaded) {
        make_valid(pos, WHITE, false);
        make_valid(pos, BLACK, false);
        int v = g_small.forward(pos);
        if (g_big.loaded && std::abs(v) < SMALL_FALLBACK_TH) {
            make_valid(pos, WHITE, true);
            make_valid(pos, BLACK, true);
            return Value(g_big.forward(pos));
        }
        return Value(v);
    }
    if (g_big.loaded) {
        make_valid(pos, WHITE, true);
        make_valid(pos, BLACK, true);
        return Value(g_big.forward(pos));
    }
    if (g_small.loaded) {
        make_valid(pos, WHITE, false);
        make_valid(pos, BLACK, false);
        return Value(g_small.forward(pos));
    }
    return VALUE_ZERO;
}

}  // namespace hypersion::NNUE
