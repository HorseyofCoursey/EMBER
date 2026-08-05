#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "AudioFileSourceSD.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorFLAC.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutput.h"
#include "ember_logo.h"
#include "turntable_frames.h"
#include "dancer0_frames.h"
#include "dancer1_frames.h"
#include "dancer2_frames.h"
#include "dancer3_frames.h"

// ---------- SD pins (Cardputer ADV) ----------
static const int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;

// ---------- Navigation keys ----------
static const char KEY_UP = ';', KEY_DOWN = '.', KEY_BACK = ',', KEY_OPEN = '/';
static const char KEY_VOLUP = '=', KEY_VOLDN = '-';
static const char KEY_NEXT = 'n', KEY_PREV = 'b';
static const char KEY_NOWPLAYING = 'm';
static const char KEY_SETTINGS = 's';
static const char KEY_ART_TOGGLE = 'a';   // Now Playing only: force turntable vs real art
static const char KEY_FULLVIS = 'v';      // Now Playing only: toggle full-screen visualizer
static const char KEY_SCREENSHOT = 'c';   // any screen: save a BMP to SD; hold to burst-capture
// ENTER also opens/plays; backtick ` also goes back; SPACE = pause/resume

// ---------- Directory model (all static, no heap) ----------
static const int MAX_ENTRIES = 256, NAME_POOL_SIZE = 8192, MAX_DEPTH = 8, MY_PATH_MAX = 256;
static char     namePool[NAME_POOL_SIZE];
static uint16_t nameOffset[MAX_ENTRIES];
static bool     entryIsDir[MAX_ENTRIES];
static int      sortIdx[MAX_ENTRIES];         // alphabetized view into entries
static int      entryCount = 0, poolUsed = 0;

static char currentPath[MY_PATH_MAX] = "/";
static int  cursor = 0, scroll = 0, depth = 0;
static int  cursorStack[MAX_DEPTH], scrollStack[MAX_DEPTH];

// ---------- Play queue (the album that's currently playing) ----------
static const int QUEUE_MAX = 256, QNAME_POOL = 8192;
static char     queuePool[QNAME_POOL];        // track filenames for the playing album
static uint16_t queueOffset[QUEUE_MAX];
static int      queueCount = 0, queuePoolUsed = 0;
static char     queueFolder[MY_PATH_MAX] = ""; // folder the queue came from
static int      queuePos = 0;                  // index into queue currently playing

// ---------- Layout / colours ----------
// Browser rows are drawn in a different, larger font (see FONT_BROWSER below)
// than Now Playing/Settings, which stay at the original size -- these two
// constants are sized for FreeSans9pt7b's 22px line height (+ padding).
static const int HEADER_H = 28, ROW_H = 26;
static int visibleRows = 0;

// Battery/volume icons only ever occupy y=5..15 regardless of screen -- their
// own clear-rects use this fixed height rather than HEADER_H so they can't
// bleed into whatever sits below the header on a given screen. This used to
// just reuse HEADER_H, which was fine while HEADER_H was small, but bumping
// it to 28 for the browser's bigger font meant the icon clear-rect (also 28px
// tall) started reaching down into the Now Playing artist line at y=22,
// blacking out its top few pixels a few seconds after any icon redraw fired.
static const int ICON_STRIP_H = 20;

// The browser (list + its header) is drawn a size up from everything else, to
// make the list more readable -- Now Playing and Settings stay as-is since
// their layout is already tuned and considered done. Each top-level draw
// function sets its own font explicitly at the start rather than relying on
// whatever a previous screen left active, so there's no ordering dependency.
//
// FONT_BROWSER (FreeSans9pt7b) is Latin-only -- GFXfont tables here simply
// have no CJK glyphs -- so any name containing non-ASCII bytes falls back to
// FONT_UI (Gothic, which does) via textFontFor() below rather than printing
// tofu boxes.
static const lgfx::IFont* FONT_UI      = &fonts::lgfxJapanGothic_16;
static const lgfx::IFont* FONT_BROWSER = &fonts::FreeSans9pt7b;
static bool needsRedraw = true;

// ---------- Theme ----------
// Colors used to be plain TFT_* compile-time constants; they're now fields on
// a runtime-swappable Theme struct instead, so a future "pick your own colors"
// tool can swap the whole palette without touching any drawing code -- every
// COL_* usage site below is unchanged, the macros just resolve through
// `theme` now. Battery charge level intentionally stays a fixed semantic
// TFT_* color (it communicates real device state, not aesthetics) -- the
// visualizer's level tiers, on the other hand, are just palette choices, so
// they're theme fields like everything else.
struct Theme {
    uint16_t bg, header, headerText, folder, file, selBg, selFg, dim, play;
    uint16_t npText, volumeIcon, progressDot;
    uint16_t visMid, visHigh;
    uint16_t npBg, visIdle;
};

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Warm amber/ember palette on near-black, matching the "Ember" name.
// Generated via tools/theme-editor.html, preset "Ember".
static const Theme THEME_EMBER = {
    rgb565(12, 8, 6),          // bg
    rgb565(42, 24, 14),        // header
    rgb565(245, 230, 215),     // headerText
    rgb565(255, 158, 56),      // folder
    rgb565(233, 219, 205),     // file
    rgb565(196, 88, 22),       // selBg
    rgb565(255, 244, 230),     // selFg
    rgb565(122, 98, 82),       // dim
    rgb565(130, 210, 100),     // play
    rgb565(255, 255, 255),     // npText
    rgb565(255, 255, 255),     // volumeIcon
    rgb565(255, 244, 230),     // progressDot
    rgb565(255, 255, 0),       // visMid
    rgb565(255, 0, 0),         // visHigh
    rgb565(12, 8, 6),          // npBg
    rgb565(135, 120, 110),     // visIdle
};
// Generated via tools/theme-editor.html, preset "90's Sweater".
static const Theme THEME_90S_SWEATER = {
    rgb565(160, 41, 43),       // bg
    rgb565(52, 92, 102),       // header
    rgb565(255, 242, 174),     // headerText
    rgb565(83, 11, 14),        // folder
    rgb565(255, 241, 177),     // file
    rgb565(154, 167, 140),     // selBg
    rgb565(255, 244, 230),     // selFg
    rgb565(83, 11, 14),        // dim
    rgb565(83, 11, 14),        // play
    rgb565(255, 255, 255),     // npText
    rgb565(52, 92, 102),       // volumeIcon
    rgb565(52, 92, 102),       // progressDot
    rgb565(154, 167, 140),     // visMid
    rgb565(255, 241, 178),     // visHigh
    rgb565(154, 39, 39),       // npBg
    rgb565(52, 92, 102),       // visIdle
};

// Generated via tools/theme-editor.html, preset "Aqua".
static const Theme THEME_AQUA = {
    rgb565(33, 192, 192),      // bg
    rgb565(65, 82, 214),       // header
    rgb565(1, 209, 209),       // headerText
    rgb565(0, 91, 92),         // folder
    rgb565(0, 91, 92),         // file
    rgb565(65, 82, 214),       // selBg
    rgb565(255, 244, 230),     // selFg
    rgb565(0, 173, 174),       // dim
    rgb565(65, 82, 214),       // play
    rgb565(65, 82, 214),       // npText
    rgb565(65, 82, 214),       // volumeIcon
    rgb565(65, 82, 214),       // progressDot
    rgb565(125, 134, 202),     // visMid
    rgb565(176, 212, 212),     // visHigh
    rgb565(2, 9, 44),          // npBg
    rgb565(47, 61, 117),       // visIdle
};
// Generated via tools/theme-editor.html, preset "Honey".
static const Theme THEME_HONEY = {
    rgb565(249, 223, 118),     // bg
    rgb565(255, 199, 33),      // header
    rgb565(137, 81, 42),       // headerText
    rgb565(137, 81, 42),       // folder
    rgb565(137, 81, 42),       // file
    rgb565(137, 81, 42),       // selBg
    rgb565(255, 244, 230),     // selFg
    rgb565(137, 81, 42),       // dim
    rgb565(137, 81, 42),       // play
    rgb565(137, 81, 42),       // npText
    rgb565(137, 81, 42),       // volumeIcon
    rgb565(137, 81, 42),       // progressDot
    rgb565(255, 199, 33),      // visMid
    rgb565(255, 252, 207),     // visHigh
    rgb565(249, 223, 118),     // npBg
    rgb565(249, 223, 118),     // visIdle
};
// Generated via tools/theme-editor.html, preset "Moody".
static const Theme THEME_MOODY = {
    rgb565(12, 8, 6),          // bg
    rgb565(93, 79, 79),        // header
    rgb565(210, 208, 209),     // headerText
    rgb565(152, 134, 134),     // folder
    rgb565(233, 219, 205),     // file
    rgb565(152, 134, 134),     // selBg
    rgb565(255, 244, 230),     // selFg
    rgb565(152, 134, 134),     // dim
    rgb565(93, 79, 79),        // play
    rgb565(255, 255, 255),     // npText
    rgb565(255, 255, 255),     // volumeIcon
    rgb565(255, 244, 230),     // progressDot
    rgb565(152, 134, 134),     // visMid
    rgb565(210, 208, 209),     // visHigh
    rgb565(12, 8, 6),          // npBg
    rgb565(8, 8, 0),           // visIdle
};
// Generated via tools/theme-editor.html, preset "Terminal Green".
static const Theme THEME_TERMINAL_GREEN = {
    rgb565(12, 8, 6),          // bg
    rgb565(0, 0, 0),           // header
    rgb565(0, 235, 16),        // headerText
    rgb565(0, 235, 16),        // folder
    rgb565(0, 235, 16),        // file
    rgb565(0, 235, 16),        // selBg
    rgb565(0, 0, 0),           // selFg
    rgb565(10, 189, 22),       // dim
    rgb565(0, 235, 16),        // play
    rgb565(0, 235, 16),        // npText
    rgb565(0, 235, 16),        // volumeIcon
    rgb565(0, 235, 16),        // progressDot
    rgb565(0, 148, 10),        // visMid
    rgb565(0, 82, 5),          // visHigh
    rgb565(12, 8, 6),          // npBg
    rgb565(0, 0, 0),           // visIdle
};

// All available themes, cycled from Settings -> Theme; Ember is always the
// default at boot (index 0), regardless of what's added after it.
static const Theme* const THEME_LIST[] = { &THEME_EMBER, &THEME_90S_SWEATER, &THEME_AQUA, &THEME_HONEY, &THEME_MOODY, &THEME_TERMINAL_GREEN };
static const char* THEME_LABELS[] = { "Ember", "90's Sweater", "Aqua", "Honey", "Moody", "Terminal Green" };
static const int THEME_COUNT = sizeof(THEME_LIST) / sizeof(THEME_LIST[0]);

// User themes loaded from /themes/*.json on SD at boot (see loadCustomThemes()
// near setup(), below -- it needs SD/File which aren't declared yet up here).
// Appended after the built-in list rather than replacing anything in it.
static const int MAX_CUSTOM_THEMES = 12;
static Theme customThemes[MAX_CUSTOM_THEMES];
static char customThemeNames[MAX_CUSTOM_THEMES][24];
static int customThemeCount = 0;

static int totalThemeCount() { return THEME_COUNT + customThemeCount; }
static const char* themeLabelAt(int idx) {
    return (idx < THEME_COUNT) ? THEME_LABELS[idx] : customThemeNames[idx - THEME_COUNT];
}
static const Theme& themeAt(int idx) {
    return (idx < THEME_COUNT) ? *THEME_LIST[idx] : customThemes[idx - THEME_COUNT];
}

static Theme theme = THEME_EMBER;

#define COL_BG          theme.bg
#define COL_HEADER      theme.header
#define COL_FOLDER      theme.folder
#define COL_FILE        theme.file
#define COL_SEL_BG      theme.selBg
#define COL_SEL_FG      theme.selFg
#define COL_DIM         theme.dim
#define COL_PLAY        theme.play
#define COL_NP_TEXT     theme.npText
#define COL_VOLUME      theme.volumeIcon
#define COL_PROGRESS    theme.progressDot
#define COL_VIS_MID     theme.visMid
#define COL_VIS_HIGH    theme.visHigh
#define COL_NP_BG       theme.npBg
#define COL_VIS_IDLE    theme.visIdle

// ---------- Playback state ----------
enum PlayState { STOPPED, PLAYING, PAUSED };
static PlayState playState = STOPPED;
static char nowPlaying[64] = "";
static int  volume = 50;                       // 0..255 (~20%)

enum UiMode { MODE_BROWSER, MODE_NOWPLAYING, MODE_SETTINGS, MODE_FULLVIS };
static UiMode uiMode = MODE_BROWSER;
static UiMode uiModeBeforeSettings = MODE_BROWSER;   // where to return to on back/'s'

// ---------- Settings ----------
// Persisted to NVS (ESP32's flash-backed key/value store, via the Preferences
// library) so they survive a reboot -- see loadSettings()/saveSettings() near
// setup(), below. More settings are expected to be added here over time.
enum AlbumEndMode { ALBUM_LOOP, ALBUM_STOP, ALBUM_NEXT, ALBUM_END_MODE_COUNT };
static const char* albumEndLabels[ALBUM_END_MODE_COUNT] = { "Loop album", "Stop", "Next album" };
static AlbumEndMode albumEndMode = ALBUM_STOP;   // default: nothing happens, playback just stops

static const uint8_t  backlightValues[] = { 64, 128, 192, 255 };
static const char*    backlightLabels[] = { "25%", "50%", "75%", "100%" };
static const int BACKLIGHT_COUNT = sizeof(backlightValues) / sizeof(backlightValues[0]);
static int settingBacklightIdx = BACKLIGHT_COUNT - 1;   // default 100%

static const uint32_t screenOffTimeoutMs[] = { 0, 10000, 30000, 60000 };
static const char*    screenOffLabels[]    = { "Never", "10 sec", "30 sec", "60 sec" };
static const int SCREEN_OFF_COUNT = sizeof(screenOffTimeoutMs) / sizeof(screenOffTimeoutMs[0]);
static int settingScreenOffIdx = 2;   // default 30 sec

static int settingThemeIdx = 0;   // default Ember (THEME_LIST[0])

// Off by default -- screenshot capture reads the framebuffer back over SPI
// (see saveScreenshot()), which most people flashing this will never use,
// so it shouldn't be live by default just because the 'c' key happens to be
// otherwise unbound.
static const char* onOffLabels[] = { "Off", "On" };
static bool settingScreenshotsEnabled = false;

static const int SETTINGS_COUNT = 5;
static int settingsCursor = 0;

// Rows visible at once in the settings box -- SETTINGS_COUNT no longer fits
// the whole list on screen at once (5 rows * ROW_H would be taller than the
// 135px display), so it scrolls like the browser list does, just over a
// much shorter list.
static const int SETTINGS_VISIBLE = 3;
static int settingsScroll = 0;

static Preferences settingsPrefs;

// Called once at boot, before the theme index is validated against
// totalThemeCount() (custom themes from SD haven't loaded yet at that
// point -- see setup()). Backlight/screen-off/album-end are clamped here
// since their bounds are fixed at compile time either way.
static void loadSettings() {
    settingsPrefs.begin("ember", true);
    settingBacklightIdx = settingsPrefs.getInt("backlight", BACKLIGHT_COUNT - 1);
    settingScreenOffIdx = settingsPrefs.getInt("screenOff", 2);
    albumEndMode = (AlbumEndMode)settingsPrefs.getInt("albumEnd", ALBUM_STOP);
    settingThemeIdx = settingsPrefs.getInt("themeIdx", 0);
    settingScreenshotsEnabled = settingsPrefs.getBool("screenshots", false);
    settingsPrefs.end();

    if (settingBacklightIdx < 0 || settingBacklightIdx >= BACKLIGHT_COUNT) settingBacklightIdx = BACKLIGHT_COUNT - 1;
    if (settingScreenOffIdx < 0 || settingScreenOffIdx >= SCREEN_OFF_COUNT) settingScreenOffIdx = 2;
    if (albumEndMode < 0 || albumEndMode >= ALBUM_END_MODE_COUNT) albumEndMode = ALBUM_STOP;
    if (settingThemeIdx < 0) settingThemeIdx = 0;   // re-clamped against totalThemeCount() once custom themes load
}

