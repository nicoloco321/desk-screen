// All screen painting for the desk display, behind the small API in ui.h.
//
// Two layout families, chosen by LAYOUT_WIDE (display.h):
//  - narrow: the original stacked layout for 172x320 / 480x320 panels
//  - wide:   1920x480 bar panels (Waveshare 8.8" DSI + the SDL emulator),
//            with the mascot on the left / usage bars on the right, a
//            Spotify screen with album art left / details right, and a
//            split screen showing both at once.
//
// The renderer keeps its own copies of the last Usage / NowPlaying so a mode
// switch can repaint everything without refetching, and tracks what is
// already on screen (track signature, progress second, play state, art URL)
// so uiTick only touches pixels that changed.

#include "ui.h"
#include "mascot.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if !defined(ARDUINO)
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295769236907684886
#endif
#endif

static DisplaySprite spin(&tft);

static Usage       cur;
static NowPlaying  np;
static unsigned long npFetchedAt = 0;   // millis() when np.progressMs was current
static DisplayMode g_mode = MODE_USAGE;

static int  frame = 0;
static unsigned long lastFrame = 0;
static bool idleSpinnerDrawn = false;
static int  lastStatusWord = -1;        // 0=idle 1=working
static char spSig[192] = "\x01";        // what the track area currently shows
static char spShownArt[120] = "";       // art url currently on screen (or tried)
static long spShownSec = -1;            // progress second on screen
static int  spShownState = -1;          // 0=paused 1=playing

static bool usageVisible()   { return g_mode == MODE_USAGE || g_mode == MODE_SPLIT; }
static bool spotifyVisible() { return g_mode == MODE_SPOTIFY || g_mode == MODE_SPLIT; }

// ---------------------------------------------------------------- helpers

void asciiCopy(char *dst, size_t n, const char *src) {
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && j + 1 < n; p++)
        if (*p >= 0x20 && *p < 0x7F) dst[j++] = (char)*p;
    dst[j] = '\0';
}

// Mix two RGB565 colours: t=0 gives a, t=255 gives b. Channels are blended in
// their own bit widths (5/6/5) so no precision is lost going via RGB888.
// (inline so the narrow-layout builds, which don't use it, don't warn.)
static inline uint16_t blend565(uint16_t a, uint16_t b, int t) {
    if (t < 0) t = 0; else if (t > 255) t = 255;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return (uint16_t)((((ar + (br - ar) * t / 255) & 0x1F) << 11) |
                      (((ag + (bg - ag) * t / 255) & 0x3F) << 5)  |
                       ((ab + (bb - ab) * t / 255) & 0x1F));
}

static uint16_t barColor(float pct) {
    if (pct < 50) return COL_GREEN;
    if (pct < 80) return COL_YELLOW;
    return COL_RED;
}

// Shorten s in place (appending "..") until it fits in maxW pixels. Uses the
// current text size, so set it before calling when scaling fonts up.
static void ellipsize(char *s, size_t cap, int maxW, int font) {
    tft.setTextFont(font);
    if ((int)tft.textWidth(s) <= maxW) return;
    char buf[96];
    size_t len = strlen(s);
    do {
        len--;
        snprintf(buf, sizeof(buf), "%.*s..", (int)len, s);
    } while (len > 0 && (int)tft.textWidth(buf) > maxW);
    strlcpy(s, buf, cap);
}

// Break s at a word boundary so the first line fits in maxW; the remainder is
// ellipsized into l2. A single overlong word gets hard-broken instead.
static void wrapTwoLines(const char *s, char *l1, size_t n1, char *l2, size_t n2,
                         int maxW, int font) {
    tft.setTextFont(font);
    strlcpy(l1, s, n1);
    l2[0] = '\0';
    if ((int)tft.textWidth(l1) <= maxW) return;

    size_t fit = strlen(l1);
    while (fit > 1) {  // longest prefix that fits
        l1[--fit] = '\0';
        if ((int)tft.textWidth(l1) <= maxW) break;
    }
    size_t brk = fit;
    while (brk > 0 && l1[brk - 1] != ' ') brk--;  // back up to a space
    size_t split = brk > 0 ? brk : fit;
    strlcpy(l2, s + split, n2);
    l1[split] = '\0';
    while (split > 0 && l1[split - 1] == ' ') l1[--split] = '\0';
    ellipsize(l2, n2, maxW, font);
}

static void fmtMs(long ms, char *out, size_t n) {
    if (ms < 0) ms = 0;
    long s = ms / 1000;
    snprintf(out, n, "%ld:%02ld", s / 60, s % 60);
}

