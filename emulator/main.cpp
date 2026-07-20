// Desktop emulator for the desk display.
//
// Runs the exact firmware renderer (firmware/src/ui.cpp) in a 1920x480 SDL
// window with fake data, so layouts can be checked before the real 8.8"
// 480x1920 panel arrives. No networking - the usage numbers and the Spotify
// queue are canned, the progress bar ticks in real time and tracks advance
// when they end.
//
// Controls
//   click window        cycle usage -> spotify -> split
//   stdin commands      mode usage|spotify|split|toggle
//                       think on|off|auto        (auto: 8s on / 4s off)
//                       play | pause | next
//                       usage <fivePct> <weekPct>
//                       shot [file.bmp]          save a screenshot
//                       quit
// Flags
//   --mode usage|spotify|split   start screen (default usage)
//   --think on|off|auto          thinking beacon behaviour (default auto)
//   --art file.jpg               album art jpeg, ideally 640x640
//                                (default: emulator/art.jpg if present)
//   --shot file.bmp              screenshot after 2s, then exit (for CI/review)

#include "ui.h"

#include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

DisplayTFT tft;

// ---- fake data -------------------------------------------------------------

struct FakeTrack {
    const char *track, *artist, *album;
    long durationMs;
};

// Chosen to exercise the layouts: short names, a two-line wrap, ellipsis.
static const FakeTrack kTracks[] = {
    {"Am I Dreaming", "Metro Boomin, A$AP Rocky, Roisee",
     "METRO BOOMIN PRESENTS SPIDER-MAN: ACROSS THE SPIDER-VERSE", 221000},
    {"Bohemian Rhapsody", "Queen", "A Night at the Opera", 355000},
    {"Knights of Cydonia (Live from Wembley Stadium)", "Muse",
     "HAARP (Live from Wembley Stadium)", 362000},
    {"Paint The Town Red", "Doja Cat", "Scarlet", 230000},
};
static int g_trackIdx = 0;

static Usage g_usage;
static NowPlaying g_np;
static bool g_playing = true;

// thinking beacon: 0=off 1=on 2=auto
static int g_thinkMode = 2;

static std::string g_artPath = "art.jpg";
static uint8_t *g_artBuf = nullptr;
static size_t g_artLen = 0;

static std::string g_shotPath;      // --shot: save then exit
static std::atomic<bool> g_quit{false};

// ---- stdin command queue ---------------------------------------------------

static std::mutex g_cmdMutex;
static std::vector<std::string> g_cmds;

static void stdinThread() {
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        std::lock_guard<std::mutex> lock(g_cmdMutex);
        g_cmds.push_back(line);
    }
}

// ---- helpers ---------------------------------------------------------------

static void loadArt(const char *path) {
    free(g_artBuf);
    g_artBuf = nullptr;
    g_artLen = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && len < 4 * 1024 * 1024) {
        g_artBuf = (uint8_t *)malloc(len);
        if (g_artBuf && fread(g_artBuf, 1, len, f) == (size_t)len) g_artLen = len;
    }
    fclose(f);
    printf("album art: %s (%zu bytes)\n", path, g_artLen);
}

static void setTrack(int idx, unsigned long now) {
    g_trackIdx = ((idx % 4) + 4) % 4;
    const FakeTrack &t = kTracks[g_trackIdx];
    g_np = NowPlaying();
    g_np.valid = true;
    g_np.hasTrack = true;
    g_np.playing = g_playing;
    g_np.progressMs = 0;
    g_np.durationMs = t.durationMs;
    asciiCopy(g_np.track, sizeof(g_np.track), t.track);
    asciiCopy(g_np.artist, sizeof(g_np.artist), t.artist);
    asciiCopy(g_np.album, sizeof(g_np.album), t.album);
    if (g_artLen) {
        snprintf(g_np.artUrl, sizeof(g_np.artUrl), "local://art/%d", g_trackIdx);
        g_np.artW = 640;
    }
    uiSetNowPlaying(g_np, now);
}