// Called after every settings change (see cycleSetting()) -- infrequent,
// user-driven writes, so no need to batch/debounce these.
static void saveSettings() {
    settingsPrefs.begin("ember", false);
    settingsPrefs.putInt("backlight", settingBacklightIdx);
    settingsPrefs.putInt("screenOff", settingScreenOffIdx);
    settingsPrefs.putInt("albumEnd", (int)albumEndMode);
    settingsPrefs.putInt("themeIdx", settingThemeIdx);
    settingsPrefs.putBool("screenshots", settingScreenshotsEnabled);
    settingsPrefs.end();
}

static bool screenIsOff = false;
static unsigned long lastInputTime = 0;

// ID3 metadata for the currently playing track (populated via RegisterMetadataCB)
static char curArtist[96] = "";
static char curTitle[96]  = "";
static char curAlbum[96]  = "";

// Which decoder is currently loaded -- decoder is a base AudioGenerator*
// since begin()/loop()/isRunning()/stop() are all virtual and identical
// across formats; curFormat exists only for the handful of format-specific
// spots (which concrete class to `new`, and MP3's desync()-based seeking,
// which the other decoders have no equivalent for).
enum AudioFormat { FMT_MP3, FMT_FLAC, FMT_WAV };
static AudioFormat        curFormat = FMT_MP3;
static AudioGenerator     *decoder = nullptr;
static AudioFileSourceSD  *file = nullptr;
static AudioFileSourceID3 *id3  = nullptr;

// ---------- Visualizer level history ----------
// A cheap amplitude-based visualizer: no FFT, just the average |sample| of each
// tri-buffer chunk (~46ms at 44.1kHz), fed into a small scrolling history that
// the Now Playing screen renders as bars. Updated from AudioOutputM5Speaker's
// flush(), which runs synchronously inside decoder->loop() -- same single audio
// thread as everything else, no locking needed.
static const int VIS_HISTORY = 14;
static float visHistory[VIS_HISTORY] = {0};
// 0 = all treble, 1 = all bass, for that chunk. Computed and kept up to date,
// but not currently used to color bars (tried it -- distracting per feedback).
// Parked here as a future "visualizer color mode" setting rather than ripped
// out, since the DSP side is already cheap and working.
static float visBassRatio[VIS_HISTORY] = {0};

static void pushVisLevel(float level, float bassRatio) {
    for (int i = 0; i < VIS_HISTORY - 1; i++) {
        visHistory[i] = visHistory[i + 1];
        visBassRatio[i] = visBassRatio[i + 1];
    }
    visHistory[VIS_HISTORY - 1] = level;
    visBassRatio[VIS_HISTORY - 1] = bassRatio;
}

// Raw stereo PCM snapshot for the full-screen FFT visualizer (see fft_t /
// drawFullVis below) -- separate from the amplitude-history path above,
// which only keeps a per-chunk scalar and throws the samples away. Captured
// once per tri-buffer chunk (~23ms of audio) in AudioOutputM5Speaker::flush(),
// from the same single audio thread, so no locking needed here either.
static const int VISRAW_FRAMES = 320;   // stereo frames; matches fft_t's needs (2*FFT_SIZE) with room to spare
static int16_t visRawBuf[VISRAW_FRAMES * 2];

// Raw per-chunk bass envelope (not the bass/treble *ratio* above -- the
// actual magnitude), for the dancer visualizer's beat-reactive stepping.
// Same one-pole low-pass tap as visBassRatio, just exposed unblended.
static float lastBassEnvelope = 0.0f;

// ---------- Custom AudioOutput: triple-buffered feed to ES8311 via M5 Speaker ----------
class AudioOutputM5Speaker : public AudioOutput {
public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t ch = 0) { _m5sound = m5sound; _virtual_ch = ch; }
    bool begin() override { return true; }
    bool ConsumeSample(int16_t sample[2]) override {
        if (_tri_buffer_index < tri_buf_size) {
            _tri_buffer[_tri_index][_tri_buffer_index]   = sample[0];
            _tri_buffer[_tri_index][_tri_buffer_index+1] = sample[1];
            _tri_buffer_index += 2;
            _visSum += abs((int)sample[0]) + abs((int)sample[1]);
            _visCount += 2;

            // Cheap bass/treble split (no FFT): a one-pole low-pass filter tracks
            // the low end, and whatever's left over (mono - lowpass) is "treble".
            // Just drives which color a bar renders as, not its height.
            float mono = 0.5f * (sample[0] + sample[1]);
            _bassLP += BASS_ALPHA * (mono - _bassLP);
            float treble = mono - _bassLP;
            _visBassSum += fabsf(_bassLP);
            _visTrebleSum += fabsf(treble);
            return true;
        }
        flush();
        return false;
    }
    void flush() override {
        if (_tri_buffer_index) {
            // Snapshot the tail of this chunk for the full-screen visualizer
            // before playRaw() hands the buffer off -- last VISRAW_FRAMES
            // stereo frames (or fewer, zero-padded, if the chunk was short,
            // e.g. right before a track ends).
            size_t want = (size_t)VISRAW_FRAMES * 2;
            size_t n = _tri_buffer_index < want ? _tri_buffer_index : want;
            if (n < want) memset(visRawBuf, 0, (want - n) * sizeof(int16_t));
            memcpy(visRawBuf + (want - n), _tri_buffer[_tri_index] + (_tri_buffer_index - n), n * sizeof(int16_t));

            _m5sound->playRaw(_tri_buffer[_tri_index], _tri_buffer_index, hertz, true, 1, _virtual_ch);
            _tri_index = _tri_index < 2 ? _tri_index + 1 : 0;
            _tri_buffer_index = 0;
        }
        if (_visCount) {
            // Typical music sits well under full-scale, so raw average |sample|
            // barely moves the bars -- apply gain plus a compressive sqrt curve
            // (boosts quiet/moderate passages more than loud ones) for a much
            // more visibly reactive meter.
            float raw = (float)_visSum / _visCount / 32768.0f;
            float boosted = sqrtf(raw * 2.0f);
            if (boosted > 1.0f) boosted = 1.0f;

            float bassRatio = 0.5f;
            float total = _visBassSum + _visTrebleSum;
            if (total > 0.0001f) bassRatio = _visBassSum / total;

            pushVisLevel(boosted, bassRatio);
            lastBassEnvelope = (_visBassSum / (_visCount / 2)) / 32768.0f;
            _visSum = 0; _visCount = 0;
            _visBassSum = 0.0f; _visTrebleSum = 0.0f;
        }
    }
    bool stop() override { flush(); _m5sound->stop(_virtual_ch); return true; }
protected:
    m5::Speaker_Class* _m5sound; uint8_t _virtual_ch;
    static constexpr size_t tri_buf_size = 2048;
    static constexpr float BASS_ALPHA = 0.04f;   // one-pole LP, roughly a ~280Hz crossover at 44.1kHz
    int16_t _tri_buffer[3][tri_buf_size];
    size_t _tri_buffer_index = 0, _tri_index = 0;
    uint32_t _visSum = 0, _visCount = 0;
    float _bassLP = 0.0f;
    float _visBassSum = 0.0f, _visTrebleSum = 0.0f;
};
static AudioOutputM5Speaker *out = nullptr;

// ---------- FFT (full-screen visualizer only) ----------
// Ported near-verbatim from AdvanceOS-for-Cardputer's MusicPlayerV2 (MIT
// license, https://github.com/bomberman30/AdvanceOS-for-cardputer), which in
// turn traces back to M5Stack's own FFT audio-visualizer example sketches --
// a 256-point, Hann-windowed, radix-2 FFT with a precomputed bit-reversal
// table. Self-contained (just math.h), so it drops in unchanged. The small
// corner visualizer elsewhere in this file intentionally stays on its cheap
// per-chunk amplitude-average approach -- this one exists only for the
// full-screen mode, which wants a real per-frequency-bin spectrum.
#define FFT_SIZE 256
class fft_t {
    float _wr[FFT_SIZE + 1];
    float _wi[FFT_SIZE + 1];
    float _fr[FFT_SIZE + 1];
    float _fi[FFT_SIZE + 1];
    uint16_t _br[FFT_SIZE + 1];
    size_t _ie;

public:
    fft_t(void) {
        _ie = logf((float)FFT_SIZE) / log(2.0) + 0.5;
        static constexpr float omega = 2.0f * (float)M_PI / FFT_SIZE;
        static constexpr int s4 = FFT_SIZE / 4;
        static constexpr int s2 = FFT_SIZE / 2;
        for (int i = 1; i < s4; ++i) {
            float f = cosf(omega * i);
            _wi[s4 + i] = f;
            _wi[s4 - i] = f;
            _wr[i] = f;
            _wr[s2 - i] = -f;
        }
        _wi[s4] = _wr[0] = 1;

        size_t je = 1;
        _br[0] = 0;
        _br[1] = FFT_SIZE / 2;
        for (size_t i = 0; i < _ie - 1; ++i) {
            _br[je << 1] = _br[je] >> 1;
            je = je << 1;
            for (size_t j = 1; j < je; ++j) {
                _br[je + j] = _br[je] + _br[j];
            }
        }
    }

    void exec(const int16_t *in) {
        memset(_fi, 0, sizeof(_fi));
        for (size_t j = 0; j < FFT_SIZE / 2; ++j) {
            float basej = 0.25f * (1.0f - _wr[j]);
            size_t r = FFT_SIZE - j - 1;

            // Hann window + stereo-to-mono downmix.
            _fr[_br[j]] = basej * (in[j * 2] + in[j * 2 + 1]);
            _fr[_br[r]] = basej * (in[r * 2] + in[r * 2 + 1]);
        }

        size_t s = 1;
        size_t i = 0;
        do {
            size_t ke = s;
            s <<= 1;
            size_t je = FFT_SIZE / s;
            size_t j = 0;
            do {
                size_t k = 0;
                do {
                    size_t l = s * j + k;
                    size_t m = ke * (2 * j + 1) + k;
                    size_t p = je * k;
                    float Wxmr = _fr[m] * _wr[p] + _fi[m] * _wi[p];
                    float Wxmi = _fi[m] * _wr[p] - _fr[m] * _wi[p];
                    _fr[m] = _fr[l] - Wxmr;
                    _fi[m] = _fi[l] - Wxmi;
                    _fr[l] += Wxmr;
                    _fi[l] += Wxmi;
                } while (++k < ke);
            } while (++j < je);
        } while (++i < _ie);
    }

    uint32_t get(size_t index) {
        return (index < FFT_SIZE / 2) ? (uint32_t)sqrtf(_fr[index] * _fr[index] + _fi[index] * _fi[index]) : 0u;
    }
};
static fft_t fft;

// ---------- Now Playing: album art ----------
// Art is decoded once per album into coverSprite and only re-decoded when the
// playing folder changes (see playQueuePos); auto-advance within an album keeps
// whatever is already on screen instead of touching the SD card again.
static const int COVER_W = 105, COVER_H = 105;
static const int COVER_X = 6,   COVER_Y = 6;   // top border matches left border

enum class ImgFmt { NONE, JPEG, PNG, BMP, GIF, QOI };

static M5Canvas *coverSprite = nullptr;
static bool coverSpriteOk = false;
static char artCachedFolder[MY_PATH_MAX] = "";
static bool artLoaded = false;

// Separate sprite from coverSprite (rather than reusing one buffer for both)
// so a manual "show turntable instead" toggle (see preferTurntable/KEY_ART_TOGGLE)
// can switch instantly without re-decoding art from SD or losing the decoded
// pixels -- a few extra KB of SRAM is cheap next to that.
static M5Canvas *turntableSprite = nullptr;
static bool turntableSpriteOk = false;

static const size_t ART_SCAN_MAX = 16384;      // generous enough to pass long EXIF/ID3 headers
static uint8_t artScanBuf[ART_SCAN_MAX];

// Scan the first ART_SCAN_MAX bytes of an open file for an embedded image signature.
static ImgFmt findImageStart(File &f, size_t &outOffset) {
    f.seek(0);
    size_t n = f.read(artScanBuf, ART_SCAN_MAX);
    for (size_t i = 0; i < n; i++) {
        if (i + 1 < n && artScanBuf[i] == 0xFF && artScanBuf[i+1] == 0xD8) { outOffset = i; return ImgFmt::JPEG; }
        if (i + 7 < n && artScanBuf[i] == 0x89 && artScanBuf[i+1] == 'P' && artScanBuf[i+2] == 'N' && artScanBuf[i+3] == 'G' &&
            artScanBuf[i+4] == 0x0D && artScanBuf[i+5] == 0x0A && artScanBuf[i+6] == 0x1A && artScanBuf[i+7] == 0x0A) { outOffset = i; return ImgFmt::PNG; }
        if (i + 1 < n && artScanBuf[i] == 'B' && artScanBuf[i+1] == 'M') { outOffset = i; return ImgFmt::BMP; }
        if (i + 5 < n && artScanBuf[i] == 'G' && artScanBuf[i+1] == 'I' && artScanBuf[i+2] == 'F' && artScanBuf[i+3] == '8' &&
            (artScanBuf[i+4] == '7' || artScanBuf[i+4] == '9') && artScanBuf[i+5] == 'a') { outOffset = i; return ImgFmt::GIF; }
        if (i + 3 < n && artScanBuf[i] == 'q' && artScanBuf[i+1] == 'o' && artScanBuf[i+2] == 'i' && artScanBuf[i+3] == 'f') { outOffset = i; return ImgFmt::QOI; }
    }
    return ImgFmt::NONE;
}

// Read width/height from the image header at dataOffset. JPEG walks marker segments
// (tolerating long EXIF/APPn blocks) looking for the SOF0/SOF2 dimensions.
static bool getImageSize(File &f, size_t dataOffset, ImgFmt fmt, uint32_t &w, uint32_t &h) {
    w = h = 0;
    f.seek(dataOffset);
    switch (fmt) {
        case ImgFmt::PNG: {
            uint8_t hdr[24];
            if (f.read(hdr, sizeof(hdr)) < sizeof(hdr)) return false;
            w = ((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) | ((uint32_t)hdr[18] << 8) | hdr[19];
            h = ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) | ((uint32_t)hdr[22] << 8) | hdr[23];
            return true;
        }
        case ImgFmt::BMP: {
            uint8_t hdr[26];
            if (f.read(hdr, sizeof(hdr)) < sizeof(hdr)) return false;
            w = (uint32_t)hdr[18] | ((uint32_t)hdr[19] << 8) | ((uint32_t)hdr[20] << 16) | ((uint32_t)hdr[21] << 24);
            int32_t hs = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) | ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24));
            h = (uint32_t)(hs < 0 ? -hs : hs);
            return true;
        }
        case ImgFmt::GIF: {
            uint8_t hdr[10];
            if (f.read(hdr, sizeof(hdr)) < sizeof(hdr)) return false;
            w = (uint32_t)hdr[6] | ((uint32_t)hdr[7] << 8);
            h = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8);
            return true;
        }
        case ImgFmt::QOI: {
            uint8_t hdr[12];
            if (f.read(hdr, sizeof(hdr)) < sizeof(hdr)) return false;
            w = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
            h = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) | ((uint32_t)hdr[10] << 8) | hdr[11];
            return true;
        }
        case ImgFmt::JPEG: {
            size_t scanned = 0;
            const size_t jpegScanMax = 16384;
            while (scanned < jpegScanMax) {
                int c1 = f.read(); if (c1 < 0) break;
                if ((uint8_t)c1 != 0xFF) { scanned++; continue; }
                int c2 = f.read(); if (c2 < 0) break;
                scanned += 2;
                if (c2 == 0xC0 || c2 == 0xC2) {
                    uint8_t seg[7];
                    if (f.read(seg, sizeof(seg)) < sizeof(seg)) break;
                    h = ((uint32_t)seg[3] << 8) | seg[4];
                    w = ((uint32_t)seg[5] << 8) | seg[6];
                    return (w > 0 && h > 0);
                } else if (c2 != 0xFF && c2 != 0x00 && c2 != 0xD8) {
                    int lh = f.read(), ll = f.read();
                    if (lh < 0 || ll < 0) break;
                    uint16_t seglen = ((uint16_t)(uint8_t)lh << 8) | (uint8_t)ll;
                    if (seglen < 2) break;
                    f.seek(f.position() + seglen - 2);
                    scanned += seglen;
                }
            }
            return false;
        }
        default: return false;
    }
}

