# Security Policy

## Supported versions

Only the **latest release** receives security fixes. As of 2026-05-14
that is **Hypersion 3.1**.

| Version | Supported |
|---------|-----------|
| 3.1     | ✅        |
| 3.0     | ❌ (superseded by 3.1) |
| ≤ 2.x   | ❌        |

## Reporting a vulnerability

Hypersion is a chess engine — the realistic attack surface is small
(UCI parser, file readers for NNUE / Polyglot books / Syzygy
tablebases). If you find an issue regardless, please report it via
**GitHub's Private Vulnerability Reporting**:

1. Visit https://github.com/RenCopp/Hypersion/security
2. Click **"Report a vulnerability"**
3. Describe the issue, ideally with a reproduction case

This is private — only the repository maintainer sees the report
until a fix is published.

### What counts as a security issue

- Malformed `.nnue` / `.bin` / Syzygy `.rtbz` file that triggers a
  crash, infinite loop, or out-of-bounds read in Hypersion
- UCI input that causes the engine to consume unbounded memory
- Memory corruption in any release build that ships from this repo
- Tournament-cheating exploits (e.g., abusing `setoption` to leak
  side information)

### What is NOT a security issue

- "The engine plays a weaker move than Stockfish" — that's a tuning
  question, not a security one. Open a regular issue.
- "I changed the source and now it crashes" — local-build
  modifications are out of scope.
- "Lichess opponent says my bot did X" — open an issue, not a
  security report.

## Disclosure

After a fix lands, the advisory will be published via GitHub's
Security Advisories tab with credit to the reporter (unless they
prefer anonymity).