static void spSignature(char *out, size_t n) {
    snprintf(out, n, "%d|%d|%ld|%s|%s|%s", (int)np.valid, (int)np.hasTrack,
             np.durationMs, np.track, np.artist, np.album);
}

// Flat take on the Spotify mark: green disc + three "sound wave" bars,
// scaled from the original r=20 artwork.
static void drawSpotifyLogo(int cx, int cy, int r) {
    tft.fillCircle(cx, cy, r, COL_SPOTIFY);
    int h = r / 5;
    if (h < 3) h = 3;
    tft.fillRoundRect(cx - (r * 13) / 20, cy - (r * 9) / 20, (r * 27) / 20, h, h / 2, COL_BG);
    tft.fillRoundRect(cx - (r * 11) / 20, cy - (r * 1) / 20, (r * 22) / 20, h, h / 2, COL_BG);
    tft.fillRoundRect(cx - (r * 8) / 20,  cy + (r * 7) / 20, (r * 17) / 20, h, h / 2, COL_BG);
}

// The built-in bitmap fonts have no play/pause glyphs, so draw them as
// shapes: a triangle while playing, two bars while paused.
static void drawPlayPause(int cx, int y, int h, bool playing) {
    if (playing) {
        tft.fillTriangle(cx - h / 3, y, cx - h / 3, y + h, cx + h / 2, y + h / 2, COL_SPOTIFY);
    } else {
        int bw = h / 3;
        tft.fillRoundRect(cx - bw - 2, y, bw, h, 1, COL_DIM);
        tft.fillRoundRect(cx + 2, y, bw, h, 1, COL_DIM);
    }
}

// ============================================================================
#if !LAYOUT_WIDE
// ================== narrow layout (172x320 portrait / 480x320) =============

static const int SPIN_SIZE = 60;
static const int SPIN_X = (SCREEN_W - SPIN_SIZE) / 2;  // centered
static const int SPIN_Y = 78;

// Animation pacing, per layout (used by uiTick). The small panels keep the
// original slow 8-spoke sweep; the CPU on the C6 is better spent elsewhere.
static const int SPIN_FRAMES   = 48;   // frames per revolution
static const int SPIN_FRAME_MS = 90;   // ~4.3 s per revolution

static const int BAR_X = 12;
static const int BAR_W = SCREEN_W - 2 * BAR_X;
static const int BAR_H = 24;
static const int PCT_X = SCREEN_W - BAR_X;  // right edge of the % / reset readouts
static const int DIV_Y = 166;               // divider under the header
static const int SEC1_Y = 176;              // 5-hour section
static const int SEC2_Y = 244;              // weekly section
static const int STATUS_Y = 304;

// ---- Spotify screen ----
static const int SP_DIV_Y    = 60;    // divider under the header
static const int SP_TRACK_Y  = 72;    // track name, up to two lines
static const int SP_ARTIST_Y = 114;   // artists, tucked under the track
static const int SP_ALBUM_Y  = 138;   // album title
static const int SP_ART_Y    = 156;   // album art, centered (LovyanGFX only)
static const int SP_ART_SIZE = 64;    // Spotify's smallest native variant
static const int SP_TIME_Y   = 238;   // elapsed / total readouts
static const int SP_BAR_Y    = 252;   // progress bar
static const int SP_BAR_H    = 10;
static const int SP_STATE_Y  = 272;   // playing / paused

void uiStatus(const char *msg, uint16_t color) {
    tft.fillRect(0, STATUS_Y, SCREEN_W, 12, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, COL_BG);
    tft.drawString(msg, BAR_X, STATUS_Y, 1);  // font 1 to fit the narrow panel
}

static void drawSpinner(bool active) {
    spin.fillSprite(COL_BG);
    uint16_t c = active ? COL_ORANGE : COL_CARD;
    float rot = active ? frame * 7.5f * DEG_TO_RAD : 0.0f;
    const float cx = SPIN_SIZE / 2.0f, cy = SPIN_SIZE / 2.0f;
    for (int i = 0; i < 8; i++) {
        float a = rot + i * (PI / 4.0f);
        float ca = cosf(a), sa = sinf(a);
#if DISPLAY_IS_LGFX
        // LovyanGFX anti-aliases against existing sprite pixels (pre-filled
        // with COL_BG), so no explicit background-colour argument.
        spin.drawWideLine(cx + ca * 7, cy + sa * 7,
                          cx + ca * 25, cy + sa * 25, 3.0f, c);
#else
        spin.drawWideLine(cx + ca * 7, cy + sa * 7,
                          cx + ca * 25, cy + sa * 25, 3.0f, c, COL_BG);
#endif
    }
#if DISPLAY_IS_LGFX
    spin.fillCircle((int)(SPIN_SIZE / 2), (int)(SPIN_SIZE / 2), 3, c);
#else
    spin.fillSmoothCircle(SPIN_SIZE / 2, SPIN_SIZE / 2, 3, c, COL_BG);
#endif
    spin.pushSprite(SPIN_X, SPIN_Y);
}