// True once loadAlbumArt() found real embedded art for the currently-playing
// album; false means the animated turntable placeholder below is showing
// instead. Read by turntableTick() to decide whether it should be animating.
static bool coverHasArt = false;

// Manual override (KEY_ART_TOGGLE, Now Playing only): show the turntable even
// when real art was found. Only meaningful when coverHasArt is true -- with
// no real art, the turntable is already what's showing either way.
static bool preferTurntable = false;

// Animated "no cover" placeholder: a real 20-frame turntable animation (one
// exact loop, extracted from a source GIF -- see turntable_frames.h) rather
// than hand-drawn shapes. Each frame is stored as a 1-bit mask (on/off per
// pixel), not fixed colors, so it can be recolored per-theme here instead of
// baking in a specific palette -- currently uses theme.file/theme.npBg, the
// same foreground/background pairing used elsewhere on this screen, so it always
// matches whatever theme is active with no extra plumbing.
//
// Redrawn into coverSprite and blitted via the existing drawArtRegion() seam
// -- same off-screen-composite-then-blit-as-one-operation pattern already
// used for real album art and the visualizer, which is why this animates
// without the flash a direct-draw redraw would show (that tradeoff was
// accepted for the title marquee for speed, but there's no reason to accept
// it here too when the sprite path is just as easy). Written directly into
// the sprite's raw buffer (same byte-swapped-565 technique as the album art
// vibrance boost) rather than per-pixel drawPixel() calls, so redrawing all
// 11k pixels every ~160ms stays cheap.
static int turntableFrameIdx = 0;

static void drawTurntableFrame() {
    if (!turntableSpriteOk) return;
    uint16_t* buf = (uint16_t*)turntableSprite->getBuffer();
    uint16_t fg = (theme.file >> 8) | (theme.file << 8);
    uint16_t bg = (theme.npBg >> 8) | (theme.npBg << 8);
    const uint8_t* frame = &turntableFrames[turntableFrameIdx * TURNTABLE_BYTES_PER_FRAME];
    int n = TURNTABLE_W * TURNTABLE_H;   // must match COVER_W * COVER_H
    for (int i = 0; i < n; i++) {
        bool on = (frame[i >> 3] >> (7 - (i & 7))) & 1;
        buf[i] = on ? fg : bg;
    }
    turntableFrameIdx = (turntableFrameIdx + 1) % TURNTABLE_FRAME_COUNT;
}

static void drawArtRegion();   // defined below; blits coverSprite to the screen

// Called every loop() iteration; only animates (and only touches the
// turntable sprite) while the turntable placeholder is actually the one
// showing -- either because there's no real art, or preferTurntable forced it.
static void turntableTick() {
    static unsigned long last = 0;
    if (uiMode != MODE_NOWPLAYING || !turntableSpriteOk) return;
    if (coverHasArt && !preferTurntable) return;
    unsigned long now = millis();
    if (now - last < 160) return;
    last = now;
    drawTurntableFrame();
    drawArtRegion();
}

// One-time saturation/contrast boost, applied in place on the already-allocated
// sprite buffer (no new RAM). Downscaling the source image to COVER_W x COVER_H
// via drawJpg's built-in resize tends to flatten contrast/saturation, so this
// pushes each pixel's channels away from its own luminance (saturation) and
// away from mid-grey (contrast) before it's ever shown. Runs once per album
// (same moment we already accept a brief decode hitch), not per frame.
static const float ART_SATURATION = 1.35f, ART_CONTRAST = 1.08f;

// w/h is the actually-drawn image region, which can be smaller than the
// COVER_W x COVER_H buffer for non-square art (see the scale-to-fit comment
// in loadAlbumArt()) -- stride stays COVER_W either way since that's the
// buffer's real row width. Only that drawn region should ever pass through
// here: running the leftover COL_NP_BG-filled strip through a contrast/
// saturation curve meant for photo content warped it away from the theme's
// actual background color (most visible as an off, sometimes near-black,
// band under short/wide covers) instead of leaving it as the clean flat
// fill loadAlbumArt() just painted.
static void boostArtVibrance(uint16_t* buf, int stride, int w, int h) {
    for (int y = 0; y < h; y++) {
        uint16_t* row = buf + y * stride;
        for (int x = 0; x < w; x++) {
            // M5Canvas's 16bpp format is "swap565" (byte-swapped to match SPI wire
            // order for fast panel blits), not naively-packed RRRRRGGGGGGBBBBB --
            // byte-swap to standard RGB565 before unpacking, and swap back before
            // storing. Skipping this produced the "wrong colored pixels" corruption.
            uint16_t raw = row[x];
            uint16_t px = (raw >> 8) | (raw << 8);

            int r = ((px >> 11) & 0x1F) * 255 / 31;
            int g = ((px >> 5)  & 0x3F) * 255 / 63;
            int b = (px         & 0x1F) * 255 / 31;

            int lum = (r * 30 + g * 59 + b * 11) / 100;
            r = lum + (int)((r - lum) * ART_SATURATION);
            g = lum + (int)((g - lum) * ART_SATURATION);
            b = lum + (int)((b - lum) * ART_SATURATION);

            r = 128 + (int)((r - 128) * ART_CONTRAST);
            g = 128 + (int)((g - 128) * ART_CONTRAST);
            b = 128 + (int)((b - 128) * ART_CONTRAST);

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            uint16_t packed = (uint16_t)(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255));
            row[x] = (packed >> 8) | (packed << 8);
        }
    }
}

// Decode the embedded cover of `path` into coverSprite. Only called when the playing
// album folder changes (see playQueuePos) -- never on every redraw/track-advance.
static void loadAlbumArt(const char* path) {
    if (!coverSpriteOk) return;

    File f = SD.open(path);
    if (!f) { coverHasArt = false; drawTurntableFrame(); return; }

    size_t offset = 0;
    ImgFmt fmt = findImageStart(f, offset);
    if (fmt == ImgFmt::NONE || fmt == ImgFmt::GIF) {   // GIF has no M5GFX draw call available
        f.close();
        coverHasArt = false;
        drawTurntableFrame();
        return;
    }

    uint32_t imgW = 0, imgH = 0;
    bool gotSize = getImageSize(f, offset, fmt, imgW, imgH);

    float scale = 0.0f;   // 0 => fit-to-box (M5GFX auto-scales when size is unknown)
    if (gotSize && imgW > 0 && imgH > 0) {
        float sx = (float)COVER_W / (float)imgW;
        float sy = (float)COVER_H / (float)imgH;
        scale = sx < sy ? sx : sy;
        if (scale > 1.0f) scale = 1.0f;      // never upscale
    }

    f.seek(offset);
    // Non-square art (e.g. a wide/short cover) scales to fit without filling
    // COVER_W x COVER_H -- clear to the Now Playing background first so the
    // leftover strip is a clean bg, not whatever pixels happened to already
    // be in the sprite (a stale previous album's art, or the turntable mask).
    // The draw below is always anchored at (0,0), so the art's top edge (and
    // thus COVER_Y, the margin above it) never moves regardless of its
    // scaled height -- only the bottom/right can fall short.
    coverSprite->fillSprite(COL_NP_BG);
    bool ok = false;
    switch (fmt) {
        case ImgFmt::JPEG: ok = coverSprite->drawJpg(&f, 0, 0, COVER_W, COVER_H, 0, 0, scale, scale); break;
        case ImgFmt::PNG:  ok = coverSprite->drawPng(&f, 0, 0, COVER_W, COVER_H, 0, 0, scale, scale); break;
        case ImgFmt::BMP:  ok = coverSprite->drawBmp(&f, 0, 0, COVER_W, COVER_H, 0, 0, scale, scale); break;
        case ImgFmt::QOI:  ok = coverSprite->drawQoi(&f, 0, 0, COVER_W, COVER_H, 0, 0, scale, scale); break;
        default: break;
    }
    f.close();
    coverHasArt = ok;
    if (ok) {
        // scale==0 means M5GFX auto-fit the image to exactly COVER_W x COVER_H
        // (see above) -- no leftover strip in that case, so boost the whole box.
        int drawnW = COVER_W, drawnH = COVER_H;
        if (scale > 0.0f) {
            drawnW = (int)(imgW * scale + 0.5f);
            drawnH = (int)(imgH * scale + 0.5f);
            if (drawnW < 1) drawnW = 1; else if (drawnW > COVER_W) drawnW = COVER_W;
            if (drawnH < 1) drawnH = 1; else if (drawnH > COVER_H) drawnH = COVER_H;
        }
        boostArtVibrance((uint16_t*)coverSprite->getBuffer(), COVER_W, drawnW, drawnH);
    }
    // Always refresh the turntable sprite too, even when real art was found --
    // otherwise a manual preferTurntable toggle (KEY_ART_TOGGLE) would show a
    // stale frame from whatever album last had no art, or garbage on first boot.
    drawTurntableFrame();
}

// SEAM: this is the only place the Now Playing screen paints the art box. Today it
// blits the cached cover (or placeholder); a future audio visualizer can render live
// here instead, without touching the loading/caching logic above.
static void drawArtRegion() {
    bool showTurntable = !coverHasArt || preferTurntable;
    if (showTurntable && turntableSpriteOk) turntableSprite->pushSprite(COVER_X, COVER_Y);
    else if (!showTurntable && coverSpriteOk) coverSprite->pushSprite(COVER_X, COVER_Y);
    else M5Cardputer.Display.fillRect(COVER_X, COVER_Y, COVER_W, COVER_H, COL_DIM);
}

// ---------- Name helpers + case-insensitive sort ----------
static bool hasExt(const char* s, const char* ext) {
    int n = (int)strlen(s), el = (int)strlen(ext);
    if (n < el) return false;
    const char* e = s + n - el;
    for (int i = 0; i < el; i++) {
        char a = e[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}
// Format is decided purely by extension -- no content sniffing. FMT_MP3 is
// the fallback for openSelected()/playQueuePos() callers that already know a
// name passed isAudioFile(), so it's never actually reached for a non-mp3
// name in practice, but keeps formatForName() total (always returns a value).
static AudioFormat formatForName(const char* nm) {
    if (hasExt(nm, ".flac")) return FMT_FLAC;
    if (hasExt(nm, ".wav") || hasExt(nm, ".wave")) return FMT_WAV;
    return FMT_MP3;
}
static bool isAudioFile(const char* nm) {
    return hasExt(nm, ".mp3") || hasExt(nm, ".flac") || hasExt(nm, ".wav") || hasExt(nm, ".wave");
}
static const char* baseName(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}
// Case-insensitive compare; non-A-Z (accented/CJK) sort after A-Z since their
// byte values are >= 0x80, which compares greater than ASCII letters.
static int nameCmp(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = *a, cb = *b;
        // if both point at a digit, compare the full numeric runs
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            // skip leading zeros
            while (*a == '0') a++;
            while (*b == '0') b++;
            // measure run lengths
            const char *ea = a, *eb = b;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            int la = ea - a, lb = eb - b;
            if (la != lb) return la - lb;          // longer number = larger
            while (a < ea && b < eb) {             // same length: compare digits
                if (*a != *b) return (int)*a - (int)*b;
                a++; b++;
            }
            continue;                              // numeric run equal, keep going
        }
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static const char* entryName(int i) { return &namePool[nameOffset[i]]; }        // raw entry
static const char* nameAt(int i)    { return &namePool[nameOffset[sortIdx[i]]]; } // sorted view
static bool isDirAt(int i)          { return entryIsDir[sortIdx[i]]; }

// insertion sort on sortIdx (folders first, then files, each alphabetized)
static void sortEntries() {
    for (int i = 0; i < entryCount; i++) sortIdx[i] = i;
    for (int i = 1; i < entryCount; i++) {
        int key = sortIdx[i], j = i - 1;
        while (j >= 0) {
            int a = sortIdx[j];
            // folders before files
            bool swap;
            if (entryIsDir[a] != entryIsDir[key]) swap = (!entryIsDir[a] && entryIsDir[key]);
            else swap = (nameCmp(entryName(a), entryName(key)) > 0);
            if (!swap) break;
            sortIdx[j + 1] = sortIdx[j];
            j--;
        }
        sortIdx[j + 1] = key;
    }
}

// ---------- Directory load ----------
static bool loadDir() {
    entryCount = 0; poolUsed = 0;
    File dir = SD.open(currentPath);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }
    File e = dir.openNextFile();
    while (e && entryCount < MAX_ENTRIES) {
        const char* nm = baseName(e.name());
        int len = strlen(nm);
        if (nm[0] != '.' && (poolUsed + len + 1) < NAME_POOL_SIZE) {
            bool isDir = e.isDirectory();
            if (isDir || isAudioFile(nm)) {
                nameOffset[entryCount] = poolUsed;
                entryIsDir[entryCount] = isDir;
                memcpy(&namePool[poolUsed], nm, len + 1);
                poolUsed += len + 1;
                entryCount++;
            }
        }
        e.close();
        e = dir.openNextFile();
    }
    if (e) e.close();
    dir.close();
    sortEntries();
    cursor = 0; scroll = 0;
    return true;
}

// ---------- Play queue: snapshot a folder's tracks (alphabetized) ----------
// Reads the given folder fresh so it's independent of the browse view.
static void buildQueue(const char* folder) {
    queueCount = 0; queuePoolUsed = 0;
    strncpy(queueFolder, folder, MY_PATH_MAX - 1);
    queueFolder[MY_PATH_MAX - 1] = '\0';

    // gather into temp offset list, then sort
    static uint16_t tmpOff[QUEUE_MAX];
    File dir = SD.open(folder);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    File e = dir.openNextFile();
    while (e && queueCount < QUEUE_MAX) {
        const char* nm = baseName(e.name()); 
        int len = strlen(nm);
        if (nm[0] != '.' && !e.isDirectory() && isAudioFile(nm) &&
            (queuePoolUsed + len + 1) < QNAME_POOL) {
            tmpOff[queueCount] = queuePoolUsed;
            queueOffset[queueCount] = queuePoolUsed;
            memcpy(&queuePool[queuePoolUsed], nm, len + 1);
            queuePoolUsed += len + 1;
            queueCount++;
        }
        e.close();
        e = dir.openNextFile();
    }
    if (e) e.close();
    dir.close();

    // insertion sort queueOffset by name
    for (int i = 1; i < queueCount; i++) {
        uint16_t key = queueOffset[i]; int j = i - 1;
        while (j >= 0 && nameCmp(&queuePool[queueOffset[j]], &queuePool[key]) > 0) {
            queueOffset[j + 1] = queueOffset[j]; j--;
        }
        queueOffset[j + 1] = key;
    }
}

static const char* queueName(int i) { return &queuePool[queueOffset[i]]; }

// ---------- Album-end "next album" lookup ----------
// Independent of both the browser's view state (namePool/entryCount/etc.) and
// the play queue -- reads the parent folder fresh into its own small buffers
// so it never disturbs whatever the user is currently browsing.
static const int ALBUMLIST_MAX = 128, ALBUMLIST_POOL = 4096;
static char     albumListPool[ALBUMLIST_POOL];
static uint16_t albumListOffset[ALBUMLIST_MAX];

// Finds the folder alphabetically after `currentFolder` among its sibling
// folders in the parent directory, wrapping to the first if `currentFolder`
// was last. Returns false if there's no sibling to advance to.
static bool findNextAlbumFolder(const char* currentFolder, char* outPath, size_t outSize) {
    char parent[MY_PATH_MAX];
    strncpy(parent, currentFolder, MY_PATH_MAX - 1);
    parent[MY_PATH_MAX - 1] = '\0';
    const char* curName = baseName(currentFolder);
    char* s = strrchr(parent, '/');
    if (s == parent) parent[1] = '\0';
    else if (s) *s = '\0';
    else return false;

    int count = 0, poolUsed = 0;
    File dir = SD.open(parent);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }
    File e = dir.openNextFile();
    while (e && count < ALBUMLIST_MAX) {
        if (e.isDirectory()) {
            const char* nm = baseName(e.name());
            int len = strlen(nm);
            if (nm[0] != '.' && (poolUsed + len + 1) < ALBUMLIST_POOL) {
                albumListOffset[count] = poolUsed;
                memcpy(&albumListPool[poolUsed], nm, len + 1);
                poolUsed += len + 1;
                count++;
            }
        }
        e.close();
        e = dir.openNextFile();
    }
    if (e) e.close();
    dir.close();
    if (count == 0) return false;

    for (int i = 1; i < count; i++) {
        uint16_t key = albumListOffset[i]; int j = i - 1;
        while (j >= 0 && nameCmp(&albumListPool[albumListOffset[j]], &albumListPool[key]) > 0) {
            albumListOffset[j + 1] = albumListOffset[j]; j--;
        }
        albumListOffset[j + 1] = key;
    }

    int curIdx = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(&albumListPool[albumListOffset[i]], curName) == 0) { curIdx = i; break; }
    }
    if (curIdx < 0) return false;
    int nextIdx = (curIdx + 1) % count;
    if (nextIdx == curIdx) return false;   // only one sibling folder -- nothing to advance to

    const char* nextName = &albumListPool[albumListOffset[nextIdx]];
    if (strcmp(parent, "/") == 0) snprintf(outPath, outSize, "/%s", nextName);
    else snprintf(outPath, outSize, "%s/%s", parent, nextName);
    return true;
}

