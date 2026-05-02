# Hypersion — Lichess Profile Content

This file holds copy-pasteable content for the bot's lichess account.

---

## Bio (paste into Profile → Bio, 400-char limit)

**Short version (~280 chars, leaves room for links):**

```
Hypersion — UCI chess engine, NNUE eval (SF18 SFNNv10).
Auto-matches your rating: I cap my strength to your level.
Strong opponents → full power; weak / new → I tone it down.
Bots & 2500+ get max effort. Casual challenges welcome.
```

**Longer version (~390 chars, fills the field):**

```
Hypersion — open chess engine. NNUE-based eval (Stockfish 18 SFNNv10
architecture, 104 MB big net + 3 MB small net), tuned classical search
on top. I auto-match my strength to your rating: weak players get a
gentler version of me, strong (2500+) and bots get full power. Casual
games preferred. New / provisional ratings → I assume ~1700.
```

Pick one, paste into <https://lichess.org/account/profile> → "Bio".

---

## Useful profile fields

| Field | Suggested value |
|---|---|
| First name | (your real name or pseudonym) |
| Last name | leave blank for a bot |
| Country | wherever you host the bot |
| Location | "behind a Python script" works |
| Flair | a knight or rook icon fits |
| Links | github repo if public |

---

## Welcome message (in-game chat)

Already wired in `lichess_bot.py`:
- On game start: `Hi, glhf!`
- On win: `gg, well played!`
- On loss: `gg, nice game!`
- On draw: `gg`

Disable per-run with `--no-chat`.

---

## Account upgrade (one-time, before first run)

Bot accounts must be upgraded via API. From a shell (replace `<TOKEN>`):

```bash
curl -d '' https://lichess.org/api/bot/account/upgrade \
     -H "Authorization: Bearer <TOKEN>"
```

Caveat: an account with any rated games **cannot** be upgraded. Use a
fresh account or one that has only played casual games.

---

## Recommended account settings

In <https://lichess.org/account/preferences>:

| Setting | Value | Why |
|---|---|---|
| Pre-move | enabled | bot doesn't pre-move but the option helps for testing |
| Allow takebacks | **never** | bots ignore takeback offers; "never" prevents reminders |
| Auto-promote to queen | enabled | bot queens; prevents draw-trolls |
| Game chat | enabled | so the hello/gg messages land |

In <https://lichess.org/account/preferences/challenge>:

| Setting | Value |
|---|---|
| Accept challenges | "Only friends" while testing, "All" when ready |
| Variants | Standard only (bot doesn't support variants) |

---

## Profile picture

Lichess auto-generates one from initials. To upload a custom image:
<https://lichess.org/account/profile> → "Edit profile picture".

A 256×256 PNG with the engine's logo or a knight silhouette works well.

Generate at <https://lichess.org/account/oauth/token/create>. Required
scope: `bot:play`. Save the token as `Hypersion.txt` (one line) in the
project root, next to `Hypersion.exe`.

---

## Discoverability

Once running, the bot appears at <https://lichess.org/player/bots>
sorted by rating. To climb the bots ladder, accept rated bot
challenges (`--accept-rated` flag). Casual games don't affect rating.

---

## API token

The bot expects the API token at `Hypersion.txt` in the project root, one level above this `lichess_bot/` folder (one line,
no quotes, no extra whitespace).

Generate at <https://lichess.org/account/oauth/token/create>. Required
scopes:
- `bot:play` — make moves
- `challenge:write` — accept/decline challenges (auto-included with bot:play)

Keep the token private — anyone with it can move your bot's pieces.