static void drawStatusWord(bool active) {
    int word = active ? 1 : 0;
    if (word == lastStatusWord) return;
    lastStatusWord = word;
    tft.fillRect(0, SPIN_Y + SPIN_SIZE + 4, SCREEN_W, 18, COL_BG);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(active ? COL_ORANGE : COL_DIM, COL_BG);
    tft.drawString(active ? "working..." : "idle", SCREEN_W / 2,
                   SPIN_Y + SPIN_SIZE + 4, 2);
}

static void drawBar(int y, const char *label, float pct, const char *reset) {
    // label, left-aligned and dim
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(label, BAR_X, y, 2);

    // % readout, bright, right-aligned on the same row as the label
    tft.fillRect(BAR_X + 96, y, PCT_X - (BAR_X + 96), 16, COL_BG);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    if (pct >= 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", (int)roundf(pct));
        tft.drawString(buf, PCT_X, y, 2);
    } else {
        tft.drawString("--", PCT_X, y, 2);
    }

    // reset time on its own line below the label
    tft.fillRect(BAR_X, y + 17, BAR_W, 10, COL_BG);
    if (reset[0] != '\0') {
        char buf[40];
        snprintf(buf, sizeof(buf), "resets %s", reset);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(buf, BAR_X, y + 17, 1);
    }

    // the bar itself, full width
    int by = y + 30;
    tft.fillRoundRect(BAR_X, by, BAR_W, BAR_H, 6, COL_CARD);
    if (pct >= 0) {
        int fw = (int)(BAR_W * (pct > 100 ? 100 : pct) / 100.0f);
        if (fw > 8) tft.fillRoundRect(BAR_X, by, fw, BAR_H, 6, barColor(pct));
    }
}

static void drawBars() {
    drawBar(SEC1_Y, "5-HOUR", cur.fivePct, cur.fiveReset);
    drawBar(SEC2_Y, "WEEKLY", cur.weekPct, cur.weekReset);
}

static void drawUsageStaticUI() {
    tft.fillScreen(COL_BG);
    drawMascot(tft, BAR_X, 12, 4, COL_ORANGE, TFT_BLACK);  // 13x10 grid -> 52x40
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_ORANGE, COL_BG);
    tft.drawString("Claude Code", 68, 18, 2);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("usage monitor", 68, 40, 1);
    tft.drawFastHLine(BAR_X, DIV_Y, BAR_W, COL_CARD);
    drawBars();
    drawSpinner(false);
    drawStatusWord(false);
}

static void drawSpotifyProgress(long ms) {
    char el[12], tot[12];
    fmtMs(ms, el, sizeof(el));
    fmtMs(np.durationMs, tot, sizeof(tot));

    tft.fillRect(BAR_X, SP_TIME_Y, BAR_W, 12, COL_BG);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(el, BAR_X, SP_TIME_Y, 1);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(tot, PCT_X, SP_TIME_Y, 1);

    tft.fillRoundRect(BAR_X, SP_BAR_Y, BAR_W, SP_BAR_H, 4, COL_CARD);
    if (np.durationMs > 0) {
        int fw = (int)((long long)BAR_W * ms / np.durationMs);
        if (fw > 6) tft.fillRoundRect(BAR_X, SP_BAR_Y, fw, SP_BAR_H, 4, COL_SPOTIFY);
    }
}

static void drawSpotifyStateWord(bool playing) {
    tft.fillRect(0, SP_STATE_Y, SCREEN_W, 18, COL_BG);
    drawPlayPause(SCREEN_W / 2, SP_STATE_Y + 1, 14, playing);
}