// ---------- ID3 metadata (artist/title/album for the Now Playing screen) ----------
// Copies the raw ID3 frame value into dst. Non-unicode frames (ISO-8859-1, or per
// ID3v2.4, UTF-8) pass straight through since our display font already expects UTF-8.
// Unicode (UTF-16) frames get a best-effort conversion; the callback API gives no
// explicit length, so a 0x00 code unit (e.g. Latin range in UTF-16LE) truncates the
// string early -- CJK text, whose code units are rarely zero, comes through fine.
static void copyMeta(char* dst, size_t dstSize, bool isUnicode, const char* str) {
    if (!isUnicode) {
        strncpy(dst, str, dstSize - 1);
        dst[dstSize - 1] = '\0';
        return;
    }
    const uint8_t* b = (const uint8_t*)str;
    bool bigEndian = false;
    size_t i = 0;
    if (b[0] == 0xFE && b[1] == 0xFF) { bigEndian = true; i = 2; }
    else if (b[0] == 0xFF && b[1] == 0xFE) { bigEndian = false; i = 2; }
    size_t o = 0;
    for (; o + 4 < dstSize && i + 1 < 63; i += 2) {   // 63: bounds to the ID3 parser's value[64]
        uint16_t cu = bigEndian ? ((b[i] << 8) | b[i+1]) : (b[i] | (b[i+1] << 8));
        if (cu == 0) break;
        if (cu < 0x80) { dst[o++] = (char)cu; }
        else if (cu < 0x800) { dst[o++] = (char)(0xC0 | (cu >> 6)); dst[o++] = (char)(0x80 | (cu & 0x3F)); }
        else { dst[o++] = (char)(0xE0 | (cu >> 12)); dst[o++] = (char)(0x80 | ((cu >> 6) & 0x3F)); dst[o++] = (char)(0x80 | (cu & 0x3F)); }
    }
    dst[o] = '\0';
}

static void mp3MetadataCB(void* cbData, const char* type, bool isUnicode, const char* str) {
    (void)cbData;
    if      (!strcmp(type, "Title"))     copyMeta(curTitle,  sizeof(curTitle),  isUnicode, str);
    else if (!strcmp(type, "Performer")) copyMeta(curArtist, sizeof(curArtist), isUnicode, str);
    else if (!strcmp(type, "Album"))     copyMeta(curAlbum,  sizeof(curAlbum),  isUnicode, str);
}

// ---------- Playback control ----------
static void stopPlayback() {
    if (decoder) { if (decoder->isRunning()) decoder->stop(); delete decoder; decoder = nullptr; }
    if (id3)  { delete id3;  id3  = nullptr; }
    if (file) { delete file; file = nullptr; }
    M5Cardputer.Speaker.stop();
}

// Play queue[pos] from the current queueFolder.
static void playQueuePos(int pos) {
    if (queueCount == 0) return;
    if (pos < 0) pos = queueCount - 1;
    if (pos >= queueCount) pos = 0;             // loop album
    queuePos = pos;

    char full[MY_PATH_MAX];
    if (strcmp(queueFolder, "/") == 0) snprintf(full, MY_PATH_MAX, "/%s", queueName(pos));
    else snprintf(full, MY_PATH_MAX, "%s/%s", queueFolder, queueName(pos));

    stopPlayback();
    curArtist[0] = curTitle[0] = curAlbum[0] = '\0';
    curFormat = formatForName(queueName(pos));
    file = new AudioFileSourceSD(full);
    id3  = new AudioFileSourceID3(file);
    id3->RegisterMetadataCB(mp3MetadataCB, nullptr);
    switch (curFormat) {
        case FMT_FLAC: decoder = new AudioGeneratorFLAC(); break;
        case FMT_WAV:  decoder = new AudioGeneratorWAV();  break;
        default:       decoder = new AudioGeneratorMP3();  break;
    }
    if (decoder->begin(id3, out)) {
        playState = PLAYING;
        strncpy(nowPlaying, queueName(pos), sizeof(nowPlaying) - 1);
        nowPlaying[sizeof(nowPlaying) - 1] = '\0';

        // decoder->begin() doesn't touch the source at all -- the ID3 tag (and
        // thus the metadata callback, when the file actually has one) is only
        // parsed on the *first* loop() call. Prime it here so curArtist/
        // curTitle/curAlbum are ready before we draw below; otherwise
        // auto-advance (which redraws immediately) would show stale/empty
        // metadata for one frame. FLAC/WAV files typically have no ID3 tag at
        // all, in which case this is a no-op and the filename fallback (see
        // nowPlaying above) is what actually displays.
        decoder->loop();

        // Reload art only when the playing folder (album) has changed.
        if (!artLoaded || strcmp(queueFolder, artCachedFolder) != 0) {
            loadAlbumArt(full);
            strncpy(artCachedFolder, queueFolder, MY_PATH_MAX - 1);
            artCachedFolder[MY_PATH_MAX - 1] = '\0';
            artLoaded = true;
        }
    } else {
        stopPlayback(); playState = STOPPED; nowPlaying[0] = '\0';
    }
    needsRedraw = true;
}

static void nextTrack() { playQueuePos(queuePos + 1); }
static void prevTrack() { playQueuePos(queuePos - 1); }

static void drawProgressBar();   // defined below, in the Now Playing section

// ---------- Seeking (Now Playing screen: left/right "arrow" keys) ----------
// The keyboard has no dedicated arrow keys -- this project already treats the
// punctuation cluster as one (KEY_BACK=',' sits in the "left" position,
// KEY_OPEN='/' in "right"), and that's what these bind to while on the Now
// Playing screen specifically; their browser meanings (back/open) are
// unaffected everywhere else -- see the keyboard handling in loop().
//
// There's no bitrate/duration parsing (see the progress bar's own comment), so
// seeking works the same way progress is displayed: as a fraction of the
// file's total byte size, not an exact time offset.
//
// Mechanically: AudioGeneratorMP3 has no seek API of its own, so this seeks
// the underlying AudioFileSourceSD directly (bypassing the decoder) and then
// calls the decoder's public desync(), which discards its buffered/stream-sync
// state. The next loop() call re-reads from the new file position and libmad
// resyncs to the next valid frame header on its own -- the same "lost sync"
// recovery path it already relies on at normal playback start.
//
// FLAC and WAV have no equivalent recovery path (FLAC needs frame-aligned
// sync points it doesn't expose; WAV's internal read buffer/byte-counter
// would just desync permanently with no way to resync from outside the
// class), so seeking is MP3-only for now -- seekBy() below no-ops for the
// other formats rather than corrupting playback.
static const float SEEK_STEP_PCT = 0.02f;         // ~2% of the file per tap/repeat
static const uint32_t SEEK_HOLD_DELAY_MS = 350;   // hold this long before repeat-seeking kicks in
static const uint32_t SEEK_HOLD_REPEAT_MS = 150;  // then repeat this often while still held
static const uint32_t DOUBLE_TAP_MS = 400;        // max gap between taps to count as a double-tap
static unsigned long lastLeftTapTime = 0, lastRightTapTime = 0;
// Hold-repeat state for the seek keys; 0 means "not currently in a hold". Reset
// whenever the key is released or we leave Now Playing, so releasing and
// re-pressing always starts a fresh tap rather than an instant repeat.
static unsigned long rightHoldNext = 0, leftHoldNext = 0;

static void seekBy(int32_t deltaBytes) {
    if (curFormat != FMT_MP3) return;   // see the note above -- no safe resync for FLAC/WAV
    if (!file || !decoder || playState == STOPPED) return;
    uint32_t size = file->getSize();
    if (size == 0) return;
    int64_t target = (int64_t)file->getPos() + deltaBytes;
    if (target < 0) target = 0;
    if (target > (int64_t)size) target = (int64_t)size;
    file->seek((int32_t)target, SEEK_SET);
    static_cast<AudioGeneratorMP3*>(decoder)->desync();
    drawProgressBar();   // instant feedback rather than waiting for the periodic tick
}

static void seekByPercent(float pct) {
    if (curFormat != FMT_MP3 || !file) return;
    seekBy((int32_t)(pct * file->getSize()));
}

// Double-tapping left restarts the track instead of seeking backward. Reuses
// playQueuePos() (rather than seeking straight to byte 0) since byte 0 is the
// ID3 header, not the start of audio -- replaying the track properly skips it
// again instead of making the decoder resync through a wall of tag/art bytes.
static void restartTrack() {
    if (queueCount == 0) return;
    playQueuePos(queuePos);
}

// Used when the ALBUM_NEXT end-of-album setting is active: advance to the next
// sibling album folder and start playing its first track. Falls back to
// stopping if there's no sibling to advance to (e.g. a lone album folder).
static void playNextAlbum() {
    char nextFolder[MY_PATH_MAX];
    if (findNextAlbumFolder(queueFolder, nextFolder, sizeof(nextFolder))) {
        buildQueue(nextFolder);
        playQueuePos(0);
    } else {
        stopPlayback();
        playState = STOPPED;
        nowPlaying[0] = '\0';
        needsRedraw = true;
    }
}

static void togglePause() {
    if (playState == PLAYING) playState = PAUSED;
    else if (playState == PAUSED) playState = PLAYING;
}

// ---------- Browser navigation ----------
static void enterFolder(int viewIdx) {
    if (depth >= MAX_DEPTH - 1) return;
    cursorStack[depth] = cursor; scrollStack[depth] = scroll;
    const char* nm = nameAt(viewIdx);
    if (strcmp(currentPath, "/") == 0) snprintf(currentPath, MY_PATH_MAX, "/%s", nm);
    else { int L = strlen(currentPath); snprintf(currentPath + L, MY_PATH_MAX - L, "/%s", nm); }
    depth++;
    loadDir();
    needsRedraw = true;
}
static void goBack() {
    if (depth == 0) {
        // Already at the root folder -- nowhere further to back out to, so
        // treat it as "leave the browser" and drop to Now Playing instead --
        // but only if there's actually something loaded to show there.
        if (playState != STOPPED) {
            uiMode = MODE_NOWPLAYING;
            needsRedraw = true;
        }
        return;
    }
    char* s = strrchr(currentPath, '/');
    if (s == currentPath) currentPath[1] = '\0';
    else if (s) *s = '\0';
    depth--;
    loadDir();
    cursor = cursorStack[depth]; scroll = scrollStack[depth];
    if (cursor >= entryCount) cursor = entryCount ? entryCount - 1 : 0;
    needsRedraw = true;
}

// Start playing the selected track: build the queue from THIS folder,
// find the selected track's position in the sorted queue, play from there.
static void openSelected() {
    if (entryCount == 0) return;
    if (isDirAt(cursor)) { enterFolder(cursor); return; }

    buildQueue(currentPath);
    const char* sel = nameAt(cursor);
    int startPos = 0;
    for (int i = 0; i < queueCount; i++) {
        if (strcmp(queueName(i), sel) == 0) { startPos = i; break; }
    }
    playQueuePos(startPos);
    if (playState == PLAYING) uiMode = MODE_NOWPLAYING;   // jump straight to Now Playing, no delay
}

static void drawBrowserRow(int idx);   // defined below, in the drawing section

// Cursor moves are by far the most frequent redraw trigger while browsing, and
// a full drawBrowser() repaint (header + battery/volume icons + every visible
// row) on every single keypress was visibly flashing and stealing enough loop()
// time to cause an audible audio hiccup. If the cursor moved without scrolling
// the viewport, only the two rows that actually changed need to be touched.
static void moveCursor(int delta) {
    if (entryCount == 0) return;
    int oldCursor = cursor;
    int oldScroll = scroll;
    cursor += delta;
    if (cursor < 0) cursor = entryCount - 1;         // wrap: up past the top -> last entry
    if (cursor >= entryCount) cursor = 0;             // wrap: down past the bottom -> first entry
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + visibleRows) scroll = cursor - visibleRows + 1;

    if (scroll != oldScroll) {
        needsRedraw = true;             // viewport shifted -- every row's content changed
    } else if (cursor != oldCursor) {
        drawBrowserRow(oldCursor);
        drawBrowserRow(cursor);
    }
}
static void drawVolumeMeter();   // defined below, in the drawing section

static void changeVolume(int d) {
    volume += d;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    M5Cardputer.Speaker.setVolume(volume);
    drawVolumeMeter();   // instant feedback -- Now Playing has no other volume readout
}

// ---------- Drawing ----------
// Trim text by pixel width, appending "~" if anything was cut.
// const-ref rather than by-value: this runs on every visible row on every
// cursor move, and the common case (text already fits) needs no mutation at
// all, so there's no reason to pay for a copy of the input just to hand it
// back unchanged.
static String trimToWidth(LovyanGFX &d, const String &text, int maxW) {
    if (d.textWidth(text.c_str()) <= maxW) return text;
    int ellW = d.textWidth("~");
    String out = text;
    while (out.length() > 1 && d.textWidth(out.c_str()) > (maxW - ellW))
        out.remove(out.length() - 1);
    out += "~";
    return out;
}

// Now Playing text column layout, needed by the volume meter below (it starts
// directly above the artist text) as well as drawNowPlaying() itself. Fixed
// independently of COVER_Y/COVER_H so repositioning the art box doesn't move it.
static const int NP_TEXT_X = 120;
static const int NP_TEXT_Y = 22;
static const int NP_LINE_H = 18;

// ---------- Battery + volume meters (top-right, both screens) ----------
static const int BATT_W = 20, BATT_H = 10, BATT_NUB_W = 2, BATT_NUB_H = 6, BATT_MARGIN = 4;

static int battIconX() { return M5Cardputer.Display.width() - BATT_MARGIN - BATT_NUB_W - BATT_W; }

// The actual backdrop behind the top strip -- MODE_SETTINGS is an overlay, not
// its own backdrop, so its color depends on whichever mode it's on top of.
static uint16_t topStripBg() {
    UiMode effective = (uiMode == MODE_SETTINGS) ? uiModeBeforeSettings : uiMode;
    return (effective == MODE_BROWSER) ? COL_HEADER : COL_NP_BG;
}

