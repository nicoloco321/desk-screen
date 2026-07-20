#pragma once

// Rendering layer. Everything that touches the screen lives behind this API;
// main.cpp (device) and emulator/main.cpp (desktop) only feed it data.
//
// Two layout families share the API, chosen at compile time in display.h:
//   - narrow (portrait 172x320 / landscape 480x320): the original stacked
//     layout, one screen per mode
//   - LAYOUT_WIDE (1920x480 bar panels): usage = mascot left / bars right,
//     spotify = album art left / details right, plus MODE_SPLIT with usage on
//     the left half and Spotify on the right

#include <stddef.h>

#include "display.h"
#include "ui_data.h"

// The one display device, defined by each frontend (main.cpp / emulator).
extern DisplayTFT tft;

// ---- palette (RGB565), shared with the status messages in main.cpp ----
static const uint16_t COL_BG     = 0x1082;  // #121212 near-black
static const uint16_t COL_CARD   = 0x2945;  // dark gray track
static const uint16_t COL_ORANGE = 0xDBAA;  // #D97757 Claude orange
static const uint16_t COL_TEXT   = 0xEF7D;  // #ECECEC
static const uint16_t COL_DIM    = 0x7BCF;  // mid gray
static const uint16_t COL_GREEN  = 0x3DCA;
static const uint16_t COL_YELLOW = 0xDD08;
static const uint16_t COL_RED    = 0xE289;
static const uint16_t COL_SPOTIFY = 0x1DCA; // #1DB954 Spotify green

// Call once after tft.init(): allocates the spinner sprite.
void uiInit();

// Switch screens / repaint the current one from the cached data.
void uiSetMode(DisplayMode m);
// Record the boot mode without painting (called before the display is up).
void uiSetModeInitial(DisplayMode m);
DisplayMode uiMode();
void uiFullRepaint();

// One shared status line (bottom of the screen); both the usage and the
// Spotify pollers write here, same as the original single-screen layout.
void uiStatus(const char *msg, uint16_t color);

// Data updates. Each repaints only if the numbers changed and the region is
// visible in the current mode.
void uiSetUsage(const Usage &u);
void uiSetNowPlaying(const NowPlaying &np, unsigned long fetchedAtMs);

// Cheap per-loop tick: spinner animation + working/idle word while thinking,
// and the locally-estimated Spotify progress bar (1 Hz).
void uiTick(unsigned long now, bool thinking);

// Album art handshake: the renderer says which art it wants (once per track /
// mode change), the caller fetches the jpeg bytes and pushes them back.
// uiArtWanted copies the URL and marks it attempted, so a failed fetch does
// not retry until the track or mode changes.
bool uiArtWanted(char *url, size_t urlCap);
int  uiArtSizePx();  // on-screen art edge in px (target for variant picking)
void uiPushArt(const uint8_t *jpg, size_t len, long nativeW);

// ---- small string/text helpers shared with main.cpp ----

// The built-in fonts are ASCII-only, so drop other UTF-8 bytes instead of
// rendering them as garbage glyphs ("Beyoncé" -> "Beyonc").
void asciiCopy(char *dst, size_t n, const char *src);