// Repaint the whole track area (everything between the header divider and the
// status line) from np. Progress/state redraw right after via their caches.
static void drawSpotifyTrack() {
    spSignature(spSig, sizeof(spSig));
    spShownSec = -1;
    spShownState = -1;
    tft.fillRect(0, SP_DIV_Y + 2, SCREEN_W, STATUS_Y - SP_DIV_Y - 6, COL_BG);
    if (!np.hasTrack) {
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(np.valid ? "nothing playing" : "loading...",
                       SCREEN_W / 2, (SP_DIV_Y + STATUS_Y) / 2 - 8, 2);
        return;
    }

    char l1[80], l2[80];
    wrapTwoLines(np.track, l1, sizeof(l1), l2, sizeof(l2), BAR_W, 2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(l1, BAR_X, SP_TRACK_Y, 2);
    if (l2[0]) tft.drawString(l2, BAR_X, SP_TRACK_Y + 20, 2);

    char line[64];
    strlcpy(line, np.artist, sizeof(line));
    ellipsize(line, sizeof(line), BAR_W, 2);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(line, BAR_X, SP_ARTIST_Y, 2);

    strlcpy(line, np.album, sizeof(line));
    ellipsize(line, sizeof(line), BAR_W, 1);
    tft.drawString(line, BAR_X, SP_ALBUM_Y, 1);

#if DISPLAY_IS_LGFX
    // Album art placeholder; uiPushArt() paints over it once fetched.
    spShownArt[0] = '\0';
    if (np.artUrl[0])
        tft.fillRoundRect((SCREEN_W - SP_ART_SIZE) / 2, SP_ART_Y,
                          SP_ART_SIZE, SP_ART_SIZE, 4, COL_CARD);
#endif
}

static void drawSpotifyStaticUI() {
    tft.fillScreen(COL_BG);
    drawSpotifyLogo(BAR_X + 20, 32, 20);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_SPOTIFY, COL_BG);
    tft.drawString("Spotify", 68, 18, 2);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("now playing", 68, 40, 1);
    tft.drawFastHLine(BAR_X, SP_DIV_Y, BAR_W, COL_CARD);
    drawSpotifyTrack();
}

void uiFullRepaint() {
    lastStatusWord = -1;
    idleSpinnerDrawn = false;
    if (g_mode == MODE_SPOTIFY) drawSpotifyStaticUI();
    else drawUsageStaticUI();
}

int uiArtSizePx() { return SP_ART_SIZE; }

static void artRect(int *x, int *y, int *size) {
    *x = (SCREEN_W - SP_ART_SIZE) / 2;
    *y = SP_ART_Y;
    *size = SP_ART_SIZE;
}

// ============================================================================
#else
// ====================== wide layout (1920x480 bar panel) ===================

static const int SPIN_SIZE = 100;

// Animation pacing, per layout (used by uiTick). The wide panel runs on the
// ESP32-P4, which has the headroom for a proper 25 fps sweep.
static const int SPIN_FRAMES   = 36;   // frames per revolution
static const int SPIN_FRAME_MS = 40;   // ~1.4 s per revolution

// ---- usage, full screen: mascot / spinner left, the two bars right ----
static const int WU_DIV_X   = 600;                    // vertical divider
static const int WU_LEFT_CX = 300;                    // centerline of the left column
static const int WU_RX      = 640;                    // right column x
static const int WU_RIGHT   = SCREEN_W - 64;          // right column right edge
static const int WU_RW      = WU_RIGHT - WU_RX;       // right column width
static const int WU_SEC1_Y  = 40;                     // 5-hour section
static const int WU_SEC2_Y  = 248;                    // weekly section
static const int WU_BAR_H   = 48;
static const int WU_SPIN_Y  = 336;

// ---- spotify, full screen: album art left, details right ----
static const int WS_ART_X    = 48;
static const int WS_ART_Y    = 48;
static const int WS_ART_SIZE = 384;
static const int WS_TX       = 496;                   // text column x
static const int WS_RIGHT    = SCREEN_W - 64;
static const int WS_TW       = WS_RIGHT - WS_TX;      // text column width
static const int WS_TRACK_Y  = 128;                   // two lines of font4 x2
static const int WS_ARTIST_Y = 258;
static const int WS_ALBUM_Y  = 294;
static const int WS_TIME_Y   = 356;
static const int WS_BAR_Y    = 378;
static const int WS_BAR_H    = 20;
static const int WS_STATE_Y  = 412;

// ---- split screen: usage on the left half, spotify on the right ----
static const int SPL_DIV_X    = 960;
static const int SPL_LX       = 48;                   // left half margin
static const int SPL_LRIGHT   = 912;                  // left half right edge
static const int SPL_LW       = SPL_LRIGHT - SPL_LX;
static const int SPL_SPIN_X   = 790;
static const int SPL_SPIN_Y   = 28;
static const int SPL_SEC1_Y   = 166;
static const int SPL_SEC2_Y   = 298;
static const int SPL_BAR_H    = 40;
static const int SPL_ART_X    = 1008;
static const int SPL_ART_Y    = 76;
static const int SPL_ART_SIZE = 280;
static const int SPL_TX       = 1328;                 // spotify text column x
static const int SPL_RIGHT    = SCREEN_W - 64;
static const int SPL_TW       = SPL_RIGHT - SPL_TX;
static const int SPL_TRACK_Y  = 124;
static const int SPL_ARTIST_Y = 196;
static const int SPL_ALBUM_Y  = 222;
static const int SPL_TIME_Y   = 268;
static const int SPL_BAR_Y    = 290;
static const int SPL_BAR_H2   = 16;
static const int SPL_STATE_Y  = 322;