// Minimal 24bpp BMP writer for screenshots.
static bool saveBmp(const char *path) {
    const int w = SCREEN_W, h = SCREEN_H;
    uint16_t *px = (uint16_t *)malloc((size_t)w * h * 2);
    if (!px) return false;
    tft.readRect(0, 0, w, h, px);

    const int rowBytes = (w * 3 + 3) & ~3;
    const uint32_t dataSize = (uint32_t)rowBytes * h;
    const uint32_t fileSize = 54 + dataSize;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4);
    uint32_t off = 54, hsz = 40;
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &hsz, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &dataSize, 4);

    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return false; }
    fwrite(hdr, 1, 54, f);
    uint8_t *row = (uint8_t *)calloc(1, rowBytes);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            // readRect returns big-endian (swapped) RGB565
            uint16_t c = px[(size_t)y * w + x];
            c = (uint16_t)((c >> 8) | (c << 8));
            row[x * 3 + 0] = (uint8_t)((c & 0x1F) << 3);          // B
            row[x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2);   // G
            row[x * 3 + 2] = (uint8_t)(((c >> 11) & 0x1F) << 3);  // R
        }
        fwrite(row, 1, rowBytes, f);
    }
    free(row);
    fclose(f);
    free(px);
    printf("screenshot: %s\n", path);
    return true;
}

static void cycleMode() {
    DisplayMode next = uiMode() == MODE_USAGE ? MODE_SPOTIFY
                     : uiMode() == MODE_SPOTIFY ? MODE_SPLIT
                     : MODE_USAGE;
    uiSetMode(next);
}

static void showStatus() {
    uiStatus("emulator  claude-display.local  192.168.1.42", COL_DIM);
}

static void handleCommand(const std::string &lineIn, unsigned long now) {
    char cmd[32] = "", a1[128] = "", a2[64] = "";
    sscanf(lineIn.c_str(), "%31s %127s %63s", cmd, a1, a2);
    if (!cmd[0]) return;

    if (!strcmp(cmd, "mode")) {
        if (!strcmp(a1, "usage")) uiSetMode(MODE_USAGE);
        else if (!strcmp(a1, "spotify")) uiSetMode(MODE_SPOTIFY);
        else if (!strcmp(a1, "split")) uiSetMode(MODE_SPLIT);
        else if (!strcmp(a1, "toggle")) cycleMode();
        showStatus();
        printf("mode: %d\n", (int)uiMode());
    } else if (!strcmp(cmd, "toggle")) {
        cycleMode();
        showStatus();
    } else if (!strcmp(cmd, "think")) {
        g_thinkMode = !strcmp(a1, "on") ? 1 : !strcmp(a1, "auto") ? 2 : 0;
    } else if (!strcmp(cmd, "play")) {
        g_playing = true; g_np.playing = true;
        uiSetNowPlaying(g_np, now);
    } else if (!strcmp(cmd, "pause")) {
        // the caller already folded elapsed time into progressMs
        g_playing = false; g_np.playing = false;
        uiSetNowPlaying(g_np, now);
    } else if (!strcmp(cmd, "next")) {
        setTrack(g_trackIdx + 1, now);
    } else if (!strcmp(cmd, "usage")) {
        g_usage.valid = true;
        g_usage.fivePct = (float)atof(a1);
        g_usage.weekPct = (float)atof(a2);
        uiSetUsage(g_usage);
    } else if (!strcmp(cmd, "shot")) {
        saveBmp(a1[0] ? a1 : "shot.bmp");
    } else if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        g_quit = true;
    } else {
        printf("commands: mode usage|spotify|split|toggle, think on|off|auto,\n"
               "          play, pause, next, usage <5h%%> <wk%%>, shot [f.bmp], quit\n");
    }
}

// ---- arduino-style entry points, run by Panel_sdl's user thread ------------

static unsigned long g_npAnchor = 0;   // "fetchedAt" of the current progressMs