// Redraws itself against whichever background the active screen already painted
// (header bar in the browser, plain background on Now Playing), so it must be
// called after that background is drawn, not before. Icon only -- no percent
// text, so it doesn't distract from the track info below it.
static void drawBatteryMeter() {
    auto &d = M5Cardputer.Display;
    uint16_t bg = topStripBg();

    const int iconX = battIconX();
    const int iconY = 5;

    d.fillRect(iconX - 1, 0, d.width() - (iconX - 1), ICON_STRIP_H, bg);   // clear stale meter

    int lvl = M5Cardputer.Power.getBatteryLevel();   // 0..100, -1 if unknown
    bool charging = (M5Cardputer.Power.isCharging() == m5::Power_Class::is_charging);

    d.drawRect(iconX, iconY, BATT_W, BATT_H, TFT_WHITE);
    d.fillRect(iconX + BATT_W, iconY + (BATT_H - BATT_NUB_H) / 2, BATT_NUB_W, BATT_NUB_H, TFT_WHITE);

    if (lvl >= 0) {
        int fillMaxW = BATT_W - 2;
        int fillW = (fillMaxW * lvl) / 100;
        uint16_t fillColor = charging ? TFT_CYAN : (lvl <= 20 ? TFT_RED : lvl <= 50 ? TFT_YELLOW : TFT_GREEN);
        if (fillW > 0) d.fillRect(iconX + 1, iconY + 1, fillW, BATT_H - 2, fillColor);
    }
}

// TV-OSD-style volume meter: a small speaker glyph (box + cone + two sound-wave
// arcs) followed by a row of segments spanning from a letter's width past the
// artist text to just short of the battery icon. At rest each unfilled segment
// is a small dim dot (reading as a dotted line); segments up to the current
// volume light up as solid bars.
static const int VOL_ICON_Y = 5;                          // same band as the battery icon
static const int VOL_BOX_W = 3, VOL_BOX_H = 6;             // speaker "coil"
static const int VOL_CONE_W = 5, VOL_CONE_HALF_H = 5;      // speaker cone
static const int VOL_WAVE_GAP = 2;                         // cone tip -> wave arcs' center
static const int VOL_WAVE_R1 = 3, VOL_WAVE_R2 = 6;         // inner/outer wave radii
static const int VOL_WAVE_ANGLE = 50;                      // arcs span +-this many degrees
static const int VOL_SEG_W = 4, VOL_SEG_GAP = 2, VOL_SEG_H = 10, VOL_DOT_SIZE = 2;
static const int VOL_GAP_BEFORE_BATT = 6;

static void drawVolumeMeter() {
    auto &d = M5Cardputer.Display;
    uint16_t bg = topStripBg();

    int apexX = NP_TEXT_X + VOL_BOX_W, apexY = VOL_ICON_Y + VOL_SEG_H / 2;
    int waveCx = apexX + VOL_CONE_W + VOL_WAVE_GAP;
    int iconRightEdge = waveCx + VOL_WAVE_R2;
    int letterGap = d.textWidth("A");   // "a letter's worth" of breathing room
    int segStartX = iconRightEdge + letterGap;
    int segEndX = battIconX() - VOL_GAP_BEFORE_BATT;
    int pitch = VOL_SEG_W + VOL_SEG_GAP;
    int segCount = (segEndX > segStartX) ? (segEndX - segStartX) / pitch : 0;

    d.fillRect(NP_TEXT_X, 0, segEndX - NP_TEXT_X, ICON_STRIP_H, bg);   // clear stale meter

    // speaker glyph: small box + a cone widening away from it + two sound-wave arcs
    d.fillRect(NP_TEXT_X, VOL_ICON_Y + (VOL_SEG_H - VOL_BOX_H) / 2, VOL_BOX_W, VOL_BOX_H, COL_VOLUME);
    d.fillTriangle(apexX, apexY, apexX + VOL_CONE_W, apexY - VOL_CONE_HALF_H,
                                  apexX + VOL_CONE_W, apexY + VOL_CONE_HALF_H, COL_VOLUME);
    d.drawArc(waveCx, apexY, VOL_WAVE_R1, VOL_WAVE_R1 + 1, -VOL_WAVE_ANGLE, VOL_WAVE_ANGLE, COL_VOLUME);
    d.drawArc(waveCx, apexY, VOL_WAVE_R2, VOL_WAVE_R2 + 1, -VOL_WAVE_ANGLE, VOL_WAVE_ANGLE, COL_VOLUME);

    // segment row: solid bars up to current volume, dim resting dots beyond
    int pct = (volume * 100) / 255;
    int lit = (segCount * pct) / 100;
    for (int i = 0; i < segCount; i++) {
        int sx = segStartX + i * pitch;
        if (i < lit) {
            d.fillRect(sx, VOL_ICON_Y, VOL_SEG_W, VOL_SEG_H, COL_VOLUME);
        } else {
            d.fillRect(sx + (VOL_SEG_W - VOL_DOT_SIZE) / 2, VOL_ICON_Y + (VOL_SEG_H - VOL_DOT_SIZE) / 2,
                       VOL_DOT_SIZE, VOL_DOT_SIZE, COL_DIM);
        }
    }
}

static void drawStatusIcons() {
    drawBatteryMeter();
    // Volume doesn't need to be visible while just browsing/sorting files --
    // still fully adjustable via the volume keys either way, just not shown here.
    if (uiMode != MODE_BROWSER) drawVolumeMeter();
}

// Small hand-drawn folder icon (rect body + a little top tab), replacing the
// old "> " text prefix -- folders are already color-coded, so the icon is
// purely a visual affordance now rather than the only way to tell them apart.
static const int FOLDER_ICON_W = 12, FOLDER_ICON_H = 9;
static void drawFolderIcon(LovyanGFX &d, int x, int y, uint16_t color) {
    d.fillRect(x, y, 6, 2, color);
    d.fillRect(x, y + 2, FOLDER_ICON_W, FOLDER_ICON_H - 2, color);
}

// FONT_BROWSER (FreeSans) is a GFXfont with only the ASCII range -- no CJK or
// Cyrillic glyphs at all -- so anything containing a non-ASCII byte falls back
// to FONT_UI (Gothic, which has them) instead of rendering as tofu boxes.
static bool isAsciiOnly(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; *p; p++)
        if (*p >= 0x80) return false;
    return true;
}