static const int STATUS_Y = 452;                      // shared, bottom line

// Status line x/width depends on which screen is up.
static void statusRegion(int *x, int *right) {
    switch (g_mode) {
        case MODE_SPOTIFY: *x = WS_TX;  *right = WS_RIGHT;  break;
        case MODE_SPLIT:   *x = SPL_LX; *right = SPL_LRIGHT; break;
        default:           *x = WU_RX;  *right = WU_RIGHT;  break;
    }
}

void uiStatus(const char *msg, uint16_t color) {
    int x, right;
    statusRegion(&x, &right);
    tft.fillRect(x, STATUS_Y, right - x, 18, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, COL_BG);
    tft.drawString(msg, x, STATUS_Y, 2);
}

static void spinnerPos(int *x, int *y) {
    if (g_mode == MODE_SPLIT) { *x = SPL_SPIN_X; *y = SPL_SPIN_Y; }
    else { *x = WU_LEFT_CX - SPIN_SIZE / 2; *y = WU_SPIN_Y; }
}

// Orbiting-dot spinner: SPIN_DOTS dots on a ring, tapering in both size and
// brightness from the leading dot backwards, so the tail dissolves into the
// background like a comet.
//
// The taper is the whole point. The previous version drew 8 identical spokes
// from a common centre, which is 8-fold symmetric in a single flat colour -
// it read as a static asterisk and you could barely tell it was turning.
// Giving each dot a different weight breaks the symmetry, so both the motion
// and its direction are obvious.
//
// fillSmoothCircle is anti-aliased (fillArc is not), so the dots stay clean
// at this size instead of showing stair-stepped edges.
static const int SPIN_DOTS = 12;

static void drawSpinner(bool active) {
    spin.fillSprite(COL_BG);
    const float cx = SPIN_SIZE / 2.0f, cy = SPIN_SIZE / 2.0f;
    // Sized to fill the sprite: ring + leading dot reaches 0.48 of SPIN_SIZE,
    // just inside the 0.5 edge. The footprint is fixed because split mode
    // packs the spinner between x=790 and the 912 column edge.
    const float ring   = SPIN_SIZE * 0.385f;  // orbit radius
    const float dotMax = SPIN_SIZE * 0.095f;  // leading dot radius

    // Idle draws the same ring at a flat, quiet weight - a calm "at rest"
    // state rather than a wheel frozen mid-spin.
    if (!active) {
        for (int i = 0; i < SPIN_DOTS; i++) {
            float a = i * (2.0f * (float)PI / SPIN_DOTS);
            spin.fillSmoothCircle((int)(cx + cosf(a) * ring + 0.5f),
                                  (int)(cy + sinf(a) * ring + 0.5f),
                                  (int)(dotMax * 0.5f + 0.5f), COL_CARD);
        }
    } else {
        const float step = 2.0f * (float)PI / SPIN_DOTS;
        const float head = frame * (2.0f * (float)PI / SPIN_FRAMES);
        for (int i = 0; i < SPIN_DOTS; i++) {
            float lead = 1.0f - (float)i / SPIN_DOTS;  // 1 at the head, ->0 at the tail
            float a = head - i * step;
            // Squared falloff keeps the head crisp and the tail long and soft.
            uint16_t c = blend565(COL_BG, COL_ORANGE, (int)(255.0f * lead * lead));
            float r = dotMax * (0.40f + 0.60f * lead);
            spin.fillSmoothCircle((int)(cx + cosf(a) * ring + 0.5f),
                                  (int)(cy + sinf(a) * ring + 0.5f),
                                  (int)(r + 0.5f), c);
        }
    }
    int x, y;
    spinnerPos(&x, &y);
    spin.pushSprite(x, y);
}

static void drawStatusWord(bool active) {
    int word = active ? 1 : 0;
    if (word == lastStatusWord) return;
    lastStatusWord = word;
    int x, y;
    spinnerPos(&x, &y);
    int cx = x + SPIN_SIZE / 2, wy = y + SPIN_SIZE + 6;
    // Font 4 to hold its own against the big numerals in the right column.
    // The 30px band still clears SPL_SEC1_Y (166) in split mode, where this
    // sits lowest: 28 + 100 + 6 + 30 = 164.
    tft.fillRect(cx - 100, wy, 200, 30, COL_BG);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(active ? COL_ORANGE : COL_DIM, COL_BG);
    tft.drawString(active ? "working..." : "idle", cx, wy, 4);
}

