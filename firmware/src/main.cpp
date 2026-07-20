// Claude Code usage display for ESP32 + TFT / MIPI-DSI panels.
//
// Fully self-contained: the ESP32 fetches your real 5-hour / weekly rate-limit
// utilization straight from Anthropic's OAuth usage API over HTTPS, using a
// dedicated login it refreshes itself (see config.h and server/device_login.py).
// No companion server needed.
//
// "Claude is thinking right now" can't be read from the usage API, so the
// device listens for tiny HTTP "beacons" instead: run server/beacon.py on any
// computer you use Claude Code on (Mac, Windows, Linux) and it pings the device
// while Claude is working. The onboard RGB LED blinks green whenever a beacon
// is live, from any machine.
//
// The same little HTTP server also switches what's on screen: the usage view,
// a Spotify "now playing" view (track / artist / progress bar) fetched with
// a dedicated Spotify login (see server/spotify_login.py), or - on wide
// panels - a split view showing both. POST /mode/usage, /mode/spotify,
// /mode/split or /mode/toggle - e.g. from the /switch Claude Code command -
// and the choice persists across power cycles.
//
// All rendering lives in ui.cpp (see ui.h); this file is networking, tokens,
// the beacon/mode HTTP server, and the main loop. Layout geometry derives
// from SCREEN_W / SCREEN_H in display.h, which also selects the panel driver
// per board (see the envs in platformio.ini).

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <ctype.h>

#include "display.h"
#include "config.h"
#include "ui.h"

DisplayTFT tft;

static const char *USAGE_URL = "https://api.anthropic.com/api/oauth/usage";
static const char *TOKEN_URL = "https://console.anthropic.com/v1/oauth/token";
static const char *OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";  // Claude Code's public client

// Spotify (only used when SPOTIFY_* are set in config.h).
static const char *SPOTIFY_TOKEN_URL = "https://accounts.spotify.com/api/token";
static const char *SPOTIFY_NOW_URL =
    "https://api.spotify.com/v1/me/player/currently-playing?additional_types=episode";

// Album art fetch cap. The wide layouts show the 640px variant (~100 KB
// jpeg); the small screens use the 64px one. The ESP32-P4 has PSRAM to spare,
// the C6 does not.
#if LAYOUT_WIDE
static const int ART_MAX_BYTES = 262144;
#else
static const int ART_MAX_BYTES = 60000;
#endif

// OAuth tokens. The usage endpoint needs a user:profile-scoped token, which the
// device gets from DEVICE_REFRESH_TOKEN (minted by server/device_login.py). The
// access token lasts only ~8h, so the device refreshes it itself and remembers
// the rotated refresh token in flash (NVS) - it's a separate login from your
// Mac's, so this never disturbs Claude Code there.
static Preferences prefs;
static String g_access;
static String g_refresh;
static long   g_expiresAt = 0;  // epoch seconds when g_access expires

// Spotify tokens, refreshed the same way. The login is a PKCE app, so only the
// client id is needed - no secret ever touches the device.
static String g_spAccess;
static String g_spRefresh;
static long   g_spExpiresAt = 0;

static Usage cur;

static NowPlaying np;
static unsigned long npFetchedAt = 0;   // millis() when np.progressMs was current
static unsigned long spLastPoll = 0;
static unsigned long spBackoff = 0;     // 429 cooldown, like pollBackoff
static unsigned long spLastOk = 0;

static WebServer beacon(BEACON_PORT);
static volatile unsigned long lastBeacon = 0;  // millis() of the last "thinking" ping

static unsigned long lastPoll = 0;
static unsigned long pollBackoff = 0;   // >0 => wait this long before next poll (429 cooldown)
static unsigned long lastOkFetch = 0;
static uint32_t lastLed = 0xFFFFFFFFu;  // cache so we only push the LED on change

// ---------------------------------------------------------------- LED

static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
#if RGB_LED_PIN >= 0
    uint32_t packed = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (packed == lastLed) return;  // neopixelWrite re-clocks the LED; skip no-ops
    lastLed = packed;
#if RGB_LED_SWAP_RG
    neopixelWrite(RGB_LED_PIN, g, r, b);  // board wires R/G swapped
