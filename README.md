# ramplayer

A RAM player for image sequences: it loads frames into memory up to a fixed
budget and lets you scrub through them at speed. EXR sequences are the current
focus; video is the next step.

Written in C11. All drawing is done on the CPU into a system-memory buffer —
there is no GPU code. SDL2 is used only to own a window and hand that one
finished buffer to the display.

## Building

Needs a C compiler, CMake, SDL2 and OpenEXR 3.x (we use its pure-C
`OpenEXRCore` API, so no C++ is involved).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Windows

Builds with Visual Studio 2022 or later (MSVC 19.35+ for C11 atomics) and
vcpkg, which fetches SDL2 and OpenEXR from the manifest. From a Developer
PowerShell with `VCPKG_ROOT` set (Visual Studio's bundled copy lives at
`<VS>\VC\vcpkg`):

```powershell
cmake --preset windows
cmake --build --preset windows-release
build\windows\Release\ramplayer.exe shot.0042.exr
```

The `windows-fixtures` preset also builds the OpenEXR command line tools so
`tools/make_test_data.sh` can run from Git Bash; see Tests below.

Paths are UTF-8 throughout, so non-ASCII names work. Drive-relative paths
without a separator (`C:shot.0001.exr`) are not supported: give the directory.

## Running

```sh
ramplayer shot.0042.exr        # one frame expands to the whole sequence
ramplayer 'shot.%04d.exr'      # printf pattern
ramplayer 'shot.####.exr'      # hash pattern
ramplayer /renders/shot/       # the largest sequence in a directory
ramplayer a.exr b.exr c.exr    # an explicit list, ordered by frame number
```

Options:

| Option | Meaning |
| --- | --- |
| `--mem SIZE` | RAM budget for cached frames; default `1G`. Accepts `512M`, `4G`, … A budget too small for two frames is raised to that, since playback could not otherwise advance. |
| `--fps RATE` | Playback rate. Defaults to the sequence's own `framesPerSecond`, else 30. |
| `--threads N` | Loader threads; default is one per core, less one. |
| `--readers N` | Read each frame file whole, at most `N` files at a time, and decode from memory. By default the decoder reads the file itself, chunk by chunk, with every loader thread independent. See Spinning disks below. |
| `--scale N` | UI scale factor for HiDPI displays. |

Controls:

| | |
| --- | --- |
| Click / drag on the timeline | pause and scrub; keeps tracking while the button is held |
| Transport buttons | laid out left to right as previous frame, play backwards, play forwards, next frame |
| Mouse wheel over the picture | zoom in or out about the cursor |
| Drag on the picture, left or middle button | pan; playback carries on underneath |
| `space` | pause, or play again in whichever direction last played |
| `j` `k` `l` | play backwards / pause / play forwards |
| `i` `o` | step one frame back / forwards (pauses first); with `Shift`, five frames |
| `←` `→` | step one frame (pauses first) |
| `↑` `↓`, `PgUp` `PgDn` | jump ten frames |
| `Home` `End` | first / last frame |
| `Backspace` | fit to window |
| `0` | actual size, centred: black borders when the window is larger than the frame, cropped when it is smaller |
| `f` | toggle a 1920x1080 frame guide, centred on the footage and drawn at its scale |
| `s` | toggle smooth scaling |
| `h` | key list |
| `q`, `Esc` | quit |

Playback loops at both ends: running forwards past the last frame returns to
the first, and running backwards past the first returns to the last.

### Spinning disks

The default loader lets the decoder read each file chunk by chunk, so several
threads interleave small reads across several files. An SSD or a fast network
share does not mind; measured on a 10 Gb SMB share with 12 MB frames it
saturates the link at about 90 frames/s. A spinning disk does mind: the same
frames from a SATA drive loaded at 6 frames/s, a third of the drive's
sequential rate, because the head seeks between the loaders' files.

`--readers 1` reads each file whole in one pass, one file at a time, and
decodes from memory in parallel, which on that drive measured 13 to 14
frames/s, about the drive's ceiling. Two readers measured worse than one. On
the share, `--readers 8` or more comes within a tenth of the default, so the
option costs little if a sequence sometimes comes from either. Each loader
thread then keeps a buffer the size of one frame file, on top of the `--mem`
budget.