// Common "smart"/typographic punctuation -- curly quotes, en/em dashes, the
// ellipsis character -- collapsed to its plain-ASCII equivalent. Most tag
// editors and streaming-service metadata use these instead of straight quotes
// even for ordinary English titles (e.g. "Don't Stop"), and FreeSans has none
// of them either, so without this an awful lot of everyday titles would fall
// back to Gothic over a single punctuation mark. Anything not in this list
// (real CJK/Cyrillic/accented-Latin text) passes through untouched, so it
// still correctly triggers the Gothic fallback in isAsciiOnly() afterward.
static String sanitizePunctuation(const char* s) {
    String out;
    int len = strlen(s);
    out.reserve(len);
    int i = 0;
    while (i < len) {
        unsigned char c0 = (unsigned char)s[i];
        if (c0 < 0x80) { out += (char)c0; i += 1; continue; }

        uint32_t cp = c0;
        int clen = 1;
        if ((c0 & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((c0 & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu);
            clen = 2;
        } else if ((c0 & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((c0 & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) | ((unsigned char)s[i + 2] & 0x3Fu);
            clen = 3;
        } else if ((c0 & 0xF8) == 0xF0 && i + 3 < len) {
            cp = ((c0 & 0x07u) << 18) | (((unsigned char)s[i + 1] & 0x3Fu) << 12)
               | (((unsigned char)s[i + 2] & 0x3Fu) << 6) | ((unsigned char)s[i + 3] & 0x3Fu);
            clen = 4;
        }

        switch (cp) {
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += '\''; break;   // ' ' ‚ ′
            case 0x201C: case 0x201D: case 0x201E: case 0x2033: out += '"';  break;   // " " „ ″
            case 0x2013: case 0x2014: out += '-'; break;                              // – —
            case 0x2026: out += "..."; break;                                          // …
            default:
                for (int k = 0; k < clen; k++) out += s[i + k];
                break;
        }
        i += clen;
    }
    return out;
}

// Picks FONT_BROWSER (after the punctuation substitution above) if what's left
// is plain ASCII, otherwise FONT_UI on the original text -- and returns
// whichever string should actually be printed, so the rendered text always
// matches the font that was set.
static String selectBrowserFont(LovyanGFX &d, const char* raw) {
    // Fast path: most filenames are already plain ASCII, so skip
    // sanitizePunctuation()'s char-by-char String rebuild entirely -- it can
    // only ever change something for non-ASCII input (that's the only case
    // its switch touches), and this runs on every visible row on every
    // cursor move, so the difference between a single strlen+memcpy here and
    // N one-character String::operator+= calls is worth avoiding.
    if (isAsciiOnly(raw)) {
        d.setFont(FONT_BROWSER);
        return String(raw);
    }
    String sanitized = sanitizePunctuation(raw);
    if (isAsciiOnly(sanitized.c_str())) {
        d.setFont(FONT_BROWSER);
        return sanitized;
    }
    d.setFont(FONT_UI);
    return String(raw);
}

// Redraws a single row by absolute entry index (no-op if it's currently
// scrolled out of view). Used both by the full drawBrowser() repaint and by
// moveCursor()'s cheap partial repaint (see there for why that matters).
static void drawBrowserRow(int idx) {
    if (idx < scroll || idx >= scroll + visibleRows || idx >= entryCount) return;
    auto &d = M5Cardputer.Display;
    int y = HEADER_H + (idx - scroll) * ROW_H;
    bool sel = (idx == cursor);
    uint16_t bg = sel ? COL_SEL_BG : COL_BG;
    uint16_t fg = sel ? COL_SEL_FG : (isDirAt(idx) ? COL_FOLDER : COL_FILE);

    if (sel) d.fillRoundRect(2, y + 1, d.width() - 4, ROW_H - 2, 4, bg);
    else     d.fillRect(0, y, d.width(), ROW_H, bg);
    d.setTextColor(fg, bg);

    int textX = 4;
    if (isDirAt(idx)) {
        drawFolderIcon(d, 6, y + (ROW_H - FOLDER_ICON_H) / 2, fg);
        textX = 6 + FOLDER_ICON_W + 5;
    }
    String text = selectBrowserFont(d, nameAt(idx));
    int availW = d.width() - textX - 4;
    d.setCursor(textX, y + (ROW_H - d.fontHeight()) / 2);
    d.print(trimToWidth(d, text, availW));
}

// "/Artist/Album" -> "Artist  /  Album"; root gets a plain label instead of a
// bare slash. Kept ASCII-only (no chevron/arrow glyph) since glyph coverage
// for anything fancier isn't guaranteed across every font a theme might pick.
static String breadcrumb() {
    if (depth == 0) return "Library";
    String s(currentPath);
    if (s.startsWith("/")) s.remove(0, 1);
    s.replace("/", "  /  ");
    return s;
}

static void drawBrowser() {
    auto &d = M5Cardputer.Display;
    d.fillRect(0, 0, d.width(), d.height(), COL_BG);
    d.fillRect(0, 0, d.width(), HEADER_H, COL_HEADER);
    String crumb = selectBrowserFont(d, breadcrumb().c_str());
    d.setTextColor(theme.headerText, COL_HEADER);
    d.setCursor(4, (HEADER_H - d.fontHeight()) / 2);
    d.print(trimToWidth(d, crumb, d.width() - 90));   // 90 ~= reserved for battery+volume icons
    drawStatusIcons();

    if (entryCount == 0) {
        d.setFont(FONT_BROWSER);
        d.setTextColor(COL_DIM, COL_BG);
        d.setCursor(4, HEADER_H + 6); d.print("(empty)");
        return;
    }
    for (int row = 0; row < visibleRows; row++) {
        int idx = scroll + row;
        if (idx >= entryCount) break;
        drawBrowserRow(idx);
    }
}

// ---------- Settings screen ----------
static void drawNowPlaying();   // defined below, in the Now Playing section

// Just the box itself, no backdrop -- used for in-place cursor-move/value-cycle
// updates so they don't have to pay for a full browser/Now-Playing repaint
// (that full repaint, including an album-art sprite blit, was previously
// happening on every single settings keystroke and was audible as a hiccup).
static void drawSettingsBox() {
    auto &d = M5Cardputer.Display;
    // FreeSans (FONT_BROWSER) instead of the Gothic fallback font -- every
    // label/value on this screen is plain ASCII (English words, digits, a
    // plain apostrophe in theme names like "90's Sweater"), so there's never
    // a need to fall back, unlike the browser's file/folder names.
    d.setFont(FONT_BROWSER);

    const int titleH = 24;   // fits FreeSans's ~22px line height with a little breathing room
    const int boxW = d.width() - 40;
    const int boxH = titleH + SETTINGS_VISIBLE * ROW_H + 8;
    const int boxX = (d.width() - boxW) / 2;
    const int boxY = (d.height() - boxH) / 2;

    d.fillRect(boxX, boxY, boxW, boxH, COL_BG);
    d.drawRect(boxX, boxY, boxW, boxH, TFT_WHITE);
    d.setTextColor(COL_FOLDER, COL_BG);
    d.setCursor(boxX + 6, boxY + (titleH - d.fontHeight()) / 2);
    d.print("Settings");

    const char* names[SETTINGS_COUNT]  = { "Backlight", "Screen off", "Album end", "Theme", "Screenshots" };
    String values[SETTINGS_COUNT] = {
        backlightLabels[settingBacklightIdx],
        screenOffLabels[settingScreenOffIdx],
        albumEndLabels[albumEndMode],
        themeLabelAt(settingThemeIdx),
        onOffLabels[settingScreenshotsEnabled ? 1 : 0],
    };
    for (int row = 0; row < SETTINGS_VISIBLE; row++) {
        int i = settingsScroll + row;
        if (i >= SETTINGS_COUNT) break;
        int y = boxY + titleH + row * ROW_H;
        bool sel = (i == settingsCursor);
        uint16_t bg = sel ? COL_SEL_BG : COL_BG;
        uint16_t fg = sel ? COL_SEL_FG : COL_FOLDER;
        d.fillRect(boxX + 2, y, boxW - 4, ROW_H, bg);
        d.setTextColor(fg, bg);
        int textY = y + (ROW_H - d.fontHeight()) / 2;
        d.setCursor(boxX + 6, textY);
        d.print(names[i]);
        int vw = d.textWidth(values[i].c_str());
        d.setCursor(boxX + boxW - 6 - vw, textY);
        d.print(values[i]);
    }

    // Small hand-drawn triangles in the title bar corner -- same spirit as
    // drawFolderIcon() elsewhere, rather than relying on an arrow glyph
    // FreeSans doesn't have -- indicating there's more to scroll to.
    int indX = boxX + boxW - 14;
    if (settingsScroll > 0) {
        d.fillTriangle(indX, boxY + 15, indX - 4, boxY + 20, indX + 4, boxY + 20, COL_FOLDER);
    }
    if (settingsScroll + SETTINGS_VISIBLE < SETTINGS_COUNT) {
        int by = boxY + boxH - 2;
        d.fillTriangle(indX, by, indX - 4, by - 5, indX + 4, by - 5, COL_FOLDER);
    }
}

static void drawFullVis();   // defined below, in the full-screen visualizer section

// Drawn as a small centered box over whatever was already on screen (browser
// or Now Playing), rather than a full-screen view of its own -- redraws that
// backdrop first, then overlays the box on top of it. Only used for
// entering/exiting settings; in-place navigation uses drawSettingsBox() alone.
static void drawSettings() {
    if      (uiModeBeforeSettings == MODE_NOWPLAYING) drawNowPlaying();
    else if (uiModeBeforeSettings == MODE_FULLVIS)    drawFullVis();
    else                                               drawBrowser();
    // drawSettingsBox() below sets its own font first thing, regardless of
    // whatever the backdrop draw above left active.
    drawSettingsBox();
}

// Applies and advances the currently-selected setting to its next option.
// Redraws just the box in place -- see drawSettingsBox() note above -- except
// for Theme, which repaints the full backdrop too (drawSettings()) so you can
// actually see what the new palette looks like on the rest of the screen
// without backing out of Settings first. That's a heavier redraw than the
// other rows get, same cost as entering/exiting Settings, but it only happens
// on a deliberate, infrequent action, not a rapid held-key repeat.
static void cycleSetting(int idx) {
    switch (idx) {
        case 0:
            settingBacklightIdx = (settingBacklightIdx + 1) % BACKLIGHT_COUNT;
            M5Cardputer.Display.setBrightness(backlightValues[settingBacklightIdx]);
            break;
        case 1:
            settingScreenOffIdx = (settingScreenOffIdx + 1) % SCREEN_OFF_COUNT;
            break;
        case 2:
            albumEndMode = (AlbumEndMode)((albumEndMode + 1) % ALBUM_END_MODE_COUNT);
            break;
        case 3:
            settingThemeIdx = (settingThemeIdx + 1) % totalThemeCount();
            theme = themeAt(settingThemeIdx);
            saveSettings();
            drawSettings();
            return;
        case 4:
            settingScreenshotsEnabled = !settingScreenshotsEnabled;
            break;
    }
    saveSettings();
    drawSettingsBox();
}

// ---------- Now Playing screen ----------
// Title-line marquee. The decision to scroll at all is a plain character-count
// threshold -- not a pixel-width comparison -- so it doesn't depend on
// font-metric measurements lining up between contexts. Short titles are just
// printed once, like the artist/album lines, and never touched again.
//
// Reverted to the original direct-draw, two-copy, fast implementation at the
// user's request -- later attempts (sprite compositing, slower speed,
// monotonic-tick tiling) were meant to fix flicker/overlap/staleness but the
// title still wasn't scrolling right, so this goes back to the first known
// state to re-evaluate from there rather than layering on another fix blind.
static const int MARQUEE_MIN_CHARS = 20;   // titles at or under this length just sit static
static const int MARQUEE_STEP = 2, MARQUEE_INTERVAL_MS = 80, MARQUEE_GAP = 30;   // half the original speed
static bool titleScrolling = false;
static int titleTextWidth = 0;
static int titleScrollX = 0;
static unsigned long lastMarqueeMove = 0;

// Renders the scrolling title: one copy at titleScrollX, plus a second
// trailing copy one period behind so the loop reads continuously instead of
// going blank between passes. Called once (at titleScrollX=0) from
// drawNowPlaying() and repeatedly by marqueeTick() thereafter. Only used when
// titleScrolling is true -- the static case is handled directly in
// drawNowPlaying(), the same way the artist/album lines are.
static void drawTitleLine() {
    auto &d = M5Cardputer.Display;
    int y = NP_TEXT_Y + NP_LINE_H;
    int maxW = d.width() - NP_TEXT_X - 4;
    const char* title = curTitle[0] ? curTitle : nowPlaying;

    d.setClipRect(NP_TEXT_X, y, maxW, NP_LINE_H);
    d.fillRect(NP_TEXT_X, y, maxW, NP_LINE_H, COL_NP_BG);
    d.setTextColor(COL_DIM, COL_NP_BG);
    int x = NP_TEXT_X + titleScrollX;
    d.setCursor(x, y);
    d.print(title);
    if (titleScrolling) {
        d.setCursor(x + titleTextWidth + MARQUEE_GAP, y);
        d.print(title);
    }
    d.clearClipRect();
}

// Called every loop() iteration; only does anything (and only touches the title
// row) when the current title is long enough to scroll and enough time has passed.
static void marqueeTick() {
    if (uiMode != MODE_NOWPLAYING || !titleScrolling) return;
    unsigned long now = millis();
    if (now - lastMarqueeMove < MARQUEE_INTERVAL_MS) return;
    lastMarqueeMove = now;

    int period = titleTextWidth + MARQUEE_GAP;
    titleScrollX -= MARQUEE_STEP;
    if (titleScrollX <= -period) titleScrollX += period;
    drawTitleLine();
}

// Byte-position progress line below the album art, with a dot marking how far
// into the file playback has read. Approximate (byte offset, not decoded time)
// but cheap and good enough for a lightweight scrubber.
static const int PROG_Y = COVER_Y + COVER_H + 10;
static const int PROG_DOT_R = 3;
static const int PROG_SPRITE_H = (PROG_DOT_R + 1) * 2 + 2;

// Composited off-screen then blitted in one shot -- same reasoning as the
// title marquee sprite: drawing the clear+line+dot as three separate direct
// display calls showed a visible flash each update, and left faint dot
// remnants (the old dot briefly still visible while the clear/redraw caught up).
static M5Canvas *progSprite = nullptr;
static bool progSpriteOk = false;

static void drawProgressBar() {
    if (!file) return;
    uint32_t sz = file->getSize();
    uint32_t pos = file->getPos();
    float frac = (sz > 0) ? (float)pos / (float)sz : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    int lineY = PROG_DOT_R + 1;   // local y within the sprite/clear-rect
    int dotX = (int)(frac * COVER_W);

    if (progSpriteOk) {
        progSprite->fillSprite(COL_NP_BG);
        progSprite->drawFastHLine(0, lineY, COVER_W, COL_DIM);
        progSprite->fillCircle(dotX, lineY, PROG_DOT_R, COL_PROGRESS);
        progSprite->pushSprite(COVER_X, PROG_Y - PROG_DOT_R - 1);
        return;
    }

    // Fallback if the sprite couldn't be allocated -- same visual, may flicker.
    auto &d = M5Cardputer.Display;
    d.fillRect(COVER_X, PROG_Y - PROG_DOT_R - 1, COVER_W, PROG_SPRITE_H, COL_NP_BG);
    d.drawFastHLine(COVER_X, PROG_Y, COVER_W, COL_DIM);
    d.fillCircle(COVER_X + dotX, PROG_Y, PROG_DOT_R, COL_PROGRESS);
}

// Amplitude-history visualizer, bottom-right. Composited into a small sprite
// (same flicker-avoidance reasoning as the title marquee) sized/positioned
// once in setup() since it needs d.width()/height(), then just blitted here.
static const int VIS_BARS = VIS_HISTORY;
static const int VIS_BAR_GAP = 2, VIS_MAX_H = 40, VIS_MARGIN = 8;
static int VIS_BAR_W = 5;   // computed in setup() so the row spans NP_TEXT_X..width-VIS_MARGIN
static M5Canvas *visSprite = nullptr;
static bool visSpriteOk = false;
static int visLeft = 0, visTop = 0;

// A column of discrete LED-style segments instead of one continuous bar,
// colored by level tier (COL_PLAY/COL_VIS_MID/COL_VIS_HIGH) like a classic
// hardware VU meter. Used to be one of two selectable styles (Settings ->
// Visualizer); the plain solid-bar style was removed in favor of this one,
// so the setting itself went away too.
static const int VIS_SEG_H = 4, VIS_SEG_GAP = 1;
static const int VIS_SEG_COUNT = VIS_MAX_H / (VIS_SEG_H + VIS_SEG_GAP);

// Bars are bottom-anchored and grow upward (taller = louder), like a classic
// hardware EQ display, not top-down.
static void drawVisualizer() {
    if (!visSpriteOk) return;
    visSprite->fillSprite(COL_NP_BG);
    for (int i = 0; i < VIS_BARS; i++) {
        float lvl = visHistory[i];
        if (lvl < 0.0f) lvl = 0.0f;
        if (lvl > 1.0f) lvl = 1.0f;
        int h = (int)(lvl * VIS_MAX_H);
        if (h < 1) h = 1;   // faint baseline tick even at silence

        int bx = i * (VIS_BAR_W + VIS_BAR_GAP);

        for (int s = 0; s < VIS_SEG_COUNT; s++) {
            int y = VIS_MAX_H - (s + 1) * VIS_SEG_H - s * VIS_SEG_GAP;
            int segTopFromBottom = (s + 1) * VIS_SEG_H + s * VIS_SEG_GAP;
            bool lit = h >= segTopFromBottom;
            float tier = (float)(s + 1) / VIS_SEG_COUNT;
            uint16_t litColor = tier <= 0.6f ? COL_PLAY : tier <= 0.85f ? COL_VIS_MID : COL_VIS_HIGH;
            visSprite->fillRect(bx, y, VIS_BAR_W, VIS_SEG_H, lit ? litColor : COL_VIS_IDLE);
        }
    }
    visSprite->pushSprite(visLeft, visTop);
}

// ---------- Full-screen visualizer (Now Playing -> KEY_FULLVIS) ----------
// A real per-frequency-bin spectrum via fft_t above, filling the whole
// screen, as an alternative to the small amplitude-history widget on the
// regular Now Playing screen -- ported (in spirit, not verbatim) from the
// same AdvanceOS reference cited near fft_t: stereo level meter up top,
// bottom-anchored frequency bars with decaying peak-hold dots, and a
// waveform trace overlaid on top of the bars.
//
// The reference redraws only the pixels that changed frame-to-frame (manual
// erase/redraw bookkeeping per column). This instead composites a full
// screen-sized sprite and blits it once, like everything else in this file
// -- simpler and safer to get right without eyes on real hardware, at the
// cost of a heavier per-frame redraw. To keep that from competing with the
// decoder the way a full drawBrowser() once did, it's throttled to
// FULLVIS_INTERVAL_MS rather than redrawing every loop() iteration, and the
// sprite itself is allocated lazily (only if this mode is actually entered),
// not at boot -- it's 240*135*2 = ~64KB, more than worth avoiding as a
// permanent reservation on a board with no PSRAM if it's never used.
static const int FULLVIS_BAR_W = 4;
static const int FULLVIS_METER_H = 2, FULLVIS_METER_GAP = 1;
static const uint32_t FULLVIS_PEAK_DECAY_MS = 40;   // peak dot falls one pixel this often
static const uint32_t FULLVIS_INTERVAL_MS = 66;     // ~15fps

static const int FULLVIS_MAX_BARS = 80;   // generous upper bound for any screen width
static int fvBarCount = 0;
static float fvPeakY[FULLVIS_MAX_BARS];   // in bar-height pixels, measured up from the bars area's bottom
static unsigned long fvLastPeakDecay = 0;

static M5Canvas *fullVisSprite = nullptr;
static bool fullVisSpriteOk = false;

// Two full-screen styles, cycled with repeated KEY_FULLVIS presses from Now
// Playing: Spectrum (the FFT bars below) -> Dancers (see further down) ->
// back to the regular Now Playing screen. Always re-enters at Spectrum.
enum FullVisStyle { FULLVIS_SPECTRUM, FULLVIS_DANCERS, FULLVIS_STYLE_COUNT };
static FullVisStyle fullVisStyle = FULLVIS_SPECTRUM;

static void drawFullVisSpectrum();   // defined here
static void drawFullVisDancers(uint32_t dtMs);   // defined further down, near the dancer frame data

// Only allocates (and only once) on first entry -- see the note above.
static void enterFullVis() {
    auto &d = M5Cardputer.Display;
    if (!fullVisSprite) {
        fullVisSprite = new M5Canvas(&d);
        fullVisSprite->setPsram(false);
        fullVisSprite->setColorDepth(16);
        fullVisSpriteOk = (fullVisSprite->createSprite(d.width(), d.height()) != nullptr);
        if (!fullVisSpriteOk) { Serial.println("full-screen visualizer sprite alloc failed"); return; }

        fvBarCount = d.width() / FULLVIS_BAR_W;
        if (fvBarCount > FULLVIS_MAX_BARS) fvBarCount = FULLVIS_MAX_BARS;
        if (fvBarCount > FFT_SIZE / 2) fvBarCount = FFT_SIZE / 2;
    }
    if (!fullVisSpriteOk) return;   // alloc failed earlier -- stay on the regular Now Playing screen
    for (int i = 0; i < fvBarCount; i++) fvPeakY[i] = 0;
    fullVisStyle = FULLVIS_SPECTRUM;
    uiMode = MODE_FULLVIS;
    needsRedraw = true;
}

// Dispatches to whichever style is active, and hands the dancers style a
// measured dt (needed for its beat-reactive step timing; the spectrum style
// tracks its own peak-decay timing independently and doesn't need it).
static void drawFullVis() {
    if (!fullVisSpriteOk) return;
    static unsigned long lastCall = 0;
    unsigned long now = millis();
    uint32_t dt = lastCall ? (uint32_t)(now - lastCall) : FULLVIS_INTERVAL_MS;
    lastCall = now;
    if (fullVisStyle == FULLVIS_DANCERS) drawFullVisDancers(dt);
    else drawFullVisSpectrum();
}

static void drawFullVisSpectrum() {
    auto &d = M5Cardputer.Display;
    int w = d.width(), h = d.height();

    fft.exec(visRawBuf);
    fullVisSprite->fillSprite(COL_NP_BG);

    // ---- stereo level meter (two thin rows at the very top) ----
    for (int ch = 0; ch < 2; ch++) {
        int32_t level = 0;
        for (int j = ch; j < VISRAW_FRAMES * 2; j += 32) {
            int32_t lv = abs((int)visRawBuf[j]);
            if (lv > level) level = lv;
        }
        int lit = (int)(((int64_t)level * w) / 32767);
        int y = ch * (FULLVIS_METER_H + FULLVIS_METER_GAP);
        if (lit > 0) fullVisSprite->fillRect(0, y, lit, FULLVIS_METER_H, COL_PLAY);
        if (lit < w) fullVisSprite->fillRect(lit, y, w - lit, FULLVIS_METER_H, COL_VIS_IDLE);
    }

    // ---- frequency bars + decaying peak-hold dots ----
    int barsTop = 2 * (FULLVIS_METER_H + FULLVIS_METER_GAP) + 2;
    int barsH = h - barsTop;
    bool decayPeak = (millis() - fvLastPeakDecay >= FULLVIS_PEAK_DECAY_MS);
    if (decayPeak) fvLastPeakDecay = millis();

    for (int bx = 0; bx < fvBarCount; bx++) {
        uint32_t f = fft.get(bx);
        int barH = (int)((f * (uint32_t)barsH) >> 18);
        if (barH > barsH) barH = barsH;

        if (barH > fvPeakY[bx]) fvPeakY[bx] = (float)barH;
        else if (decayPeak && fvPeakY[bx] > 0) fvPeakY[bx] -= 1.0f;

        int x = bx * FULLVIS_BAR_W;
        float tier = (float)(bx + 1) / fvBarCount;
        uint16_t barColor = tier <= 0.6f ? COL_PLAY : tier <= 0.85f ? COL_VIS_MID : COL_VIS_HIGH;

        if (barH > 0) fullVisSprite->fillRect(x, h - barH, FULLVIS_BAR_W - 1, barH, barColor);
        int peakY = (int)fvPeakY[bx];
        if (peakY > 1) fullVisSprite->fillRect(x, h - peakY, FULLVIS_BAR_W - 1, 1, COL_NP_TEXT);
    }

    // ---- waveform trace, overlaid on top of the bars ----
    int midY = barsTop + barsH / 2;
    int prevX = -1, prevY = midY;
    int waveCols = w < VISRAW_FRAMES ? w : VISRAW_FRAMES;
    for (int i = 0; i < waveCols; i++) {
        int32_t sum = (int32_t)visRawBuf[i * 2] + (int32_t)visRawBuf[i * 2 + 1];
        int y = midY - (sum * (barsH / 2)) / 65536;
        if (y < barsTop) y = barsTop; else if (y >= h) y = h - 1;
        if (prevX >= 0) fullVisSprite->drawLine(prevX, prevY, i, y, COL_PROGRESS);
        prevX = i; prevY = y;
    }

    fullVisSprite->pushSprite(0, 0);
}

// ---------- Dancers full-screen style ----------
// Four distinct silhouette dancers (dancerN_frames.h, N=0..3 -- see each
// file's header comment for provenance/crop notes), same 1-bit-mask-
// recolored-per-theme technique as the turntable placeholder. Two of the
// four (dancer2, dancer3) turned out to have genuine native loop points in
// their source clips (found by comparing downsampled frames across the
// whole clip for the closest start/end pose match, not guessed), so those
// play straight through; the other two didn't, so they play ping-pong
// (forward through the frames, then back) instead of jumping the last frame
// to the first, which would show a visible pop.
//
// "Synced to the music" here means reacting to bass hits, not tracking
// actual tempo/BPM -- real beat-tracking is a much harder, failure-prone DSP
// problem I can't tune without hearing it on the device, so this instead
// nudges playback speed up on each detected bass transient (a simple
// threshold-over-rolling-average onset detector) and lets it decay back to
// a steady baseline. It'll feel reactive to the music's punches, not
// metronome-locked to it.
enum DancerLoop { DANCE_PINGPONG, DANCE_NATIVE_LOOP };
struct DancerAsset {
    const uint8_t* frames;
    int w, h, frameCount, bytesPerFrame;
    DancerLoop loop;
};
static const int DANCER_COUNT = 4;
static const DancerAsset DANCERS[DANCER_COUNT] = {
    { dancer0Frames, DANCER0_W, DANCER0_H, DANCER0_FRAME_COUNT, DANCER0_BYTES_PER_FRAME, DANCE_PINGPONG },
    { dancer1Frames, DANCER1_W, DANCER1_H, DANCER1_FRAME_COUNT, DANCER1_BYTES_PER_FRAME, DANCE_PINGPONG },
    { dancer2Frames, DANCER2_W, DANCER2_H, DANCER2_FRAME_COUNT, DANCER2_BYTES_PER_FRAME, DANCE_NATIVE_LOOP },
    { dancer3Frames, DANCER3_W, DANCER3_H, DANCER3_FRAME_COUNT, DANCER3_BYTES_PER_FRAME, DANCE_NATIVE_LOOP },
};

static int fvDancerX[DANCER_COUNT];
static int fvDancerY[DANCER_COUNT];
static bool fvDancerLayoutDone = false;

static float fvDancerPos = 0.0f;       // shared continuous "step" position -- each dancer maps it onto its own cycle length, so they drift in and out of phase with each other rather than all four looping in lockstep
static float fvDanceSpeedMul = 1.0f;   // >1 right after a beat, decays back to 1
static float fvBassRollingAvg = 0.0f;
static unsigned long fvLastBeatMs = 0;

static const float DANCE_BASE_STEPS_PER_SEC = 7.0f;   // baseline pace at speed 1x -- tune to taste
static const float DANCE_BEAT_THRESHOLD = 1.5f;       // envelope must exceed rollingAvg * this to count as a hit
static const float DANCE_BEAT_MIN_GAP_MS = 200.0f;    // debounce so one hit doesn't retrigger repeatedly
static const float DANCE_BEAT_SPEED_BOOST = 2.2f;     // speed multiplier applied right on a hit

static void resetDancePhysics() {
    fvDancerPos = 0.0f;
    fvDanceSpeedMul = 1.0f;
    fvBassRollingAvg = lastBassEnvelope;
    fvLastBeatMs = 0;
}

// Even horizontal slots (screenW / DANCER_COUNT wide each), dancer centered
// within its own slot regardless of that dancer's actual width -- overlap
// between neighbors when a silhouette is wider than its slot is expected and
// fine (asked for explicitly: "ok if they overlap at times"). Each dancer is
// bottom-anchored using its own height, since the four source clips aren't
// all the same aspect ratio.
static void layoutDancers(int screenW, int screenH) {
    int slotW = screenW / DANCER_COUNT;
    for (int i = 0; i < DANCER_COUNT; i++) {
        const DancerAsset& a = DANCERS[i];
        fvDancerX[i] = slotW * i + (slotW - a.w) / 2;
        fvDancerY[i] = screenH - a.h - 8;
        if (fvDancerY[i] < 0) fvDancerY[i] = 0;
    }
    fvDancerLayoutDone = true;
}

static void updateDancePhysics(uint32_t dtMs) {
    fvBassRollingAvg += 0.04f * (lastBassEnvelope - fvBassRollingAvg);
    unsigned long now = millis();
    if (lastBassEnvelope > fvBassRollingAvg * DANCE_BEAT_THRESHOLD + 0.015f &&
        now - fvLastBeatMs > (unsigned long)DANCE_BEAT_MIN_GAP_MS) {
        fvLastBeatMs = now;
        fvDanceSpeedMul = DANCE_BEAT_SPEED_BOOST;
    }
    fvDanceSpeedMul += (1.0f - fvDanceSpeedMul) * 0.12f;   // relax back toward 1x
    fvDancerPos += (dtMs / 1000.0f) * DANCE_BASE_STEPS_PER_SEC * fvDanceSpeedMul;
}

// Maps the shared position (plus a per-dancer phase seed, so all 4 don't
// start in lockstep) onto a frame index, per that dancer's own loop mode.
static int dancerFrameIndex(float pos, float phaseOffset, const DancerAsset& a) {
    if (a.loop == DANCE_NATIVE_LOOP) {
        float p = fmodf(pos + phaseOffset, (float)a.frameCount);
        if (p < 0) p += (float)a.frameCount;
        return (int)p;
    }
    float cycleLen = 2.0f * (a.frameCount - 1);
    float p = fmodf(pos + phaseOffset, cycleLen);
    if (p < 0) p += cycleLen;
    int idx = (int)p;
    return idx < a.frameCount ? idx : (int)cycleLen - idx;
}

// Stamps "on" mask pixels straight into dest's buffer in the theme's
// foreground color -- doesn't touch "off" pixels, so whatever's already
// there (the background fill, or another dancer drawn first) shows through,
// unlike drawTurntableFrame()'s full fill+redraw (that one owns its whole
// sprite; this one shares the full-screen canvas with 3 other dancers).
static void blitDancer(M5Canvas* dest, int destX, int destY, const DancerAsset& a, int frameIdx, bool mirror, uint16_t fg) {
    const uint8_t* frame = &a.frames[frameIdx * a.bytesPerFrame];
    uint16_t* buf = (uint16_t*)dest->getBuffer();
    int destW = dest->width(), destH = dest->height();
    int rowBytes = (a.w + 7) / 8;
    uint16_t fgSwapped = (fg >> 8) | (fg << 8);
    for (int y = 0; y < a.h; y++) {
        int dy = destY + y;
        if (dy < 0 || dy >= destH) continue;
        const uint8_t* row = frame + y * rowBytes;
        for (int x = 0; x < a.w; x++) {
            int srcX = mirror ? (a.w - 1 - x) : x;
            bool on = (row[srcX >> 3] >> (7 - (srcX & 7))) & 1;
            if (!on) continue;
            int dx = destX + x;
            if (dx < 0 || dx >= destW) continue;
            buf[dy * destW + dx] = fgSwapped;
        }
    }
}

static void drawFullVisDancers(uint32_t dtMs) {
    auto &d = M5Cardputer.Display;
    if (!fvDancerLayoutDone) layoutDancers(d.width(), d.height());
    updateDancePhysics(dtMs);

    fullVisSprite->fillSprite(COL_NP_BG);
    for (int i = 0; i < DANCER_COUNT; i++) {
        const DancerAsset& a = DANCERS[i];
        float phase = i * 11.0f;   // arbitrary desync seed so all 4 don't start in unison
        int frameIdx = dancerFrameIndex(fvDancerPos, phase, a);
        bool mirror = (i % 2) == 1;   // alternate facing for a bit more visual variety
        blitDancer(fullVisSprite, fvDancerX[i], fvDancerY[i], a, frameIdx, mirror, COL_NP_TEXT);
    }
    fullVisSprite->pushSprite(0, 0);
}

static void drawNowPlaying() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT_UI);   // the browser may have left FONT_BROWSER active
    d.fillRect(0, 0, d.width(), d.height(), COL_NP_BG);   // no bottom status bar on this screen

    drawArtRegion();
    drawStatusIcons();

    int maxW = d.width() - NP_TEXT_X - 4;

    d.setTextColor(COL_NP_TEXT, COL_NP_BG);
    d.setCursor(NP_TEXT_X, NP_TEXT_Y);
    d.print(trimToWidth(d, curArtist[0] ? curArtist : "(unknown artist)", maxW));

    const char* title = curTitle[0] ? curTitle : nowPlaying;
    titleScrolling = strlen(title) > MARQUEE_MIN_CHARS;
    if (titleScrolling) {
        titleTextWidth = d.textWidth(title);
        titleScrollX = 0;
        drawTitleLine();
    } else {
        d.setTextColor(COL_DIM, COL_NP_BG);
        d.setCursor(NP_TEXT_X, NP_TEXT_Y + NP_LINE_H);
        d.print(trimToWidth(d, title, maxW));
    }

    d.setTextColor(COL_DIM, COL_NP_BG);
    d.setCursor(NP_TEXT_X, NP_TEXT_Y + 2 * NP_LINE_H);
    d.print(trimToWidth(d, curAlbum, maxW));

    drawProgressBar();
    drawVisualizer();
}

// ---------- Startup splash ----------
// Shown once at boot, before SD/audio init -- doesn't depend on either, so the
// logo shows even if the card is missing or fails to mount. The PNG is
// embedded (ember_logo.h) rather than read from SD for the same reason.
// Solid logo on black, chosen over a glowing variant after comparing both on
// hardware. Auto-continues after a short beat, or immediately on any keypress.
static void showSplash() {
    auto &d = M5Cardputer.Display;
    const float scale = 2.0f;
    const int logoW = (int)(107 * scale), logoH = (int)(32 * scale);
    const int logoX = (d.width() - logoW) / 2;
    const int logoY = (d.height() - logoH) / 2 + 2;

    d.fillScreen(TFT_BLACK);
    d.drawPng(emberLogoPng, emberLogoPngLen, logoX, logoY, logoW, logoH, 0, 0, scale, scale);

    const unsigned long start = millis();
    while (millis() - start < 1500) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
    }
}

// ---------- Screenshots (KEY_SCREENSHOT, any screen) ----------
// Off by default -- see settingScreenshotsEnabled (Settings -> Screenshots).
// For pulling real on-device screenshots for documentation/README use
// without photographing the screen. Reads the framebuffer back over SPI via
// LovyanGFX's readRect() and writes a plain 24-bit uncompressed BMP to SD --
// untested on this specific panel, since screen readback support/speed
// varies a lot by panel controller. If this comes out blank, garbled, or
// visibly wrong-side-out colorwise, that's readback quirks to work around,
// not an obviously-wrong approach.
//
// readRect(..., uint16_t*) hands back swap565 (byte-swapped RGB565), same
// as M5Canvas's raw sprite buffer elsewhere in this file -- see
// boostArtVibrance()'s comment for why that byte-swap matters.
//
// Holding the key repeats the capture (same hold-repeat pattern as seeking)
// so a short animation -- the visualizer, the turntable, the dancers -- can
// be captured as a numbered sequence of BMPs and stitched into a GIF
// afterward (e.g. `ffmpeg -i shot_%03d.bmp -vf palettegen palette.png` then
// `ffmpeg -i shot_%03d.bmp -i palette.png -lavfi paletteuse out.gif`).
static int screenshotIndex = -1;   // -1 = not yet scanned this boot

static void initScreenshotIndex() {
    if (screenshotIndex >= 0) return;
    screenshotIndex = 0;
    if (!SD.exists("/screenshots")) SD.mkdir("/screenshots");
    File dir = SD.open("/screenshots");
    if (dir) {
        File e = dir.openNextFile();
        while (e) {
            int n = 0;
            if (sscanf(baseName(e.name()), "shot_%d.bmp", &n) == 1 && n >= screenshotIndex) {
                screenshotIndex = n + 1;
            }
            e.close();
            e = dir.openNextFile();
        }
        dir.close();
    }
}

static void saveScreenshot() {
    if (!settingScreenshotsEnabled) return;   // off by default -- Settings -> Screenshots
    initScreenshotIndex();
    auto &d = M5Cardputer.Display;
    int w = d.width(), h = d.height();
    int rowSize = ((w * 3 + 3) / 4) * 4;   // BMP rows padded to a 4-byte boundary
    uint32_t dataSize = (uint32_t)rowSize * h;

    char path[48];
    snprintf(path, sizeof(path), "/screenshots/shot_%03d.bmp", screenshotIndex);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("screenshot: failed to open %s\n", path); return; }
    screenshotIndex++;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t fileSize = 54 + dataSize;
    memcpy(hdr + 2,  &fileSize, 4);
    uint32_t dataOffset = 54;
    memcpy(hdr + 10, &dataOffset, 4);
    uint32_t dibSize = 40;
    memcpy(hdr + 14, &dibSize, 4);
    int32_t bw = w, bh = h;                // positive height => bottom-up row order (BMP's native order)
    memcpy(hdr + 18, &bw, 4);
    memcpy(hdr + 22, &bh, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &dataSize, 4);
    f.write(hdr, sizeof(hdr));

    uint16_t rowPixels[320];   // wide enough for this display; bump if it ever changes
    uint8_t rowOut[960];
    for (int y = h - 1; y >= 0; y--) {     // write bottom row first
        d.readRect(0, y, w, 1, rowPixels);
        int o = 0;
        for (int x = 0; x < w; x++) {
            uint16_t raw = rowPixels[x];
            uint16_t px = (raw >> 8) | (raw << 8);   // undo swap565
            rowOut[o++] = (uint8_t)(((px & 0x1F) * 255) / 31);          // B
            rowOut[o++] = (uint8_t)((((px >> 5) & 0x3F) * 255) / 63);   // G
            rowOut[o++] = (uint8_t)((((px >> 11) & 0x1F) * 255) / 31);  // R
        }
        while (o < rowSize) rowOut[o++] = 0;
        f.write(rowOut, rowSize);
    }
    f.close();
    Serial.printf("screenshot saved: %s\n", path);
}