#else
    neopixelWrite(RGB_LED_PIN, r, g, b);
#endif
#else
    (void)r; (void)g; (void)b;
#endif
}

static const int RGB_LED_MAX   = 70;     // peak green brightness (full is harsh)
static const int LED_BREATHE_MS = 3500;  // one inhale+exhale

// Breathe green while a beacon is live, otherwise off.
static void updateLed(bool active, unsigned long now) {
    if (!active) {
        ledSet(0, 0, 0);
        return;
    }
    // Smooth 0->1->0 over LED_BREATHE_MS; squared for a more natural ease that
    // lingers dim, like breathing rather than a triangle fade.
    float phase = (now % LED_BREATHE_MS) / (float)LED_BREATHE_MS;  // 0..1
    float level = 0.5f - 0.5f * cosf(phase * 2.0f * PI);           // 0..1..0
    level *= level;
    ledSet(0, (uint8_t)(level * RGB_LED_MAX + 0.5f), 0);
}

// ---------------------------------------------------------------- time

// resets_at arrives as ISO-8601 UTC, e.g. "2026-06-24T06:30:00.5+00:00".
// Render it in the configured local timezone like the old server did.
static time_t utc_from_tm(const struct tm *t) {
    static const int mdays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    long year = t->tm_year + 1900;
    long days = (year - 1970) * 365 + (year - 1969) / 4
                - (year - 1901) / 100 + (year - 1601) / 400;
    days += mdays[t->tm_mon] + (t->tm_mday - 1);
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (t->tm_mon > 1 && leap) days += 1;
    return ((time_t)days * 24 + t->tm_hour) * 3600 + t->tm_min * 60 + t->tm_sec;
}

static void fmtReset(const char *iso, char *out, size_t n) {
    out[0] = '\0';
    if (!iso || !iso[0]) return;
    struct tm t = {};
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) < 5) return;
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    time_t when = utc_from_tm(&t);

    struct tm local;
    localtime_r(&when, &local);
    time_t nowt = time(nullptr);
    struct tm nowlocal;
    localtime_r(&nowt, &nowlocal);

    // Only say "today" once NTP has actually set the clock (year >= 2024).
    bool synced = nowlocal.tm_year + 1900 >= 2024;
    char buf[32];
    if (synced && local.tm_year == nowlocal.tm_year && local.tm_yday == nowlocal.tm_yday)
        strftime(buf, sizeof(buf), "%I:%M %p", &local);          // "03:00 PM"
    else
        strftime(buf, sizeof(buf), "%a %I:%M %p", &local);       // "Wed 09:00 AM"

    // Trim a leading zero on the hour ("03:00" -> "3:00") to match the old look.
    char *colon = strchr(buf, ':');
    if (colon) {
        char *hour = colon;  // walk back over the hour digits before ':'
        while (hour > buf && isdigit((unsigned char)*(hour - 1))) hour--;
        if (colon - hour == 2 && hour[0] == '0') memmove(hour, hour + 1, strlen(hour));
    }
    strlcpy(out, buf, n);
}

// ---------------------------------------------------------------- modes

static bool spotifyConfigured() { return g_spRefresh.length() > 0; }

// Flip between the usage / spotify / split screens (persisted in NVS).
static void applyMode(DisplayMode m) {
    if (m == uiMode()) return;
    prefs.putUChar("mode", (uint8_t)m);
    uiSetMode(m);
    if (m == MODE_USAGE) {
        char msg[48];
        snprintf(msg, sizeof(msg), "%s.local  %s", MDNS_NAME,
                 WiFi.localIP().toString().c_str());
        uiStatus(msg, COL_DIM);
    } else {
        spLastPoll = 0;  // fetch as soon as loop() comes back around
        spBackoff = 0;
        uiStatus("fetching spotify...", COL_DIM);
    }
}

// ---------------------------------------------------------------- oauth tokens