No spinning disk delivers 30 fps of 12 MB frames (360 MB/s), so on a first
pass playback will hold at the loading rate. Playback at full rate comes from
resident frames: give `--mem` a budget that holds the whole sequence, and the
second pass is free.

## How it works

**The timeline** maps its leftmost pixel to the first frame and its rightmost
to the last. `ui_frame_at_x()` and `ui_x_for_frame()` are exact inverses at
both ends, so the playhead always lands back under the cursor that placed it.
The band behind the playhead shows what is actually resident: green for frames
in RAM, amber for frames being loaded, red for frames that failed. When the
sequence has more frames than the timeline has pixels, each column mixes the
residency of the frames beneath it rather than picking one.

Scrubbing into frames that are not in RAM yet does not freeze the picture: the
viewport shows the resident frame nearest the playhead, the badge names it,
and each frame that lands nearer replaces it until the real one arrives. The
direction of a drag becomes the loaders' direction, so the read-ahead runs
towards the frames the cursor is heading for, and during a steady drag they
are usually there by the time it reaches them.

**The cache** (`src/cache.c`) keeps the frames nearest the playhead in memory
within the byte budget. Rather than filling a work queue that goes stale the
moment the user scrubs somewhere else, each loader thread asks the cache which
frame is currently worth fetching most, so a jump across the timeline redirects
every thread at once. "Worth most" is the distance from the playhead measured
in the direction of play, wrapping at the ends because playback loops — which
is why the cached region sits ahead of the playhead when playing forwards and
behind it when playing backwards.

What to give up is judged the other way round. Any frame behind the playhead
is recycled before any frame ahead, and among the frames behind, the farthest
goes first, so the frames just shown are the last to go. Reversing, or
scrubbing back over what was just played, therefore finds those frames still
in RAM while the loaders turn around, and the space for the new direction
comes from the far end of the old one.

A frame is only fetched if there is room for it, or if something resident is
worth less than it, beyond what the loads already in flight will take when
they land. That admission rule is what keeps a full cache from thrashing and
stops one freed slot from starting a load on every thread. The same comparison
is applied again when a decoded frame is inserted, so a frame that became less
useful while it was decoding is dropped rather than evicting something better.
A decode still in its first half when the playhead jumps out of its reach in
both directions is abandoned, so a jump across the timeline frees the threads
for the frames around the new position instead of waiting for the old ones to
finish. One past half way finishes, since that is cheaper than decoding it
again and the frame is useful when it lands; with `--readers` a frame already
read into memory always finishes, since the read was the expensive part. No
thread abandons two decodes in a row, so a playhead that never stops moving
cannot starve the cache.

Frames are reference counted. The cache holds one reference and the drawing
code takes another while it paints, so a frame evicted mid-draw stays alive
until the pixels have been read.

**Decoding** (`src/reader_exr.c`) asks OpenEXR for half floats no matter how
the file stores them, which turns the scene-linear to display conversion into a
single lookup per channel against a 64 KB table instead of a `pow()` per pixel.
Frames are decoded chunk by chunk into a small scratch buffer and converted
into the destination image immediately, so a loader thread's working set stays
in the tens of kilobytes rather than holding a whole float image. With
`--readers` the compressed file is held whole while it decodes, which is still
far smaller than the float image would be.

Frames are cached ready to display, as 8-bit pixels. That costs half what
keeping half floats would, so the budget holds roughly twice as many frames,
and scrubbing does no per-frame work beyond scaling to the window.

Scanline and tiled files are both handled, along with mipmapped files (level 0),
data windows that differ from the display window, and single-channel luminance
images.

**Playback** advances on a clock, but only onto frames that are already
resident. If the next frame has not arrived, the playhead and the clock both
hold rather than racing ahead through frames nobody would see; once caching
catches up, playback resumes at true speed.

**The view** (`src/view.c`) is either fitted to the window or free: a zoom
and the source-image point that sits at the centre of the viewport. Keeping
the centre point rather than an offset means a resize keeps the same picture
centred, and zooming about the cursor is a matter of moving that point so
the pixel under the cursor stays where it is. A pan is not clamped; the frame
can be dragged anywhere, and `Backspace` brings it back.