// One usage section: label + reset on the left, a big colored % on the
// right, and a thick bar underneath.
static void drawBarWide(int x, int right, int y, int barH, const char *label,
                        float pct, const char *reset, bool bigPct) {
    int w = right - x;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(label, x, y + 8, 4);

    int resetY = y + (bigPct ? 46 : 40);
    tft.fillRect(x, resetY, w / 2, 20, COL_BG);
    if (reset[0] != '\0') {
        char buf[40];
        snprintf(buf, sizeof(buf), "resets %s", reset);
        tft.drawString(buf, x, resetY, 2);
    }

    // big % readout, right-aligned, colored like the bar
    uint16_t c = pct >= 0 ? barColor(pct) : COL_DIM;
    tft.fillRect(right - 330, y, 330, bigPct ? 80 : 56, COL_BG);
    tft.setTextColor(c, COL_BG);
    if (pct >= 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", (int)roundf(pct));
        if (bigPct) {
            // font 8 is digits-only (75px), so the % sign is drawn in font 4
            tft.setTextDatum(TR_DATUM);
            tft.drawString(buf, right - 44, y, 8);
            tft.setTextDatum(TL_DATUM);
            tft.drawString("%", right - 36, y + 44, 4);
        } else {
            tft.setTextSize(2);
            strlcat(buf, "%", sizeof(buf));
            tft.setTextDatum(TR_DATUM);
            tft.drawString(buf, right, y - 6, 4);
            tft.setTextSize(1);
        }
    } else {
        tft.setTextDatum(TR_DATUM);
        tft.setTextSize(2);
        tft.drawString("--", right, y, 4);
        tft.setTextSize(1);
    }
    tft.setTextDatum(TL_DATUM);

    int by = y + (bigPct ? 92 : 66);
    tft.fillRoundRect(x, by, w, barH, barH / 4, COL_CARD);
    if (pct >= 0) {
        int fw = (int)(w * (pct > 100 ? 100 : pct) / 100.0f);
        if (fw > barH / 2) tft.fillRoundRect(x, by, fw, barH, barH / 4, barColor(pct));
    }
}

static void drawBars() {
    if (g_mode == MODE_SPLIT) {
        drawBarWide(SPL_LX, SPL_LRIGHT, SPL_SEC1_Y, SPL_BAR_H, "5-HOUR",
                    cur.fivePct, cur.fiveReset, false);
        drawBarWide(SPL_LX, SPL_LRIGHT, SPL_SEC2_Y, SPL_BAR_H, "WEEKLY",
                    cur.weekPct, cur.weekReset, false);
    } else {
        drawBarWide(WU_RX, WU_RIGHT, WU_SEC1_Y, WU_BAR_H, "5-HOUR",
                    cur.fivePct, cur.fiveReset, true);
        drawBarWide(WU_RX, WU_RIGHT, WU_SEC2_Y, WU_BAR_H, "WEEKLY",
                    cur.weekPct, cur.weekReset, true);
    }
}

// Mascot + title block, used by the usage screen (big) and split (small).
static void drawClaudeHeader(int x, int y, int scale) {
    drawMascot(tft, x, y, scale, COL_ORANGE, TFT_BLACK);
}

static void drawUsageStaticUI() {
    tft.fillScreen(COL_BG);
    // left column: big Clawd, name, spinner
    drawClaudeHeader(WU_LEFT_CX - (MASCOT_COLS * 18) / 2, 44, 18);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COL_ORANGE, COL_BG);
    tft.setTextSize(2);
    tft.drawString("Claude Code", WU_LEFT_CX, 250, 4);
    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("usage monitor", WU_LEFT_CX, 308, 2);
    tft.drawFastVLine(WU_DIV_X, 28, STATUS_Y - 32, COL_CARD);
    drawBars();
    drawSpinner(false);
    drawStatusWord(false);
}

static void artRect(int *x, int *y, int *size) {
    if (g_mode == MODE_SPLIT) { *x = SPL_ART_X; *y = SPL_ART_Y; *size = SPL_ART_SIZE; }
    else { *x = WS_ART_X; *y = WS_ART_Y; *size = WS_ART_SIZE; }
}

int uiArtSizePx() {
    return g_mode == MODE_SPLIT ? SPL_ART_SIZE : WS_ART_SIZE;
}