static void loadTokens() {
    prefs.begin("clt", false);
    g_refresh   = prefs.getString("refresh", "");
    g_access    = prefs.getString("access", "");
    g_expiresAt = prefs.getLong("exp", 0);
    if (g_refresh.length() == 0) g_refresh = DEVICE_REFRESH_TOKEN;  // first boot: seed from config.h

    g_spRefresh   = prefs.getString("sp_refresh", "");
    g_spAccess    = prefs.getString("sp_access", "");
    g_spExpiresAt = prefs.getLong("sp_exp", 0);
    if (g_spRefresh.length() == 0) g_spRefresh = SPOTIFY_REFRESH_TOKEN;

    DisplayMode m = (DisplayMode)prefs.getUChar("mode", MODE_USAGE);
    if ((m == MODE_SPOTIFY || m == MODE_SPLIT) && !spotifyConfigured()) m = MODE_USAGE;
    if (m == MODE_SPLIT && !LAYOUT_WIDE) m = MODE_USAGE;
    if (m > MODE_SPLIT) m = MODE_USAGE;
    uiSetModeInitial(m);  // display isn't up yet; setup() paints it
}

static void saveTokens() {
    prefs.putString("refresh", g_refresh);
    prefs.putString("access", g_access);
    prefs.putLong("exp", g_expiresAt);
}

// Exchange a refresh token for a fresh access token (and the rotated refresh
// token), Claude Code-style. Persists the result. Returns true on success.
static bool tryRefresh(const String &refreshTok) {
    if (refreshTok.length() == 0) return false;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (!http.begin(client, TOKEN_URL)) return false;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "claude-usage-display/1.0");  // Cloudflare 1010-blocks the default UA
    String body = String("{\"grant_type\":\"refresh_token\",\"refresh_token\":\"") +
                  refreshTok + "\",\"client_id\":\"" + OAUTH_CLIENT_ID + "\"}";
    int code = http.POST(body);
    if (code != 200) { http.end(); return false; }
    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    const char *at = doc["access_token"] | "";
    if (!at[0]) return false;
    g_access = at;
    const char *rt = doc["refresh_token"] | "";
    if (rt[0]) g_refresh = rt;  // refresh tokens rotate; keep the new one
    long expires_in = doc["expires_in"] | 28800;       // ~8h default
    g_expiresAt = (long)time(nullptr) + expires_in;
    saveTokens();
    return true;
}

// Make sure g_access is valid, refreshing if it's missing or about to expire.
// Falls back to the config.h token if the stored (rotated) one is rejected -
// e.g. after a full flash erase wiped NVS but left a fresh token in config.h.
static bool ensureAccessToken() {
    time_t now = time(nullptr);
    bool synced = now > 1700000000;  // NTP set the clock (after ~2023)
    if (g_access.length() && synced && now < g_expiresAt - 300) return true;
    if (tryRefresh(g_refresh)) return true;
    String cfg = DEVICE_REFRESH_TOKEN;
    if (g_refresh != cfg && tryRefresh(cfg)) return true;
    return false;
}

// Spotify's equivalent of tryRefresh(): swap the refresh token for an access
// token. PKCE app, so the body is form-encoded and carries no client secret.
// Spotify sometimes rotates the refresh token too; keep whatever comes back.
static bool trySpotifyRefresh(const String &refreshTok) {
    if (refreshTok.length() == 0 || strlen(SPOTIFY_CLIENT_ID) == 0) return false;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (!http.begin(client, SPOTIFY_TOKEN_URL)) return false;
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = String("grant_type=refresh_token&refresh_token=") + refreshTok +
                  "&client_id=" + SPOTIFY_CLIENT_ID;
    int code = http.POST(body);
    if (code != 200) { http.end(); return false; }
    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    const char *at = doc["access_token"] | "";
    if (!at[0]) return false;
    g_spAccess = at;
    const char *rt = doc["refresh_token"] | "";
    if (rt[0]) g_spRefresh = rt;
    long expires_in = doc["expires_in"] | 3600;
    g_spExpiresAt = (long)time(nullptr) + expires_in;
    prefs.putString("sp_refresh", g_spRefresh);
    prefs.putString("sp_access", g_spAccess);
    prefs.putLong("sp_exp", g_spExpiresAt);
    return true;
}

static bool ensureSpotifyToken() {
    time_t now = time(nullptr);
    bool synced = now > 1700000000;
    if (g_spAccess.length() && synced && now < g_spExpiresAt - 300) return true;
    if (trySpotifyRefresh(g_spRefresh)) return true;
    String cfg = SPOTIFY_REFRESH_TOKEN;
    if (cfg.length() && g_spRefresh != cfg && trySpotifyRefresh(cfg)) return true;
    return false;
}