**Drawing** composites the whole window — image, timeline, buttons, text — into
one buffer, which is then handed over in a single upload. Scaling the frame
into the viewport is the only part with a real cost, so it is done in two
passes: blending the two source rows into a scratch line first leaves one
interpolation per output pixel instead of three, and makes the vertical pass a
straight walk the compiler can vectorise. On a 2020-era laptop core that is
about 10 ms to put a 2K frame into a 1080p window and 16 ms into a 1440p one,
against a 41.7 ms budget at 24 fps. `s` switches to nearest-neighbour, which
costs about 2 ms and shows the pixels as they are.

## Layout

```
src/
  main.c          SDL window, event loop, command line
  app.c/.h        player state, playback clock, input handling
  ui.c/.h         layout, hit testing, widget drawing
  view.c/.h       the frame in the viewport: fitted, or zoomed and panned
  cache.c/.h      RAM frame store and loader threads
  reader_exr.c    EXR decoding (OpenEXRCore)
  sequence.c/.h   turning a path, pattern or directory into a frame list
  draw.c/.h       CPU rasteriser: rects, text, triangles, scaled image blit
  font.c/.h       built-in 5x7 bitmap font
  color.c/.h      linear to display transform, as a lookup table
  image.c/.h      reference-counted frame images
  util.c/.h       timing, allocation, byte formatting
tests/
  test_player.c   timeline mapping, transport, looping, memory budget
  test_reader.c   EXR decoding and sequence discovery
  test_draw.c     the CPU rasteriser
tools/
  mkexr.c         writes the awkward EXRs the tests need
  make_test_data.sh
```

`app.c` and `ui.c` know nothing about SDL: `main.c` translates events into the
calls in `app.h`, which is what lets the tests drive the player without a
window.

On Windows `ramplayer.exe` is a windowed program, so no console opens beside
the player. Started from a terminal it attaches to that terminal for its usage
and log lines. A shell does not wait for a windowed program, so it would show
its prompt before those lines; once startup printing is done the player posts
an Enter to the terminal so the shell draws a fresh prompt beneath them.

## Tests

```sh
tools/make_test_data.sh                 # generates fixtures under test/
cmake --build build --target test_player test_reader test_draw
./build/test_draw
./build/test_reader
./build/test_player test/seq_a
```

On Windows, configure with `--preset windows-fixtures`, build the `mkexr`
target as well, and run the script from Git Bash in the project root; it finds
`mkexr` and `exrmaketiled` in the build tree and uses Arial for the burnt-in
frame numbers. The test executables land in `build\windows\Release\` and are
run from the project root the same way.

`test_player` drives the same entry points the event loop calls, so the
timeline mapping, transport buttons, keys, zoom and pan, looping and the
memory budget are tested the way a user drives them. `test_reader` checks decoding against overscan,
cropped, tiled, mipmapped, luminance, float and damaged files, and checks that
a tiled file decodes to exactly the same pixels as the scanline original.

`test_draw` covers the rasteriser, including the invariant that scaling a flat
colour by any factor must return exactly that colour — the check that catches
rounding and channel-packing mistakes a gradient would hide.

All three suites run clean under ThreadSanitizer, AddressSanitizer, UBSan and
LeakSanitizer.

## Not yet

- **Video.** `reader_exr.c` sits behind the small interface in `reader.h`;
  video belongs alongside it as a second implementation, dispatched on file
  extension, with FFmpeg decoding to the same display-ready `Image`. Seeking
  makes video caching different in character: frames come in decode order from
  the nearest keyframe rather than individually, so the loader would fetch runs
  of frames rather than one at a time.
- **Exposure and view transforms.** The display transform is a fixed
  linear-to-sRGB table built in `color_lut_init()`. Adding exposure means
  rebuilding that table, which is cheap — but frames are cached already
  converted, so the cache has to be refilled when it changes. Caching half
  floats instead would make it instant at the cost of holding half as many
  frames.
- **Audio**, which would also mean playback timing driven by the audio clock.

## License

MIT. See [LICENSE](LICENSE).