// Placeholder where the album art goes: a dark card with a vinyl-ish ring.
static void drawArtPlaceholder() {
    int x, y, size;
    artRect(&x, &y, &size);
    tft.fillRoundRect(x, y, size, size, 8, COL_CARD);
    int cx = x + size / 2, cy = y + size / 2;
    tft.drawCircle(cx, cy, size / 3, COL_DIM);
    tft.drawCircle(cx, cy, size / 3 - 1, COL_DIM);
    tft.fillCircle(cx, cy, size / 12, COL_DIM);
}

// Track / artist / album text block for either spotify layout.
static void drawSpotifyText(int x, int right, bool full) {
    int w = right - x;
    char l1[80], l2[80];
    if (full) {
        tft.setTextSize(2);
        wrapTwoLines(np.track, l1, sizeof(l1), l2, sizeof(l2), w / 2, 4);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.drawString(l1, x, WS_TRACK_Y, 4);
        if (l2[0]) tft.drawString(l2, x, WS_TRACK_Y + 58, 4);
        tft.setTextSize(1);

        char line[64];
        strlcpy(line, np.artist, sizeof(line));
        ellipsize(line, sizeof(line), w, 4);
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.drawString(line, x, WS_ARTIST_Y, 4);

        strlcpy(line, np.album, sizeof(line));
        ellipsize(line, sizeof(line), w, 2);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(line, x, WS_ALBUM_Y, 2);
    } else {
        wrapTwoLines(np.track, l1, sizeof(l1), l2, sizeof(l2), w, 4);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.drawString(l1, x, SPL_TRACK_Y, 4);
        if (l2[0]) tft.drawString(l2, x, SPL_TRACK_Y + 32, 4);

        char line[64];
        strlcpy(line, np.artist, sizeof(line));
        ellipsize(line, sizeof(line), w, 2);
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.drawString(line, x, SPL_ARTIST_Y, 2);

        strlcpy(line, np.album, sizeof(line));
        ellipsize(line, sizeof(line), w, 2);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(line, x, SPL_ALBUM_Y, 2);
    }
}

static void drawSpotifyProgress(long ms) {
    bool full = g_mode != MODE_SPLIT;
    int x = full ? WS_TX : SPL_TX;
    int right = full ? WS_RIGHT : SPL_RIGHT;
    int ty = full ? WS_TIME_Y : SPL_TIME_Y;
    int by = full ? WS_BAR_Y : SPL_BAR_Y;
    int bh = full ? WS_BAR_H : SPL_BAR_H2;
    int w = right - x;

    char el[12], tot[12];
    fmtMs(ms, el, sizeof(el));
    fmtMs(np.durationMs, tot, sizeof(tot));

    tft.fillRect(x, ty, w, 18, COL_BG);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(el, x, ty, 2);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(tot, right, ty, 2);

    tft.fillRoundRect(x, by, w, bh, bh / 3, COL_CARD);
    if (np.durationMs > 0) {
        int fw = (int)((long long)w * ms / np.durationMs);
        if (fw > bh / 2) tft.fillRoundRect(x, by, fw, bh, bh / 3, COL_SPOTIFY);
    }
}

static void drawSpotifyStateWord(bool playing) {
    bool full = g_mode != MODE_SPLIT;
    int x = full ? WS_TX : SPL_TX;
    int right = full ? WS_RIGHT : SPL_RIGHT;
    int y = full ? WS_STATE_Y : SPL_STATE_Y;
    int h = full ? 20 : 16;
    int cx = (x + right) / 2;
    tft.fillRect(cx - 20, y, 40, h + 2, COL_BG);
    drawPlayPause(cx, y, h, playing);
}

// Repaint the track area of whichever spotify layout is showing.
static void drawSpotifyTrack() {
    spSignature(spSig, sizeof(spSig));
    spShownSec = -1;
    spShownState = -1;
    spShownArt[0] = '\0';

    bool full = g_mode != MODE_SPLIT;
    int x = full ? WS_TX : SPL_TX;
    int right = full ? WS_RIGHT : SPL_RIGHT;
    int top = full ? 110 : 110;
    tft.fillRect(x, top, right - x, STATUS_Y - top - 6, COL_BG);

    int ax, ay, asize;
    artRect(&ax, &ay, &asize);
    tft.fillRect(ax, ay, asize, asize, COL_BG);

    if (!np.hasTrack) {
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.drawString(np.valid ? "nothing playing" : "loading...",
                       (x + right) / 2, 220, 4);
        return;
    }
    drawArtPlaceholder();
    drawSpotifyText(x, right, full);
}

// Header (logo + wordmark) for the spotify column.
static void drawSpotifyHeader(int x, int y, bool full) {
    int r = full ? 20 : 16;
    drawSpotifyLogo(x + r, y + r + 4, r);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_SPOTIFY, COL_BG);
    tft.drawString("Spotify", x + 2 * r + 14, y, 4);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("now playing", x + 2 * r + 14, y + 30, 2);
}

