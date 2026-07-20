#pragma once

// Plain data shared between the network side (main.cpp), the renderer
// (ui.cpp) and the desktop emulator (emulator/main.cpp). Keep this file free
// of Arduino / LovyanGFX includes so every frontend can use it.

#include <stdint.h>

struct Usage {
    bool  valid = false;
    float fivePct = -1;
    float weekPct = -1;
    char  fiveReset[24] = "";
    char  weekReset[24] = "";
};

// Which screen is showing. Persisted in NVS so the display comes back up in
// the same mode after a power cycle. MODE_SPLIT (usage + Spotify side by
// side) only exists on wide panels; narrow builds fall back to MODE_USAGE.
enum DisplayMode : uint8_t { MODE_USAGE = 0, MODE_SPOTIFY = 1, MODE_SPLIT = 2 };

struct NowPlaying {
    bool valid = false;      // at least one successful fetch
    bool hasTrack = false;
    bool playing = false;
    long progressMs = 0;
    long durationMs = 0;
    char track[80] = "";
    char artist[64] = "";
    char album[64] = "";
    char artUrl[120] = "";   // smallest suitable album-art jpeg
    long artW = 0;           // its native width, for scaling
};
