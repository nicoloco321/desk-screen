# Claude Code Usage Display

An ESP32 desk display for your Claude Code rate limits. Supported panels
range from the little **Waveshare ESP32-C6-LCD-1.47** (172x320 SPI, the
original build) up to the **Waveshare ESP32-P4 + 8.8" 480x1920 MIPI-DSI bar
display** (see [Hardware](#hardware)).

- **Usage screen** — Clawd, the Claude Code mascot (pixel art), an animated
  thinking spinner whenever Claude is actively working, and two live bars:
  your **5-hour limit** and **weekly limit**, with real utilization
  percentages and reset times (green < 50%, yellow < 80%, red above)
- **Onboard RGB LED** — blinks green whenever Claude is thinking (Waveshare C6 board)
- **Spotify mode** — flip the same screen to a Spotify **now playing** view
  (album art, track, artist, live progress bar). Switch from inside Claude
  Code with the **`/switch`** slash command, or with a one-line `curl`; the
  choice survives power cycles
- **Split mode** (wide panels) — Claude usage on the left half, Spotify on
  the right, at the same time
- **Desktop emulator** — run the exact same UI in a window on your Mac/PC
  while you wait for hardware (see [emulator/](emulator/))

Narrow boards stack everything vertically; the 1920x480 bar panel puts the
mascot + spinner on the left and the bars on the right:

```
+------------------+   172x320 portrait        1920x480 landscape
|  [Clawd] Claude  |   +--------------+----------------------------------+
|         Code     |   |    [Clawd]   |  5-HOUR                      33% |
|         usage..  |   |              |  resets 4:19 AM                  |
|                  |   |  Claude Code |  [############                 ] |
|    ( spinner )   |   | usage monitor|                                  |
|    working...    |   |              |  WEEKLY                      62% |
|------------------|   |  ( spinner ) |  resets Mon 1 PM                 |
|  5-HOUR     33%  |   |  working...  |  [####################         ] |
|  resets 4:19 AM  |   +--------------+----------------------------------+
|  [#######      ] |
|                  |   Spotify full screen: album art left, track /
|  WEEKLY     14%  |   artist / album / progress right. Split screen:
|  resets Mon 1 PM |   usage on the left half, Spotify on the right.
|  [###          ] |
|  usage ok  ...   |
+------------------+
```

## How it works

The ESP32 talks to Anthropic directly — **no companion server required**:

1. **Usage** — the device fetches your real 5-hour / weekly utilization straight
   from Anthropic's OAuth usage API over HTTPS, using a **dedicated login** you
   mint once with `server/device_login.py`. It's a separate authorization from
   your everyday Claude Code session, so the device refreshing its token never
   logs you out anywhere. The access token is short-lived, so the device
   refreshes it itself and remembers the rotated token in flash (NVS). It also
   syncs time over NTP to render reset times locally.
   (Note: `claude setup-token` does **not** work here — those tokens lack the
   `user:profile` scope the usage endpoint requires.)
2. **"Claude is thinking"** — the usage API has no real-time activity signal, so
   the device listens for tiny HTTP *beacons* on `/thinking/on` and
   `/thinking/off`. There are two ways to send them (both optional — skip them
   and you just lose the LED/spinner):
   - **Claude Code hooks (precise, recommended)** — fire `on` the instant you
     submit a prompt and `off` the instant Claude finishes, straight from Claude
     Code's lifecycle events. No timeout, no guessing. See
     [Precise thinking via hooks](#3-optional-light-up-while-claude-is-thinking).
   - **`server/beacon.py` (no config)** — a dependency-free watcher that infers
     activity from `~/.claude/projects` session-log writes. Less precise (a small
     trailing delay) but needs zero setup. Run it on any machine; the display
     reacts whenever *any* of them is active.

3. **Screen modes** — the same tiny HTTP server switches what's on screen:
   `POST /mode/usage`, `/mode/spotify`, `/mode/split` (wide panels) or
   `/mode/toggle` (and `GET /mode` to
   ask). In Spotify mode the device polls Spotify's currently-playing endpoint
   with its own login (minted once with `server/spotify_login.py`, PKCE — no
   client secret on the device) and ticks the progress bar locally between
   polls. The repo ships a [`/switch` Claude Code command](.claude/commands/switch.md)
   that drives this. See [Spotify now-playing mode](#4-optional-spotify-now-playing-mode).

> The old `server/claude_usage_server.py` (a Mac-side usage proxy) is no longer
> needed and is kept only as a fallback. The display is self-contained now.

## Hardware

Four build environments are defined in
[platformio.ini](firmware/platformio.ini); pick the one for your board.

| Env | Board | Panel | Notes |
|---|---|---|---|
| `waveshare-c6-lcd-147` | Waveshare ESP32-C6-LCD-1.47 | 172x320 ST7789 | **default**, portrait |
| `waveshare-p4-88` | Waveshare ESP32-P4 (e.g. P4-NANO) | 8.8" 480x1920 DSI LCD | wide landscape, split mode |
| `esp32-3248s035` | Sunton ESP32-3248S035 ("Cheap Yellow Display") | 480x320 ST7796 | landscape |
| `ili9488` | bare ESP32 + separate ILI9488 module | 480x320 ILI9488 | landscape, wire it yourself |

### Waveshare ESP32-P4 + 8.8" DSI bar display

The `waveshare-p4-88` env drives Waveshare's
[8.8inch DSI LCD](https://www.waveshare.com/wiki/8.8inch_DSI_LCD) (480x1920
IPS, 2-lane MIPI-DSI, the Raspberry Pi display) from a Waveshare ESP32-P4
board such as the
[ESP32-P4-NANO](https://www.waveshare.com/wiki/ESP32-P4-NANO), shown in
landscape as 1920x480. Notes:

- **Connection** — the display plugs into the board's MIPI-DSI connector
  (DSI-Cable / 15-pin FFC, same as on a Raspberry Pi) and is powered over the
  same cable. The panel self-initializes; the firmware supplies the DSI video
  timing (from the Raspberry Pi kernel driver) and pokes the display's
  onboard I2C controller (addr `0x45`, SDA 7 / SCL 8 on the P4-NANO) for
  panel power and backlight — all in
  [display.h](firmware/src/display.h).
- **Wi-Fi** — the ESP32-P4 has no radio; Wi-Fi 6 runs on the board's
  ESP32-C6 companion over SDIO (esp-hosted). Arduino-ESP32 3.3.x handles
  this transparently, and like the C6 board this needs the
  [pioarduino](https://github.com/pioarduino/platform-espressif32) platform
  fork (the env downloads it on first build, a few hundred MB).
- **Graphics** — LovyanGFX ≥ 1.2.25, which has native ESP32-P4 MIPI-DSI
  support (`Bus_DSI`/`Panel_DSI`). The 1920x480 framebuffer lives in the
  P4's 32 MB PSRAM.
- **Chip revision** — the env's board file targets production rev 3.0x
  chips (current P4-NANO stock). For an early engineering-sample chip,
  change `board` to `esp32-p4-evboard`.
- **Untested on hardware yet**: this env compiles and the layout is
  verified in the [emulator](emulator/), but it was written before the
  panel arrived. If the screen stays dark or shows wrong colours, the
  prime suspects (link colour format, lane rate) are commented in
  [display.h](firmware/src/display.h).

The wide build also unlocks **split mode** (`/mode/split` or
`/switch split`): usage on the left half, Spotify on the right.

The **Waveshare ESP32-C6-LCD-1.47** has the 172x320 ST7789 panel wired to the
ESP32-C6 on-board: MOSI 6, SCLK 7, CS 14, DC 15, RST 21, backlight 22 (no
MISO). Those pins (plus colour order / inversion / the 34px column offset) live
in [display.h](firmware/src/display.h). If you get a blank or wrong-coloured
screen, cross-check them against the
[board wiki](https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47) — wrong pins are
the usual cause.

> **Why the C6 is special:** it's RISC-V, so it needs two things the older
> boards don't.
> - **Toolchain:** Arduino-ESP32 core 3.x, which the stock PlatformIO
>   `espressif32` platform doesn't ship — the env uses the
>   [pioarduino](https://github.com/pioarduino/platform-espressif32) fork
>   instead (the first build downloads it, a few hundred MB). If the pinned
>   release URL 404s, bump it to the newest tag from the pioarduino releases.
> - **Graphics library:** TFT_eSPI has no working C6 driver (it miscompiles the
>   RISC-V SPI/GPIO as a classic Xtensa ESP32), so the C6 env uses **LovyanGFX**.
>   The 480x320 envs stay on TFT_eSPI. [display.h](firmware/src/display.h)
>   typedef-switches between the two, so the rest of the firmware is shared.

The 480x320 boards' pins and driver live in
[platformio.ini](firmware/platformio.ini) (per-env `build_flags`). Screen size
and rotation for each board are in [display.h](firmware/src/display.h), and the
layout scales from those.

## Setup

### 1. Mint a login for the device

```sh
python3 server/device_login.py
```

Open the URL it prints, approve access, and paste the code back. It runs the
same OAuth flow Claude Code uses (requesting the `user:profile` scope the usage
endpoint needs), checks the new token against the usage API, and prints a
`DEVICE_REFRESH_TOKEN` line to paste into config.h. This is a separate login
from your everyday Claude Code session, so it won't disturb it.

> Don't use `claude setup-token` — those tokens are inference-only and lack
> `user:profile`, so the usage endpoint returns `403`.

### 2. Configure and flash the ESP32

Edit [config.h](firmware/src/config.h):

- `WIFI_SSID` / `WIFI_PASS` — your 2.4 GHz network (ESP32 has no 5 GHz)
- `DEVICE_REFRESH_TOKEN` — the value printed by step 1
- `TIMEZONE` — your POSIX TZ string (examples are in the file) for reset times

Then plug in the board and:

```sh
cd firmware
pio run -t upload                          # Waveshare ESP32-C6-LCD-1.47 (default)
pio run -e waveshare-p4-88 -t upload       # Waveshare ESP32-P4 + 8.8" DSI panel
pio run -e esp32-3248s035 -t upload        # Sunton Cheap Yellow Display
pio run -e ili9488 -t upload               # generic ESP32 + ILI9488 module
```

All environments are verified to compile. The first build of the pioarduino
envs (`waveshare-c6-lcd-147`, `waveshare-p4-88`) is slow because it downloads
the toolchain. On boot the display
shows its address (e.g. `claude-display.local  192.168.1.42`) on the status line.

The bars should fill in within ~30 s. If they show `--`, see Troubleshooting.

### 3. (Optional) Light up while Claude is thinking

Pick **one** of these per machine. Hooks are precise (exact start/stop); the
beacon needs zero config but lags a little.

#### Option A — Claude Code hooks (recommended)

Claude Code fires lifecycle [hooks](https://docs.claude.com/en/docs/claude-code/hooks)
you can hang a command on. We use three: `UserPromptSubmit` → **on**, `Stop` →
**off**, and `PreToolUse` → **on** (a keep-alive for long turns). Merge
[`server/claude-hooks.example.json`](server/claude-hooks.example.json) into your
`~/.claude/settings.json` (user-level, so it applies to every project), replacing
`claude-display.local` with the device's IP if mDNS doesn't resolve. Each hook is
just:

```sh
curl -sf -m 1 -X POST http://claude-display.local:8080/thinking/on  >/dev/null 2>&1 || true
curl -sf -m 1 -X POST http://claude-display.local:8080/thinking/off >/dev/null 2>&1 || true
```

The LED turns on the moment you hit enter and off the moment Claude stops — no
trailing delay. (`-m 1` keeps a hook from ever blocking Claude Code; the device's
5-min `BEACON_TTL_MS` is just a backstop if a `Stop` hook is ever missed.)

> **Use the device's IP, not `claude-display.local`, in hooks.** With the 1s
> `curl` timeout, `.local` mDNS names often don't resolve in time (macOS `ping`
> resolves them, but `curl` frequently can't), so the hook silently no-ops. Get
> the IP from the device's status line, or run
> [`python3 server/find_display.py`](server/find_display.py) — it resolves the
> device and prints the IP plus the exact hook commands ready to paste.
>
> Then **pin that IP** so it doesn't change out from under the hooks: add a
> **DHCP reservation** for the device in your router (map the display's MAC
> address to a fixed IP). Otherwise a new lease can reassign the address and the
> hooks quietly stop working. Hooks also load at **session start**, so restart
> Claude Code after editing `settings.json`.

#### Option B — the beacon watcher (no config)

```sh
python3 server/beacon.py            # auto-finds the display at claude-display.local
python3 server/beacon.py --host 192.168.1.42   # or point at its IP
```

No dependencies — Python 3 stdlib only, macOS/Windows/Linux. It infers activity
from session-log writes, sends `/thinking/on` while busy and `/thinking/off` when
idle. Simpler, but a few seconds less precise than hooks.

> On **Windows**, the `claude-display.local` name needs Apple Bonjour installed.
> If it can't resolve, use the device's IP (in the hook URLs, or `--host`).

To keep the beacon running across reboots on macOS, add a LaunchAgent:

```sh
cat > ~/Library/LaunchAgents/com.nicoloco.claude-beacon.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.nicoloco.claude-beacon</string>
  <key>ProgramArguments</key><array>
    <string>/usr/bin/python3</string>
    <string>$HOME/Documents/code-usage/server/beacon.py</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
</dict></plist>
EOF
launchctl load ~/Library/LaunchAgents/com.nicoloco.claude-beacon.plist
```

On **Windows**, drop a shortcut to `pythonw beacon.py` in
`shell:startup`, or register it with Task Scheduler at logon.

### 4. (Optional) Spotify now-playing mode

The display can flip to a "now playing" screen. It needs its own Spotify
authorization (any free account works):

1. Create an app at <https://developer.spotify.com/dashboard> (any
   name/description). In the app's settings add this **exact Redirect URI**:
   `http://127.0.0.1:8898/callback`, and tick **Web API**. You never need the
   client secret — the login uses PKCE.
2. Mint the token (paste the app's **Client ID** when prompted):

   ```sh
   python3 server/spotify_login.py
   ```

3. Paste the two printed `#define` lines (`SPOTIFY_CLIENT_ID`,
   `SPOTIFY_REFRESH_TOKEN`) into `firmware/src/config.h` and reflash.

Switch screens any time (the device remembers the mode across power cycles,
and the thinking LED keeps working in every mode):

```sh
curl -X POST http://claude-display.local:8080/mode/spotify   # now playing
curl -X POST http://claude-display.local:8080/mode/usage     # back to usage
curl -X POST http://claude-display.local:8080/mode/split     # both (wide panels)
curl -X POST http://claude-display.local:8080/mode/toggle    # cycle
curl      http://claude-display.local:8080/mode              # ask
```

`/mode/toggle` cycles usage → spotify → split → usage on wide panels, and
just flips usage ↔ spotify on the small ones (where `/mode/split` answers
409).

#### The `/switch` command in Claude Code

The repo ships a slash command at
[.claude/commands/switch.md](.claude/commands/switch.md) — inside this repo,
just type `/switch`. To use it from **any** project, copy it to your user
commands folder:

```sh
mkdir -p ~/.claude/commands && cp .claude/commands/switch.md ~/.claude/commands/
```

Then `/switch spotify`, `/switch usage`, `/switch split`, `/switch toggle` —
or plain
`/switch` to be asked which one you want. (Claude Code discovers new commands
at session start, so restart it once after copying.)

## Desktop emulator

Don't have the screen yet (or don't want to reflash to try a layout tweak)?
[emulator/](emulator/) builds the exact firmware renderer
([firmware/src/ui.cpp](firmware/src/ui.cpp)) into a 1920x480 desktop window
with canned data — same library (LovyanGFX), same fonts, same pixels the
panel will show:

```sh
brew install sdl2 cmake          # once (macOS; on Linux: apt install libsdl2-dev cmake)
cd emulator
cmake -B build && cmake --build build -j
./build/desk-screen-emulator --mode split
```

Click the window to cycle usage → spotify → split, or type commands on
stdin (`mode spotify`, `think off`, `next`, `pause`,
`usage 85 40`, `shot layout.bmp`, `quit`). `--shot file.bmp` takes a
screenshot after two seconds and exits, which is handy for eyeballing layout
changes. `emulator/art.jpg` stands in for album art (`--art yours.jpg` to
swap it).

## Customizing

All layout geometry and drawing lives in [ui.cpp](firmware/src/ui.cpp) (the
`LAYOUT_WIDE` half is the 1920x480 layouts, the other half the small
screens); check tweaks instantly in the [emulator](#desktop-emulator).

- **Mascot** — pixel grid in [mascot.h](firmware/src/mascot.h); edit the
  array, any size works (adjust the scale passed in the `drawStaticUI` /
  `drawClaudeHeader` calls in ui.cpp).
- **Thinking animation** — a frame-drawn rotating starburst in `drawSpinner`
  ([ui.cpp](firmware/src/ui.cpp)). Frame-based drawing looks crisper than
  decoding an actual GIF on-device, but if you want a real GIF, the
  `bitbank2/AnimatedGIF` library works well with TFT_eSPI.
- **Screen upside down?** Change `SCREEN_ROTATION` in
  [display.h](firmware/src/display.h) (C6: `0`↔`2`; landscape boards: `1`↔`3`).
- **Colours wrong?** On the C6, toggle `cfg.rgb_order` (red/blue swapped) or
  `cfg.invert` (photo-negative) in [display.h](firmware/src/display.h); on the
  480x320 boards, toggle `-DTFT_RGB_ORDER=TFT_BGR` in `build_flags`.
- **Poll rate / thresholds** — `USAGE_POLL_MS` in config.h; bar colors in
  `barColor()`; "working" detection window is `ACTIVE_WINDOW_SECS` in
  `beacon.py`, and how long the LED keeps blinking after the last beacon is
  `BEACON_TTL_MS` in config.h.
- **Spotify poll rate** — `SPOTIFY_POLL_MS` in config.h paces how fast track
  changes/seeks show up; the progress bar animates locally between polls
  either way. The Spotify screen layout lives in the `SP_*` / `WS_*` /
  `SPL_*` constants in [ui.cpp](firmware/src/ui.cpp).
- **RGB LED** — pin is `RGB_LED_PIN` in config.h (GPIO8 on the Waveshare C6,
  `-1` to disable); `RGB_LED_SWAP_RG` fixes boards that show the wrong colour.
  The green "breathing" effect (brightness, speed) lives in `updateLed()` /
  `RGB_LED_MAX` / `LED_BREATHE_MS` in main.cpp.

## Troubleshooting

| Symptom | Fix |
|---|---|
| White / blank screen | Wrong driver for your panel — try the other env, check `TFT_BL` pin |
| Bars show `--` | No successful fetch yet; check the status line and Wi-Fi |
| "auth failed - run device_login.py" | The refresh token was rejected (revoked, or NVS was wiped and config.h's token is stale). Re-run `python3 server/device_login.py` and update `DEVICE_REFRESH_TOKEN` in config.h |
| "rate limited, retry in …s" | Throttled on the usage endpoint. The device backs off automatically (honors `Retry-After`) and clears itself. Don't lower `USAGE_POLL_MS` much, and avoid polling the same token from elsewhere |
| "usage fetch failed" | Wi-Fi/DNS issue, or Anthropic unreachable; the device keeps retrying |
| Reset times look wrong | Set the correct `TIMEZONE` in config.h; they're blank until NTP syncs (~few s) |
| LED/spinner never moves | Run `server/beacon.py` on the busy machine; check it prints `blinking`, not `could not reach …` |
| Beacon can't find device | Use `--host <IP shown on the display>` (Windows needs Bonjour for `.local`) |
| `/mode/spotify` answers 409 / "spotify not set up" | Spotify isn't configured: run `python3 server/spotify_login.py`, paste both `#define`s into config.h, reflash |
| "spotify auth failed" | Refresh token revoked or wrong client id — re-run `spotify_login.py`. A persistent 403 usually means your account isn't added to the Spotify app (Dashboard → your app → User Management) |
| "nothing playing" but music is on | Spotify only reports an *active* device; start playback from any Spotify app and it appears within one poll (~5 s) |