// ---------- Custom themes (SD: /themes/*.json) ----------
// One JSON file per theme, same shape tools/theme-editor.html exports (a
// flat "colors" object of "#rrggbb" strings). Hand-rolled substring-search
// parsing rather than pulling in a JSON library -- matches this file's
// existing pragmatic parsing style (see the ID3/image-header parsing
// elsewhere) and the format is fixed/simple enough not to need a real
// parser. A field that's missing or fails to parse (e.g. an older export
// made before npBg/visIdle existed) falls back to Ember's value for that
// field rather than failing the whole theme.
static bool findJsonHex(const char* json, const char* key, char* out, size_t outSize) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    const char* colon = strchr(p + strlen(pat), ':');
    if (!colon) return false;
    const char* q1 = strchr(colon, '"');
    if (!q1) return false;
    q1++;
    const char* q2 = strchr(q1, '"');
    if (!q2) return false;
    size_t len = q2 - q1;
    if (len >= outSize) len = outSize - 1;
    memcpy(out, q1, len);
    out[len] = '\0';
    return true;
}

static uint16_t parseHexColor(const char* hex, uint16_t fallback) {
    if (hex[0] == '#') hex++;
    if (strlen(hex) < 6) return fallback;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    int r = (hv(hex[0]) << 4) | hv(hex[1]);
    int g = (hv(hex[2]) << 4) | hv(hex[3]);
    int b = (hv(hex[4]) << 4) | hv(hex[5]);
    return rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static bool parseThemeJson(const char* json, Theme& outTheme, char* outName, size_t nameSize) {
    if (!findJsonHex(json, "name", outName, nameSize)) {
        strncpy(outName, "Custom", nameSize - 1);
        outName[nameSize - 1] = '\0';
    }
    struct FieldRef { const char* key; uint16_t Theme::*field; };
    static const FieldRef fields[] = {
        {"bg", &Theme::bg}, {"header", &Theme::header}, {"headerText", &Theme::headerText},
        {"folder", &Theme::folder}, {"file", &Theme::file}, {"selBg", &Theme::selBg},
        {"selFg", &Theme::selFg}, {"dim", &Theme::dim}, {"play", &Theme::play},
        {"npText", &Theme::npText}, {"volumeIcon", &Theme::volumeIcon}, {"progressDot", &Theme::progressDot},
        {"visMid", &Theme::visMid}, {"visHigh", &Theme::visHigh}, {"npBg", &Theme::npBg}, {"visIdle", &Theme::visIdle},
    };
    outTheme = THEME_EMBER;
    char hexBuf[16];
    bool foundAny = false;
    for (const auto& f : fields) {
        if (findJsonHex(json, f.key, hexBuf, sizeof(hexBuf))) {
            outTheme.*(f.field) = parseHexColor(hexBuf, outTheme.*(f.field));
            foundAny = true;
        }
    }
    return foundAny;
}

// Scans /themes/*.json on SD -- called once from setup(), after SD.begin()
// succeeds. Silently skips anything that doesn't parse as a theme (e.g. a
// non-JSON file dropped in there by mistake) rather than failing the whole
// scan over one bad file.
static void loadCustomThemes() {
    customThemeCount = 0;
    if (!SD.exists("/themes")) return;
    File dir = SD.open("/themes");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }

    static char jsonBuf[2048];
    File e = dir.openNextFile();
    while (e && customThemeCount < MAX_CUSTOM_THEMES) {
        if (!e.isDirectory()) {
            const char* nm = baseName(e.name());
            int len = strlen(nm);
            if (len > 5 && strcasecmp(nm + len - 5, ".json") == 0) {
                int n = e.read((uint8_t*)jsonBuf, sizeof(jsonBuf) - 1);
                if (n > 0) {
                    jsonBuf[n] = '\0';
                    if (parseThemeJson(jsonBuf, customThemes[customThemeCount],
                                        customThemeNames[customThemeCount], sizeof(customThemeNames[0]))) {
                        customThemeCount++;
                    }
                }
            }
        }
        e.close();
        e = dir.openNextFile();
    }
    dir.close();
    if (customThemeCount > 0) Serial.printf("loaded %d custom theme(s) from /themes\n", customThemeCount);
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    // Backlight/screen-off/album-end are valid immediately; themeIdx is only
    // provisional until loadCustomThemes() (after SD.begin(), further down)
    // confirms how many themes actually exist this boot.
    loadSettings();
    auto cfg = M5.config();
    cfg.external_speaker.hat_spk = true;
    M5Cardputer.begin(cfg, true);
    delay(100);

    auto spk_cfg = M5Cardputer.Speaker.config();
    spk_cfg.sample_rate      = 44100;
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    spk_cfg.dma_buf_count    = 8;
    spk_cfg.dma_buf_len      = 256;
    spk_cfg.task_priority    = 3;
    M5Cardputer.Speaker.config(spk_cfg);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(volume);

    auto &d = M5Cardputer.Display;
    d.setRotation(1);
    d.setFont(&fonts::lgfxJapanGothic_16);
    d.setTextWrap(false);
    d.fillScreen(COL_BG);

    showSplash();

    visibleRows = (d.height() - HEADER_H) / ROW_H;

    // Board has no PSRAM -- M5Canvas defaults to PSRAM allocation, which would
    // silently fail createSprite() here, so force internal SRAM explicitly.
    coverSprite = new M5Canvas(&d);
    coverSprite->setPsram(false);
    coverSprite->setColorDepth(16);
    coverSpriteOk = (coverSprite->createSprite(COVER_W, COVER_H) != nullptr);
    if (!coverSpriteOk) Serial.println("cover sprite alloc failed");

    turntableSprite = new M5Canvas(&d);
    turntableSprite->setPsram(false);
    turntableSprite->setColorDepth(16);
    turntableSpriteOk = (turntableSprite->createSprite(COVER_W, COVER_H) != nullptr);
    if (!turntableSpriteOk) Serial.println("turntable sprite alloc failed");

    // Span the same left margin as the text column (NP_TEXT_X) out to the same
    // right margin the battery/volume meters use, dividing that span evenly
    // across VIS_BARS bars.
    int visAvailW = d.width() - VIS_MARGIN - NP_TEXT_X;
    int pitch = visAvailW / VIS_BARS;
    VIS_BAR_W = pitch - VIS_BAR_GAP;
    int visTotalW = VIS_BARS * VIS_BAR_W + (VIS_BARS - 1) * VIS_BAR_GAP;
    visLeft = NP_TEXT_X;
    visTop  = d.height() - VIS_MARGIN - VIS_MAX_H;
    visSprite = new M5Canvas(&d);
    visSprite->setPsram(false);
    visSprite->setColorDepth(16);
    visSpriteOk = (visSprite->createSprite(visTotalW, VIS_MAX_H) != nullptr);
    if (!visSpriteOk) Serial.println("visualizer sprite alloc failed");

    progSprite = new M5Canvas(&d);
    progSprite->setPsram(false);
    progSprite->setColorDepth(16);
    progSpriteOk = (progSprite->createSprite(COVER_W, PROG_SPRITE_H) != nullptr);
    if (!progSpriteOk) Serial.println("progress sprite alloc failed");

    out = new AudioOutputM5Speaker(&M5Cardputer.Speaker, 0);
    out->begin();

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    bool ok = SD.begin(SD_CS, SPI, 25000000);
    if (!ok) ok = SD.begin(SD_CS, SPI, 4000000);
    if (!ok) {
        d.setTextColor(TFT_RED, COL_BG);
        d.setCursor(4, 4); d.print("SD init FAILED");
        Serial.println("SD init failed");
        return;
    }
    Serial.println("SD ok");

    // Now that SD is up, pull in any user themes and re-clamp the persisted
    // theme index against the real total (loadSettings() above only knew
    // about the built-in count when it ran).
    loadCustomThemes();
    if (settingThemeIdx >= totalThemeCount()) settingThemeIdx = 0;
    theme = themeAt(settingThemeIdx);

    loadDir();

    d.setBrightness(backlightValues[settingBacklightIdx]);
    lastInputTime = millis();

    drawBrowser();
    needsRedraw = false;
}