static void drawSpotifyStaticUI() {
    tft.fillScreen(COL_BG);
    drawSpotifyHeader(WS_TX, 44, true);
    drawSpotifyTrack();
}

static void drawSplitStaticUI() {
    tft.fillScreen(COL_BG);
    // left half: compact usage
    drawClaudeHeader(SPL_LX, 36, 9);  // 13x10 grid -> 117x90
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_ORANGE, COL_BG);
    tft.drawString("Claude Code", 186, 44, 4);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("usage monitor", 186, 78, 2);
    tft.drawFastVLine(SPL_DIV_X, 24, STATUS_Y - 28, COL_CARD);
    drawBars();
    drawSpinner(false);
    drawStatusWord(false);
    // right half: compact spotify
    drawSpotifyHeader(SPL_TX, 44, false);
    drawSpotifyTrack();
}

void uiFullRepaint() {
    lastStatusWord = -1;
    idleSpinnerDrawn = false;
    switch (g_mode) {
        case MODE_SPOTIFY: drawSpotifyStaticUI(); break;
        case MODE_SPLIT:   drawSplitStaticUI();   break;
        default:           drawUsageStaticUI();   break;
    }
}

#endif  // LAYOUT_WIDE
// ============================================================================
// ------------------------- shared, layout-independent -----------------------

void uiInit() {
    spin.setColorDepth(16);
    spin.createSprite(SPIN_SIZE, SPIN_SIZE);
}

DisplayMode uiMode() { return g_mode; }

void uiSetModeInitial(DisplayMode m) { g_mode = m; }

void uiSetMode(DisplayMode m) {
    if (m == g_mode) return;
    g_mode = m;
    uiFullRepaint();
}

void uiSetUsage(const Usage &u) {
    bool changed = !cur.valid ||
                   u.fivePct != cur.fivePct ||
                   u.weekPct != cur.weekPct ||
                   strcmp(u.fiveReset, cur.fiveReset) != 0 ||
                   strcmp(u.weekReset, cur.weekReset) != 0;
    cur = u;
    if (changed && usageVisible()) drawBars();
}

void uiSetNowPlaying(const NowPlaying &n, unsigned long fetchedAtMs) {
    np = n;
    npFetchedAt = fetchedAtMs;
    if (!spotifyVisible()) return;
    char sig[192];
    spSignature(sig, sizeof(sig));
    if (strcmp(sig, spSig) != 0) drawSpotifyTrack();
}

bool uiArtWanted(char *url, size_t urlCap) {
#if DISPLAY_IS_LGFX
    if (!spotifyVisible() || !np.hasTrack || !np.artUrl[0]) return false;
    if (strcmp(np.artUrl, spShownArt) == 0) return false;
    strlcpy(spShownArt, np.artUrl, sizeof(spShownArt));  // one attempt per track
    strlcpy(url, np.artUrl, urlCap);
    return true;
#else
    (void)url; (void)urlCap;
    return false;  // TFT_eSPI builds skip album art (no jpeg decoder)
#endif
}

void uiPushArt(const uint8_t *jpg, size_t len, long nativeW) {
#if DISPLAY_IS_LGFX
    if (!spotifyVisible() || !np.hasTrack) return;
    int x, y, size;
    artRect(&x, &y, &size);
    float scale = nativeW > 0 ? (float)size / nativeW : 1.0f;
    tft.drawJpg(jpg, len, x, y, size, size, 0, 0, scale, scale);
#else
    (void)jpg; (void)len; (void)nativeW;
#endif
}

void uiTick(unsigned long now, bool thinking) {
    if (usageVisible()) {
        if (thinking) {
            idleSpinnerDrawn = false;
            if (now - lastFrame >= (unsigned long)SPIN_FRAME_MS) {
                lastFrame = now;
                frame = (frame + 1) % SPIN_FRAMES;
                drawSpinner(true);
            }
        } else if (!idleSpinnerDrawn) {
            idleSpinnerDrawn = true;
            drawSpinner(false);
        }
        drawStatusWord(thinking);
    }

    if (spotifyVisible() && np.hasTrack) {
        long est = np.progressMs + (np.playing ? (long)(now - npFetchedAt) : 0);
        if (np.durationMs > 0 && est > np.durationMs) est = np.durationMs;
        long sec = est / 1000;
        if (sec != spShownSec) {
            spShownSec = sec;
            drawSpotifyProgress(est);
        }
        int st = np.playing ? 1 : 0;
        if (st != spShownState) {
            spShownState = st;
            drawSpotifyStateWord(np.playing);
        }
    }
}