// ---------------------------------------------------------------- network

// One GET to the usage endpoint with the current access token. Returns the HTTP
// status (200 ok), or a negative HTTPClient error. On 429/403, retryAfter is
// set to the server's requested cooldown in seconds.
static int usageRequest(Usage &u, int &retryAfter) {
    retryAfter = 0;
    WiFiClientSecure client;
    client.setInsecure();  // skip CA validation; fine on a home LAN to a known host

    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    if (!http.begin(client, USAGE_URL)) return -1;
    http.addHeader("Authorization", String("Bearer ") + g_access);
    http.addHeader("anthropic-beta", "oauth-2025-04-20");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "claude-usage-display/1.0");
    const char *collect[] = {"Retry-After"};
    http.collectHeaders(collect, 1);

    int code = http.GET();
    if (code != 200) {
        if (code == 429 || code == 403) retryAfter = http.header("Retry-After").toInt();
        http.end();
        return code;
    }
    String body = http.getString();
    http.end();

    // Only pull the fields we render, so a big response stays cheap to parse.
    JsonDocument filter;
    for (const char *k : {"five_hour", "seven_day"}) {
        filter[k]["utilization"] = true;
        filter[k]["resets_at"] = true;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return -2;

    u.fivePct = doc["five_hour"]["utilization"] | -1.0f;
    u.weekPct = doc["seven_day"]["utilization"] | -1.0f;
    fmtReset(doc["five_hour"]["resets_at"] | "", u.fiveReset, sizeof(u.fiveReset));
    fmtReset(doc["seven_day"]["resets_at"] | "", u.weekReset, sizeof(u.weekReset));
    u.valid = true;
    return 200;
}

// Fetch usage, getting/refreshing the access token as needed. Returns the HTTP
// status (200 ok), -3 if no valid token could be obtained, or a negative
// HTTPClient error. A 401 means the token went stale mid-flight: refresh once
// and retry.
static int fetchUsage(Usage &u, int &retryAfter) {
    retryAfter = 0;
    if (!ensureAccessToken()) return -3;
    int code = usageRequest(u, retryAfter);
    if (code == 401) {
        g_expiresAt = 0;  // force a refresh
        if (ensureAccessToken()) code = usageRequest(u, retryAfter);
    }
    return code;
}

// Pick the art variant to fetch: the smallest one that's still >= the
// on-screen size (Spotify lists images largest first; albums ship 640/300/64).
// Falls back to the largest available if nothing reaches it.
static void pickArt(JsonArray imgs, NowPlaying &out) {
    const int want = uiArtSizePx();
    long best = 0;
    for (JsonObject im : imgs) {
        const char *u = im["url"] | "";
        long w = im["width"] | 0L;
        if (!u[0]) continue;
        bool haveGood = best >= want;
        bool thisGood = w >= want;
        bool better = !out.artUrl[0] ||
                      (thisGood && !haveGood) ||
                      (thisGood && haveGood && w < best) ||
                      (!thisGood && !haveGood && w > best);
        if (better) {
            best = w;
            out.artW = w;
            strlcpy(out.artUrl, u, sizeof(out.artUrl));
        }
    }
}

