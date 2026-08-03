# Ember — M5Stack Cardputer ADV MP3 player firmware

Single-file firmware (`src/main.cpp`) for an ESP32-S3-based MP3 player named
**Ember**: SD-card folder-stack browsing (Artist → Album → tracks),
natural-sorted lists, auto-advance with album-looping, and a Now Playing screen
with embedded album art, a scrolling title marquee, battery/volume meters, a
progress scrubber, and an amplitude-based visualizer.

A startup splash screen with a fire theme (matching the name) is planned but
not yet built.

## Hardware constraints (do not "fix" these)

- **Board has no PSRAM.** All buffers (name pools, queue pool, art scan buffer,
  cover sprite) are sized to fit in the ~320KB internal SRAM. Don't reach for
  PSRAM-backed allocations.
- **M5Canvas defaults to PSRAM.** `M5Canvas(parent)` sets `_psram = true` in its
  constructor. On this board that makes `createSprite()` silently fail (returns
  nullptr) since there's no PSRAM to allocate from. Always call
  `setPsram(false)` before `createSprite()` on any `M5Canvas`/`LGFX_Sprite`.
- **ES8311 codec needs `cfg.external_speaker.hat_spk = true`** set *before*
  `M5Cardputer.begin(cfg, true)`. Without it, the speaker doesn't initialize.
  Leave the existing speaker DMA config alone too
  (`dma_buf_count=8`, `dma_buf_len=256`, speaker task pinned to `APP_CPU_NUM`,
  `task_priority=3`) — these were tuned to avoid audio glitches/underruns.

## Audio is single-threaded — on purpose

The MP3 decoder is pumped by calling `mp3->loop()` directly from the main
`loop()`, on the same core/task as SD card access and keyboard input. This was
a deliberate choice after a previous attempt at a second FreeRTOS task (or a
mutex guarding audio/SD) **deadlocked / froze on folder navigation**. Do not
reintroduce a second task or a mutex for audio or SD access. If something needs
to happen "in the background" (e.g. a future visualizer), it has to be cheap
enough to run inline in `loop()` without stalling decode.

This is also why album art is decoded on the same core as audio: expect a
brief hitch on the *first* track of a new album (art decode blocks `loop()`
momentarily). That's expected and accepted — it's why art is cached per
album-folder (see `artCachedFolder`/`artLoaded` in `playQueuePos`) and never
re-decoded on auto-advance or track skip within the same folder.

## Audio library: ESP8266Audio, pinned to 1.9.7

`platformio.ini` pins
`https://github.com/earlephilhower/ESP8266Audio.git#1.9.7`. The 2.x line
requires `i2s_std.h` and breaks this build — don't bump the version without
re-validating the whole audio path.

- ID3 tags (artist/title/album) come from `AudioFileSourceID3`'s metadata
  callback: `id3->RegisterMetadataCB(fn, data)`, called with
  `(void*, const char* type, bool isUnicode, const char* str)`. `type` is one
  of `"Title"`, `"Performer"` (artist), `"Album"`, `"Year"`, `"track"`, `"Set"`,
  `"Compilation"`.
- The callback API has **no length parameter** — for `isUnicode` (UTF-16)
  frames, an embedded 0x00 code unit truncates the string early (this hits
  Latin text hardest since every other byte is 0x00; CJK code units are rarely
  zero, so those come through better). This is a library limitation, not a bug
  in the calling code — see `copyMeta()` in `main.cpp`.
- Output is triple-buffered stereo via the custom `AudioOutputM5Speaker` class
  feeding `M5Cardputer.Speaker.playRaw`. Don't change this output path.

### Multi-format playback (MP3 / FLAC / WAV)

`decoder` is a base `AudioGenerator*`, not `AudioGeneratorMP3*` — `begin()`/
`loop()`/`isRunning()`/`stop()` are virtual and identical across all three
generators, so `playQueuePos()` just `new`s the right concrete class based on
`formatForName()` (extension only, no content sniffing: `.mp3`, `.flac`,
`.wav`/`.wave`). `curFormat` exists only for the few spots that actually need
to know which concrete type is loaded.

- `AudioFileSourceID3` wraps `file` for **all** formats, not just MP3 — it's a
  transparent passthrough when the first 10 bytes aren't an ID3 header, so
  FLAC/WAV files play fine through it and pick up tags for free in the (rare)
  case a tagger prepended an ID3v2 block. Neither ESP8266Audio's
  `AudioGeneratorFLAC` (its `metadata_cb` is a stub that only logs) nor WAV's
  container format expose their *native* tag schemes (Vorbis comments /
  `LIST INFO` chunks) through this library, so FLAC/WAV tracks normally fall
  back to filename display — same fallback path already used for untagged
  MP3s.