void setup() {
    tft.init();
    tft.setRotation(SCREEN_ROTATION);
    uiInit();

    g_usage.valid = true;
    g_usage.fivePct = 33;
    g_usage.weekPct = 62;
    snprintf(g_usage.fiveReset, sizeof(g_usage.fiveReset), "4:19 AM");
    snprintf(g_usage.weekReset, sizeof(g_usage.weekReset), "Mon 1 PM");

    uiFullRepaint();
    uiSetUsage(g_usage);
    showStatus();

    loadArt(g_artPath.c_str());
    unsigned long now = lgfx::millis();
    g_npAnchor = now;
    setTrack(0, now);
}

void loop() {
    unsigned long now = lgfx::millis();

    // thinking beacon
    bool think = g_thinkMode == 1 || (g_thinkMode == 2 && (now / 1000) % 12 < 8);

    // advance the fake track when it ends
    if (g_np.playing && g_np.durationMs > 0 &&
        (long)(now - g_npAnchor) + g_np.progressMs > g_np.durationMs) {
        g_npAnchor = now;
        setTrack(g_trackIdx + 1, now);
    }

    // album art handshake, same as the firmware loop
    char artUrl[120];
    if (uiArtWanted(artUrl, sizeof(artUrl)) && g_artLen)
        uiPushArt(g_artBuf, g_artLen, g_np.artW);

    // pending stdin commands
    std::vector<std::string> cmds;
    {
        std::lock_guard<std::mutex> lock(g_cmdMutex);
        cmds.swap(g_cmds);
    }
    for (auto &c : cmds) {
        // fold elapsed progress into progressMs so play/pause math stays simple
        if (g_np.playing) {
            g_np.progressMs += (long)(now - g_npAnchor);
            if (g_np.durationMs > 0 && g_np.progressMs > g_np.durationMs)
                g_np.progressMs = g_np.durationMs;
        }
        g_npAnchor = now;
        handleCommand(c, now);
    }

    // click anywhere: cycle modes
    static bool wasTouched = false;
    lgfx::touch_point_t tp;
    bool touched = tft.getTouch(&tp);
    if (touched && !wasTouched) {
        cycleMode();
        showStatus();
    }
    wasTouched = touched;

    uiTick(now, think);

    // --shot: capture after the screen settles, then exit
    if (!g_shotPath.empty() && now > 2000) {
        saveBmp(g_shotPath.c_str());
        g_quit = true;
    }
    if (g_quit) exit(0);

    lgfx::delay(16);
}

static int userFunc(bool *running) {
    setup();
    while (*running && !g_quit) loop();
    return 0;
}

int main(int argc, char **argv) {
    DisplayMode startMode = MODE_USAGE;
    for (int i = 1; i < argc; i++) {
        auto next = [&](const char *dflt) -> const char * {
            return i + 1 < argc ? argv[++i] : dflt;
        };
        if (!strcmp(argv[i], "--mode")) {
            const char *m = next("usage");
            startMode = !strcmp(m, "spotify") ? MODE_SPOTIFY
                      : !strcmp(m, "split") ? MODE_SPLIT : MODE_USAGE;
        } else if (!strcmp(argv[i], "--think")) {
            const char *t = next("auto");
            g_thinkMode = !strcmp(t, "on") ? 1 : !strcmp(t, "off") ? 0 : 2;
        } else if (!strcmp(argv[i], "--art")) {
            g_artPath = next("art.jpg");
        } else if (!strcmp(argv[i], "--shot")) {
            g_shotPath = next("shot.bmp");
        } else {
            printf("usage: %s [--mode usage|spotify|split] [--think on|off|auto]\n"
                   "          [--art file.jpg] [--shot file.bmp]\n", argv[0]);
            return 1;
        }
    }
    uiSetModeInitial(startMode);

    std::thread t(stdinThread);
    t.detach();

    return lgfx::Panel_sdl::main(userFunc);
}