// One GET to Spotify's currently-playing endpoint. 204 means nothing is
// playing - that's a success, just an empty one.
static int spotifyRequest(NowPlaying &out, int &retryAfter) {
    retryAfter = 0;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    if (!http.begin(client, SPOTIFY_NOW_URL)) return -1;
    http.addHeader("Authorization", String("Bearer ") + g_spAccess);
    const char *collect[] = {"Retry-After"};
    http.collectHeaders(collect, 1);

    int code = http.GET();
    if (code == 204) {
        http.end();
        out = NowPlaying();
        out.valid = true;
        return 200;
    }
    if (code != 200) {
        if (code == 429) retryAfter = http.header("Retry-After").toInt();
        http.end();
        return code;
    }
    String body = http.getString();
    http.end();

    // The full response is several KB of album art URLs etc.; filter it down
    // to the handful of fields we render.
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["progress_ms"] = true;
    filter["item"]["name"] = true;
    filter["item"]["duration_ms"] = true;
    filter["item"]["artists"][0]["name"] = true;
    filter["item"]["album"]["name"] = true;
    filter["item"]["album"]["images"][0]["url"] = true;
    filter["item"]["album"]["images"][0]["width"] = true;
    filter["item"]["show"]["name"] = true;  // podcast episodes have a show, not artists
    filter["item"]["images"][0]["url"] = true;   // ...and their art hangs off the item
    filter["item"]["images"][0]["width"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return -2;

    out = NowPlaying();
    out.valid = true;
    out.hasTrack = !doc["item"].isNull();
    if (!out.hasTrack) return 200;
    out.playing = doc["is_playing"] | false;
    out.progressMs = doc["progress_ms"] | 0L;
    out.durationMs = doc["item"]["duration_ms"] | 0L;
    asciiCopy(out.track, sizeof(out.track), doc["item"]["name"] | "");
    asciiCopy(out.album, sizeof(out.album), doc["item"]["album"]["name"] | "");
    for (JsonObject a : doc["item"]["artists"].as<JsonArray>()) {
        char nm[48];
        asciiCopy(nm, sizeof(nm), a["name"] | "");
        if (!nm[0]) continue;
        if (out.artist[0]) strlcat(out.artist, ", ", sizeof(out.artist));
        strlcat(out.artist, nm, sizeof(out.artist));
    }
    if (!out.artist[0])
        asciiCopy(out.artist, sizeof(out.artist), doc["item"]["show"]["name"] | "");
    pickArt(doc["item"]["album"]["images"].as<JsonArray>(), out);
    if (!out.artUrl[0]) pickArt(doc["item"]["images"].as<JsonArray>(), out);
    return 200;
}

static int fetchNowPlaying(NowPlaying &out, int &retryAfter) {
    retryAfter = 0;
    if (!ensureSpotifyToken()) return -3;
    int code = spotifyRequest(out, retryAfter);
    if (code == 401) {
        g_spExpiresAt = 0;  // force a refresh
        if (ensureSpotifyToken()) code = spotifyRequest(out, retryAfter);
    }
    return code;
}

// Fetch a jpeg the renderer asked for (see uiArtWanted) and hand it over.
// Blocking for ~1s on a track change; on any failure the placeholder just
// stays. Only the compressed image sits in RAM, and it's freed on every path.
static void fetchAlbumArt(const char *url) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(5000);
    if (!http.begin(client, url)) return;
    if (http.GET() != 200) { http.end(); return; }
    int len = http.getSize();
    if (len <= 0 || len > ART_MAX_BYTES) { http.end(); return; }

    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { http.end(); return; }
    WiFiClient *stream = http.getStreamPtr();
    int got = 0;
    unsigned long deadline = millis() + 6000;
    while (got < len && (long)(deadline - millis()) > 0) {
        int avail = stream->available();
        if (avail > 0) {
            int want = len - got;
            if (want > avail) want = avail;
            int n = stream->read(buf + got, want);
            if (n > 0) got += n;
        } else if (!client.connected()) {
            break;
        } else {
            delay(1);
        }
    }
    http.end();

    if (got == len) uiPushArt(buf, len, np.artW);
    free(buf);
}

// ---------------------------------------------------------------- beacons

// /thinking and /thinking/on -> Claude is working (refresh the keep-alive).
static void handleThinkingOn() {
    lastBeacon = millis();
    beacon.send(200, "text/plain", "on\n");
}

// /thinking/off -> Claude stopped: go idle immediately, no trailing timeout.
static void handleThinkingOff() {
    lastBeacon = 0;
    beacon.send(200, "text/plain", "off\n");
}

static void handleRoot() {
    beacon.send(200, "text/plain",
                "Claude Code usage display. POST /thinking/on while working, "
                "/thinking/off when done. POST /mode/usage, /mode/spotify, "
                "/mode/split or /mode/toggle to switch screens; GET /mode to ask.\n");
}

// ---- screen mode switching (used by the /switch Claude Code command) ----

static const char *modeName(DisplayMode m) {
    if (m == MODE_SPOTIFY) return "spotify";
    if (m == MODE_SPLIT) return "split";
    return "usage";
}

