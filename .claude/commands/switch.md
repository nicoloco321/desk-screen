---
description: Switch the desk display between Claude Code usage, Spotify now playing, and split screen
argument-hint: [usage | spotify | split | toggle]
allowed-tools: Bash(curl:*), Bash(python3:*)
---

Switch what the ESP32 desk display (the Claude usage monitor) is showing.

The device listens on `http://claude-display.local:8080`:

- `POST /mode/usage` — the Claude Code usage bars
- `POST /mode/spotify` — Spotify now playing
- `POST /mode/split` — both side by side (wide 1920x480 panels only)
- `POST /mode/toggle` — cycle usage → spotify → split → usage
- `GET /mode` — report the current mode without changing it

Do this:

1. Pick the target endpoint from the arguments: "$ARGUMENTS"
   - mentions usage / claude / limits → `/mode/usage`
   - mentions spotify / music / song / now playing → `/mode/spotify`
   - mentions split / both / half → `/mode/split`
   - mentions toggle / flip / cycle / other one → `/mode/toggle`
   - empty or ambiguous → ask with the AskUserQuestion tool: "What should the
     desk display show?" with options "Spotify now playing", "Claude Code
     usage" and "Split screen (both)".

2. Send it — the response body is the mode that is now active:

   ```sh
   curl -4 -sf -m 3 -X POST http://claude-display.local:8080/mode/<mode>
   ```

   The `-4` is required, not cosmetic. The device advertises only an A record
   over mDNS, so a dual-stack lookup of `claude-display.local` stalls on the
   AAAA query for ~5s before falling back — longer than the 3s budget, so the
   request times out even though the device is healthy and the name resolves.
   Forcing IPv4 skips that query and answers in ~40ms.

   If it still fails, find the device's IP with
   `python3 ~/Documents/desk-screen/server/find_display.py` — or use the IP
   shown on the display's own status line — and retry against
   `http://<ip>:8080`.

3. Tell the user what the display is showing now. If the device answered
   HTTP 409: either Spotify isn't configured on it yet — point them at the
   "Spotify now-playing mode" section of `~/Documents/desk-screen/README.md`
   (run `server/spotify_login.py`, paste the two defines into
   `firmware/src/config.h`, reflash) — or they asked for split mode on a
   small screen, which only exists on the wide 1920x480 panel.