- **Seeking is MP3-only.** `seekBy()`/`seekByPercent()` no-op unless
  `curFormat == FMT_MP3`. MP3 seeking works by seeking the raw
  `AudioFileSourceSD` and calling `AudioGeneratorMP3::desync()` to drop
  libmad's stream-sync state so it resyncs on the next valid frame header —
  FLAC has no equivalent public resync hook (its frame sync is internal to
  the libFLAC stream decoder), and WAV's internal read buffer/byte-countdown
  (`buffPtr`/`buffLen`/`availBytes`) would desync permanently with no way to
  fix it from outside the class. Don't try to bolt on seeking for these
  without a real plan for that state.

## Now Playing / album art

Album art uses M5GFX's built-in decoders (`drawJpg`/`drawPng`/`drawBmp`/`drawQoi`)
reading directly from an open SD `File` — no separate JPEG library, no
pre-baked art files. The flow (`findImageStart` → `getImageSize` →
`loadAlbumArt` in `main.cpp`):

1. Scan the first ~16KB of the MP3 file for an embedded image signature
   (JPEG `FFD8`, PNG `89504E47...`, BMP `BM`, GIF `GIF8[79]a`, QOI `qoif`).
   GIF is detected but not drawn — M5GFX has no `drawGif` in this version, so
   it falls back to the placeholder.
2. Read width/height from the format header (JPEG requires walking marker
   segments to find SOF0/SOF2, tolerating long EXIF/APPn blocks).
3. Compute a uniform scale = min(coverW/imgW, coverH/imgH), clamped to ≤1.0
   (never upscale); pass 0 (fit-to-box) if dimensions couldn't be read.
4. Decode into a cached `M5Canvas` (`coverSprite`), reused across the whole
   album — see the single-threaded/caching note above for why.

`drawArtRegion()` was originally left as a seam for a future visualizer over
the album art; instead, the visualizer ended up as its own bottom-right widget
(see below), so that seam is currently unused but still there if wanted.

## UI redraw performance (flicker + audio hiccups)

Direct-to-display draw sequences (`fillRect` to blank an area, then `print`/
`drawFastHLine`/`fillCircle` on top) showed a visible flash on real hardware,
because the LCD briefly displays the blanked-out state between calls. Anything
that redraws on a timer (title marquee, progress dot, visualizer bars) is
composited into a small off-screen `M5Canvas` first and blitted with a single
`pushSprite()` instead — see `titleSprite`, `progSprite`, `visSprite`. Each has
a direct-draw fallback for if the sprite alloc fails, which may still flicker.

Every keypress-triggered redraw also competes with `mp3->loop()` for `loop()`
time -- a big redraw (e.g. the full browser list) can starve the decoder long
enough to cause an audible glitch, not just a visual one. `moveCursor()` avoids
this by redrawing only the two rows that actually changed (`drawBrowserRow()`)
instead of the whole list, and skips the header/battery/volume icons entirely
since cursor movement doesn't affect them. Entering/leaving a folder still does
a full repaint (and a fresh SD directory read via `loadDir()`), so some hitch
there is expected -- the fix targets the far more frequent up/down case.

## Settings ('s' key)

`MODE_SETTINGS` is a small, growing list (`SETTINGS_COUNT`) toggled from any
other mode; `uiModeBeforeSettings` remembers where to return to on back/`s`.
Runtime-only, no NVS persistence yet. Current settings:

- **Backlight** — cycles `backlightValues[]` via `Display.setBrightness()`.
- **Screen off** — inactivity timeout (`lastInputTime`, reset on every
  keypress). When it elapses, brightness is set to 0 and `screenIsOff = true`;
  all periodic redraws and the `needsRedraw`/`needsStatus` dispatch are skipped
  entirely while off (state changes still happen, just aren't painted) so a
  quiet screen doesn't cost `loop()` time it can't spend on audio. The *next*
  keypress after that only wakes the screen (restores brightness, forces one
  full repaint) and is otherwise swallowed, not also acted on.
- **Album end** (`albumEndMode`) — only changes behavior at the *end of the
  last track in an album* (natural playback end via `mp3->loop()` returning
  false with no tracks left in the queue); manual `n`/`p` skipping past the
  end still always wraps. `ALBUM_NEXT` looks up the next sibling folder via
  `findNextAlbumFolder()`, which reads the parent directory into its own small
  static buffers (`albumListPool`/`albumListOffset`) rather than touching the
  browser's `namePool`/`entryCount` — otherwise it would silently change what
  the browser is showing while the user is off looking at something else.