static void handleModeGet() {
    beacon.send(200, "text/plain", String(modeName(uiMode())) + "\n");
}

static void handleModeUsage() {
    applyMode(MODE_USAGE);
    beacon.send(200, "text/plain", "usage\n");
}

static bool spotifySetupOk() {
    if (spotifyConfigured()) return true;
    beacon.send(409, "text/plain",
                "spotify not configured - run server/spotify_login.py and "
                "set SPOTIFY_CLIENT_ID / SPOTIFY_REFRESH_TOKEN in config.h\n");
    return false;
}

static void handleModeSpotify() {
    if (!spotifySetupOk()) return;
    applyMode(MODE_SPOTIFY);
    beacon.send(200, "text/plain", "spotify\n");
}

static void handleModeSplit() {
#if LAYOUT_WIDE
    if (!spotifySetupOk()) return;
    applyMode(MODE_SPLIT);
    beacon.send(200, "text/plain", "split\n");
#else
    beacon.send(409, "text/plain",
                "split mode needs a wide (1920x480) panel\n");
#endif
}

// usage -> spotify -> split (wide panels) -> usage
static void handleModeToggle() {
    switch (uiMode()) {
        case MODE_USAGE:   handleModeSpotify(); break;
        case MODE_SPOTIFY:
            if (LAYOUT_WIDE) handleModeSplit();
            else handleModeUsage();
            break;
        default:           handleModeUsage(); break;
    }
}

// "Thinking" is sticky between an explicit on and off. BEACON_TTL_MS is only a
// backstop: if a sender dies mid-turn and never sends /thinking/off, fall idle.
static bool beaconActive(unsigned long now) {
    return lastBeacon != 0 && (now - lastBeacon) < BEACON_TTL_MS;
}

// ---------------------------------------------------------------- spotify tick

// Everything Spotify-side per loop(): poll on its own schedule and hand the
// results to the renderer; the progress bar ticks locally inside uiTick.
static void spotifyTick(unsigned long now) {
    unsigned long interval = spBackoff ? spBackoff : SPOTIFY_POLL_MS;
    if (spLastPoll == 0 || now - spLastPoll >= interval) {
        spLastPoll = now;
        if (WiFi.status() != WL_CONNECTED) {
            uiStatus("WiFi reconnecting...", COL_RED);
        } else if (!spotifyConfigured()) {
            uiStatus("spotify not set up - spotify_login.py", COL_YELLOW);
        } else {
            NowPlaying u;
            int retryAfter = 0;
            int code = fetchNowPlaying(u, retryAfter);
            if (code == 200) {
                spBackoff = 0;
                np = u;
                npFetchedAt = now;
                spLastOk = now;
                uiSetNowPlaying(np, npFetchedAt);
                char msg[48];
                snprintf(msg, sizeof(msg), "spotify ok  %s.local", MDNS_NAME);
                uiStatus(msg, COL_GREEN);
            } else if (code == 401 || code == 403 || code == -3) {
                // 403 usually means the Spotify app doesn't include this
                // account (Dashboard -> app -> User Management).
                uiStatus("spotify auth failed - spotify_login.py", COL_RED);
            } else if (code == 429) {
                unsigned long secs = (retryAfter > 0 ? (unsigned long)retryAfter : 30) + 2;
                spBackoff = secs * 1000UL;
                char msg[48];
                snprintf(msg, sizeof(msg), "spotify rate limited, %lus", secs);
                uiStatus(msg, COL_YELLOW);
            } else if (now - spLastOk > 30000) {
                char msg[40];
                snprintf(msg, sizeof(msg), "spotify fetch failed (%d)", code);
                uiStatus(msg, COL_RED);
            }
        }
    }

    // Album art: the renderer says what it wants (once per track/mode).
    char artUrl[120];
    if (uiArtWanted(artUrl, sizeof(artUrl))) fetchAlbumArt(artUrl);

    // When the local estimate says the track ended, poll right away so the
    // next one shows up promptly.
    if (np.hasTrack && np.playing && np.durationMs > 0) {
        long est = np.progressMs + (long)(now - npFetchedAt);
        if (est > np.durationMs && now - spLastPoll > 3000) spLastPoll = 0;
    }
}