void loop() {
    M5Cardputer.update();

    // ---- pump decoder; auto-advance on track end ----
    if (playState == PLAYING && decoder && decoder->isRunning()) {
        if (!decoder->loop()) {
            if (queuePos + 1 < queueCount) {
                nextTrack();          // more tracks left in this album
            } else {
                // reached the end of the album -- behavior per the Settings screen
                switch (albumEndMode) {
                    case ALBUM_LOOP: nextTrack(); break;   // wraps to track 0
                    case ALBUM_NEXT: playNextAlbum(); break;
                    default:
                        stopPlayback();
                        playState = STOPPED;
                        nowPlaying[0] = '\0';
                        needsRedraw = true;
                        break;
                }
            }
        }
    }

    // ---- keyboard ----
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        lastInputTime = millis();
        if (screenIsOff) {
            // First press after screen-off just wakes it -- don't also act on it.
            // Force a full repaint since state (track, cursor, etc.) may have
            // changed while nothing was being drawn.
            screenIsOff = false;
            M5Cardputer.Display.setBrightness(backlightValues[settingBacklightIdx]);
            needsRedraw = true;
        } else {
            auto ks = M5Cardputer.Keyboard.keysState();
            if (ks.enter) {
                if (uiMode == MODE_BROWSER) openSelected();
                else if (uiMode == MODE_SETTINGS) cycleSetting(settingsCursor);
            }
            if (ks.space) togglePause();
            for (char c : ks.word) {
                if (c == KEY_SETTINGS) {
                    if (uiMode == MODE_SETTINGS) { uiMode = uiModeBeforeSettings; }
                    else { uiModeBeforeSettings = uiMode; uiMode = MODE_SETTINGS; settingsCursor = 0; settingsScroll = 0; }
                    needsRedraw = true;
                } else if (c == KEY_NOWPLAYING) {
                    // Nothing to show there with no track loaded -- only allow
                    // switching in, not out (leaving Now Playing always works).
                    if (uiMode == MODE_NOWPLAYING || uiMode == MODE_FULLVIS) { uiMode = MODE_BROWSER; needsRedraw = true; }
                    else if (playState != STOPPED) { uiMode = MODE_NOWPLAYING; needsRedraw = true; }
                } else if (uiMode == MODE_NOWPLAYING && c == KEY_FULLVIS) {
                    enterFullVis();   // always starts at the Spectrum style
                } else if (uiMode == MODE_FULLVIS && c == KEY_FULLVIS) {
                    // Cycle Spectrum -> Dancers -> back to the regular Now Playing screen.
                    if (fullVisStyle == FULLVIS_SPECTRUM) {
                        fullVisStyle = FULLVIS_DANCERS;
                        resetDancePhysics();
                        needsRedraw = true;
                    } else {
                        uiMode = MODE_NOWPLAYING;
                        needsRedraw = true;
                    }
                } else if (c == '`') {
                    // Backtick is a universal "back" regardless of mode -- always
                    // exits, even in Now Playing where KEY_BACK itself now means
                    // "seek backward" instead (see below).
                    if (uiMode == MODE_SETTINGS)        { uiMode = uiModeBeforeSettings; needsRedraw = true; }
                    else if (uiMode == MODE_NOWPLAYING || uiMode == MODE_FULLVIS) { uiMode = MODE_BROWSER; needsRedraw = true; }
                    else goBack();
                } else if (uiMode == MODE_SETTINGS && c == KEY_UP) {
                    settingsCursor = (settingsCursor - 1 + SETTINGS_COUNT) % SETTINGS_COUNT;
                    if (settingsCursor < settingsScroll) settingsScroll = settingsCursor;
                    if (settingsCursor >= settingsScroll + SETTINGS_VISIBLE) settingsScroll = settingsCursor - SETTINGS_VISIBLE + 1;
                    drawSettingsBox();
                } else if (uiMode == MODE_SETTINGS && c == KEY_DOWN) {
                    settingsCursor = (settingsCursor + 1) % SETTINGS_COUNT;
                    if (settingsCursor < settingsScroll) settingsScroll = settingsCursor;
                    if (settingsCursor >= settingsScroll + SETTINGS_VISIBLE) settingsScroll = settingsCursor - SETTINGS_VISIBLE + 1;
                    drawSettingsBox();
                } else if (uiMode == MODE_SETTINGS && c == KEY_OPEN) {
                    cycleSetting(settingsCursor);
                } else if (uiMode == MODE_SETTINGS && c == KEY_BACK) {
                    uiMode = uiModeBeforeSettings; needsRedraw = true;
                } else if (uiMode == MODE_NOWPLAYING && c == KEY_ART_TOGGLE) {
                    preferTurntable = !preferTurntable;
                    drawArtRegion();
                } else if (uiMode == MODE_NOWPLAYING && c == KEY_OPEN) {
                    // "right arrow" -- seek forward, or skip to next track on a quick double-tap
                    unsigned long now = millis();
                    if (now - lastRightTapTime <= DOUBLE_TAP_MS) nextTrack();
                    else seekByPercent(+SEEK_STEP_PCT);
                    lastRightTapTime = now;
                } else if (uiMode == MODE_NOWPLAYING && c == KEY_BACK) {
                    // "left arrow" -- seek backward, or restart on a quick double-tap
                    unsigned long now = millis();
                    if (now - lastLeftTapTime <= DOUBLE_TAP_MS) restartTrack();
                    else seekByPercent(-SEEK_STEP_PCT);
                    lastLeftTapTime = now;
                } else if (uiMode == MODE_BROWSER && c == KEY_BACK) {
                    goBack();
                } else if (uiMode == MODE_BROWSER && c == KEY_UP)   moveCursor(-1);
                else if (uiMode == MODE_BROWSER && c == KEY_DOWN) moveCursor(+1);
                else if (uiMode == MODE_BROWSER && c == KEY_OPEN) openSelected();
                else if (c == KEY_NEXT)  nextTrack();
                else if (c == KEY_PREV)  prevTrack();
                else if (c == KEY_VOLUP) changeVolume(+15);
                else if (c == KEY_VOLDN) changeVolume(-15);
                else if (c == KEY_SCREENSHOT) saveScreenshot();
            }
        }
    }

    // ---- seek key hold-repeat ----
    // isChange()-gated taps above handle a single press; this handles "and
    // more so with hold" by polling isKeyPressed() (level, not edge) once the
    // key's been down past an initial delay, then repeating on its own timer.
    if (!screenIsOff && uiMode == MODE_NOWPLAYING) {
        unsigned long now = millis();
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_OPEN)) {
            if (rightHoldNext == 0) rightHoldNext = now + SEEK_HOLD_DELAY_MS;
            else if (now >= rightHoldNext) { seekByPercent(+SEEK_STEP_PCT); rightHoldNext = now + SEEK_HOLD_REPEAT_MS; }
        } else rightHoldNext = 0;

        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACK)) {
            if (leftHoldNext == 0) leftHoldNext = now + SEEK_HOLD_DELAY_MS;
            else if (now >= leftHoldNext) { seekByPercent(-SEEK_STEP_PCT); leftHoldNext = now + SEEK_HOLD_REPEAT_MS; }
        } else leftHoldNext = 0;
    } else {
        rightHoldNext = 0; leftHoldNext = 0;
    }

    // ---- screenshot burst capture (hold KEY_SCREENSHOT) ----
    // Same hold-repeat pattern as the seek keys above, just on its own timer
    // (slower -- SD writes aren't free) and not restricted to Now Playing.
    {
        static unsigned long shotHoldNext = 0;
        const uint32_t SHOT_HOLD_DELAY_MS = 500, SHOT_HOLD_REPEAT_MS = 400;
        if (!screenIsOff && M5Cardputer.Keyboard.isKeyPressed(KEY_SCREENSHOT)) {
            unsigned long now = millis();
            if (shotHoldNext == 0) shotHoldNext = now + SHOT_HOLD_DELAY_MS;
            else if (now >= shotHoldNext) { saveScreenshot(); shotHoldNext = now + SHOT_HOLD_REPEAT_MS; }
        } else {
            shotHoldNext = 0;
        }
    }

    // ---- screen-off timeout ----
    uint32_t offTimeout = screenOffTimeoutMs[settingScreenOffIdx];
    if (!screenIsOff && offTimeout != 0 && millis() - lastInputTime >= offTimeout) {
        screenIsOff = true;
        M5Cardputer.Display.setBrightness(0);
    }

    // ---- lightweight periodic redraws (partial, so they don't disturb decode) ----
    // Skipped entirely while the screen is off -- nothing is visible, so there's
    // no point spending loop() time on SPI writes nobody can see.
    if (!screenIsOff) {
        marqueeTick();
        turntableTick();
        static unsigned long lastProgressDraw = 0;
        if (uiMode == MODE_NOWPLAYING && playState != STOPPED && millis() - lastProgressDraw >= 500) {
            lastProgressDraw = millis();
            drawProgressBar();
        }
        static unsigned long lastBatteryDraw = 0;
        if (uiMode != MODE_FULLVIS && millis() - lastBatteryDraw >= 20000) {
            lastBatteryDraw = millis();
            drawStatusIcons();
        }
        static unsigned long lastVisDraw = 0;
        if (uiMode == MODE_NOWPLAYING && playState == PLAYING && millis() - lastVisDraw >= 60) {
            lastVisDraw = millis();
            drawVisualizer();
        }
        static unsigned long lastFullVisDraw = 0;
        if (uiMode == MODE_FULLVIS && playState == PLAYING && millis() - lastFullVisDraw >= FULLVIS_INTERVAL_MS) {
            lastFullVisDraw = millis();
            drawFullVis();
        }
    }

    // Deferred while the screen is off -- the flags stay pending (not cleared)
    // so the moment it wakes, the redraw above forces a full, up-to-date repaint.
    if (needsRedraw && !screenIsOff) {
        if      (uiMode == MODE_BROWSER)    drawBrowser();
        else if (uiMode == MODE_NOWPLAYING) drawNowPlaying();
        else if (uiMode == MODE_FULLVIS)    drawFullVis();
        else                                drawSettings();
        needsRedraw = false;
    }
}