// ---------------------------------------------------------------- arduino

void setup() {
    Serial.begin(115200);
    ledSet(0, 0, 0);
    loadTokens();

    displayBoardInit();  // panel power / backlight (P4 DSI board); no-op elsewhere
    tft.init();
    tft.setRotation(SCREEN_ROTATION);  // C6 portrait=0 (2 if upside down); landscape boards=1 (3 if flipped)
    uiInit();
    uiFullRepaint();
    uiStatus("connecting to WiFi...", COL_DIM);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
        // Local time for reset formatting, plus mDNS + beacon listener.
        configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com", "time.nist.gov");
        if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", BEACON_PORT);
        beacon.on("/thinking", HTTP_POST, handleThinkingOn);
        beacon.on("/thinking", HTTP_GET, handleThinkingOn);
        beacon.on("/thinking/on", HTTP_POST, handleThinkingOn);
        beacon.on("/thinking/on", HTTP_GET, handleThinkingOn);
        beacon.on("/thinking/off", HTTP_POST, handleThinkingOff);
        beacon.on("/thinking/off", HTTP_GET, handleThinkingOff);
        beacon.on("/mode", handleModeGet);
        beacon.on("/mode/usage", handleModeUsage);
        beacon.on("/mode/spotify", handleModeSpotify);
        beacon.on("/mode/split", handleModeSplit);
        beacon.on("/mode/toggle", handleModeToggle);
        beacon.on("/", handleRoot);
        beacon.begin();

        char msg[48];
        snprintf(msg, sizeof(msg), "%s.local  %s", MDNS_NAME,
                 WiFi.localIP().toString().c_str());
        uiStatus(msg, COL_DIM);
    } else {
        uiStatus("WiFi failed - check config.h", COL_RED);
    }
}

void loop() {
    unsigned long now = millis();

    beacon.handleClient();

    // Usage keeps polling in every mode (so the bars are current the moment
    // you switch back); the renderer decides whether it's visible.
    bool showUsage = uiMode() != MODE_SPOTIFY;
    unsigned long interval = pollBackoff ? pollBackoff : USAGE_POLL_MS;
    if (lastPoll == 0 || now - lastPoll >= interval) {
        lastPoll = now;
        if (WiFi.status() == WL_CONNECTED) {
            Usage u;
            int retryAfter = 0;
            int code = fetchUsage(u, retryAfter);
            if (code == 200) {
                pollBackoff = 0;
                cur = u;
                uiSetUsage(cur);
                if (showUsage) {
                    char msg[48];
                    snprintf(msg, sizeof(msg), "usage ok  %s.local", MDNS_NAME);
                    uiStatus(msg, COL_GREEN);
                }
                lastOkFetch = now;
            } else if (code == 401 || code == -3) {
                if (showUsage) uiStatus("auth failed - run device_login.py", COL_RED);
            } else if (code == 429 || code == 403) {
                // A 403 here is the edge rate-limiter, not a real auth failure: a
                // valid token still gets it when hammered. Back off (plus a small
                // margin) so the cooldown actually expires instead of being
                // re-armed by the next poll. Default 10 min if no Retry-After.
                unsigned long secs = (retryAfter > 0 ? (unsigned long)retryAfter : 600) + 30;
                pollBackoff = secs * 1000UL;
                if (showUsage) {
                    char msg[48];
                    snprintf(msg, sizeof(msg), "rate limited, retry in %lus", secs);
                    uiStatus(msg, COL_YELLOW);
                }
            } else if (now - lastOkFetch > 90000) {
                if (showUsage) {
                    char msg[40];
                    snprintf(msg, sizeof(msg), "usage fetch failed (%d)", code);
                    uiStatus(msg, COL_RED);
                }
            }
        } else if (showUsage) {
            uiStatus("WiFi reconnecting...", COL_RED);
        }
    }

    if (uiMode() != MODE_USAGE) spotifyTick(now);

    // The LED breathes while Claude is thinking in every mode; the spinner
    // and working/idle word only exist where the usage panel is visible.
    bool active = beaconActive(now);
    uiTick(now, active);
    updateLed(active, now);

    delay(10);
}
