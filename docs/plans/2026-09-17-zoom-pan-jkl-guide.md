# Zoom, pan, JKL transport and a frame guide — rapid plan

**Mode:** rapid   **Date:** 2026-09-17
**Goal:** The viewport gains a view transform: the mouse wheel zooms about the
cursor, the left button drags the frame, Backspace returns to fit-to-window
and `0` shows the frame at 100%, centred, with black borders or cropped as the
window allows. The keyboard gains editor-style transport: `J` plays backwards,
`K` pauses, `L` plays forwards, Space toggles whichever direction last played,
`I`/`O` pause and step one frame back/forwards, and with Shift five frames.
`F` toggles a 1920x1080 guide rectangle, two pixels thick, centred on the
footage and scaling with it. On Windows the executable no longer opens a
console window; when started from a terminal its messages still reach that
terminal.

## Steps Summary
1. `CMakeLists.txt`/`platform.c`/`main.c`: Windows subsystem build, attaching to the parent console for `--help` and log output.
2. `view.c`/`view.h` (new): the view transform — fit, 1:1, zoom about a point, pan, and the destination rect — as pure functions.
3. `app.c`/`app.h`/`main.c`: wheel and viewport drag drive the view; Backspace and `0`; the status bar shows the zoom.
4. `app.c`/`app.h`/`main.c`: `J`/`K`/`L`, Space resumes the last play direction, `I`/`O` and Shift+`I`/`O`.
5. `draw.c`/`draw.h`/`ui.c`: `F` toggles the 1920x1080 guide, drawn as a clipped two-pixel frame.
6. Help overlay, `usage()` and README: the new keys; the "Zoom and pan" entry leaves Not yet.
7. Tests: `test_player` cases for the view maths, the mouse, and every new key; `test_draw` for the frame primitive.

## Approach
**View.** A new module `view.c` holds a `View` of four fields: `fit` (1 until
the user zooms or drags), `zoom` (screen pixels per source pixel), and `cx`,
`cy`, the source-image point that sits at the centre of the viewport. Keeping
the centre point rather than an offset makes a resize keep the same picture
centred, and makes zoom about the cursor two lines: find the source point
under the cursor, change the zoom, and move `cx`,`cy` so that point is back
under the cursor. `view_rect()` returns the destination rect that `ui_draw()`
hands to `draw_image()`; in fit mode it returns `rect_fit()` exactly as today,
so nothing about the fitted picture changes until the user acts. Leaving fit
mode derives `zoom` from the fitted rect and centres on the image middle, so
the first wheel notch or drag starts from what is on screen. `0` sets
`zoom = 1` and centres, which is `rect_center()` as today under `1`.

The wheel changes the zoom by a factor of 1.25 per notch, clamped to 1/64
to 64, and only when the cursor is over the viewport; over the bars the wheel
does nothing. Panning moves `cx`,`cy` by the mouse delta divided by the zoom.
Neither clamps the position: the frame may be dragged anywhere, and
Backspace brings it back. `app.c` keeps
the last mouse position for the drag; `app_mouse_down()` starts a pan when
the press is in the viewport and on nothing else, and a pan never pauses
playback. The status bar shows `FIT` or the zoom as a percentage next to the
resolution.

**Transport.** `App` gains `resume_dir`, the direction of the last play
command (a key or a button), independent of `last_dir`, which drags and
steps also set and which only drives the cache. Space pauses when playing and
otherwise plays in `resume_dir`. `J` and `L` are `app_play(dir)`: no-ops when
already playing that way, else the existing `app_toggle_play()` switch. `K`
is `app_pause()`. `I`/`O` are `app_step(∓1)`, which already pauses first and
wraps at the ends like every other step; Shift makes it five. `main.c` reads
Shift from the key event's modifier mask and maps to separate `AppKey`
values, so `app.c` stays free of modifier state. `I`/`O` auto-repeat while
held, like the arrow keys.

**Guide.** `ui_draw()` derives the guide from the destination rect: the
scale is `dst.w / iw`, and the guide is 1920x1080 source pixels centred on the
image, drawn two pixels thick (times the UI scale) straddling its edge: one
pixel inside the boundary and one outside. A new `draw_frame()` draws the four bands clipped to the viewport, so a frame
panned down cannot paint over the status bar.

**Console.** `WIN32_EXECUTABLE` on the `ramplayer` target switches the link
to the Windows subsystem; SDL2main is already linked and supplies the
`WinMain` that calls `main()` with UTF-8 arguments, exactly as its console
`main` does today. A Windows-subsystem process has no console, so `--help`,
`rp_log()` and the startup summary would vanish; a new
`rp_console_attach()` in `platform.c` calls `AttachConsole()` on the parent
and reopens stdout and stderr onto it, a no-op when there is no parent
console (double-click from Explorer) and on POSIX. The shell prompt returns
before the output, as it does for any windowed program.

Alternatives considered, one line each:
- Keep the view maths in `app.c`: the module is pure and testable without a cache; `app.c` stays about input and playback.
- Pan by a screen-space offset rather than a source-space centre: a resize would then drift the picture; the centre form makes zoom-about-cursor and resize both trivial.
- Clamp the pan so the viewport centre stays over the image: not wanted; fit-to-window is the reset.
- Wheel everywhere, anchored to the clamped cursor: the wheel over the timeline is better left free for a future scrub.
- Snap the zoom to 100% when a wheel step crosses it: `0` already goes there directly.
- Guide drawn wholly inside or wholly outside the boundary: inside hides two rows of the footage, outside is invisible for a 1920x1080 frame in fit mode; straddling shows one pixel of each.
- A `/SUBSYSTEM:WINDOWS` with `FreeConsole()` tricks or a launcher stub: `WIN32_EXECUTABLE` plus SDL2main is the supported path and changes nothing about arguments.

Retired bindings: `f` now toggles the guide and Backspace takes its old job;
`1` is retired since `0` replaces it; `b` is retired since `J` replaces it.

## Files
**Source:** `src/view.c`, `src/view.h` (new), `src/app.c`, `src/app.h`,
`src/ui.c`, `src/draw.c`, `src/draw.h`, `src/main.c`, `src/platform.c`,
`src/platform.h`
**CMake:** `CMakeLists.txt` (`view.c` in `ramplayer` and `test_player`;
`WIN32_EXECUTABLE` on `ramplayer`)
**Tests:** `tests/test_player.c`, `tests/test_draw.c`
**Docs:** `README.md`, this plan
**Dependencies added:** none.

## Steps
- [ ] 1. Windows subsystem and console attach. Test: manual (below); `rp_console_attach()` is declared for both backends and compiles as a no-op on POSIX.
- [ ] 2. `view.c`: `view_init()`, `view_fit()`, `view_one_to_one()`, `view_zoom_at()`, `view_pan()`, `view_rect()`, `view_scale()`. Test: fit returns `rect_fit()`; 1:1 returns `rect_center()`; zooming about a point keeps the same source pixel under it and leaves fit mode; panning moves the rect by the delta and is not clamped; zoom clamps at both limits; fit after a zoom restores the fitted rect; a resize in free mode keeps the centre point.
- [ ] 3. Wheel and drag in `app.c`, `app_wheel()` in `app.h`, `SDL_MOUSEWHEEL` and Backspace/`0` in `main.c`, zoom in the status bar. Test: a wheel notch over the viewport zooms in about the cursor and leaves fit mode; a notch over the transport does nothing; a viewport drag pans by the delta, does not pause, and does not scrub; release ends the pan; Backspace restores fit; `0` gives 1:1.
- [ ] 4. `resume_dir`, `app_play()`, `KEY_PLAY_FWD`/`KEY_PLAY_REV`/`KEY_PAUSE`/`KEY_STEP5_BACK`/`KEY_STEP5_FWD`, Shift in `translate_key()`, repeat for `I`/`O`. Test: `L` plays forwards and again is a no-op; `J` plays backwards; `K` pauses and a second `K` stays paused; after `J`,`K`, a rightward drag, Space plays backwards; after `L`,`K`, Space plays forwards; on a fresh player Space plays forwards; `I`/`O` pause and step ±1; Shift steps ±5 and wraps.
- [ ] 5. `draw_frame()` and the guide. Test (`test_draw`): a frame of thickness 2 writes exactly the four bands, one pixel either side of the boundary, leaves the interior untouched, and stays inside its clip. Test (`test_player`): `F` toggles `show_guide`.
- [ ] 6. Help overlay, `usage()`, README controls table and Not yet.

## Tests
**Unit:** `test_player`: the view cases, the mouse cases and the key cases
above; existing cases unchanged. `test_draw`: `draw_frame()`. `test_reader`
unchanged.
**Manual:** `build\windows\Release\ramplayer.exe test\seq_a` from Explorer
or a shortcut opens only the player window, no console. From PowerShell,
`build\windows\Release\ramplayer.exe --help` prints the usage into that
terminal (after the prompt), and the normal launch prints its startup line.
In the player: wheel over the image zooms about the cursor and the pixel
under the cursor stays put; drag moves the frame in every direction; Backspace refits; `0` shows 100% with
black borders on a large window and cropped on a small one; `F` shows a
two-pixel rectangle centred on the footage, one pixel either side of the
1920x1080 edge, that scales and moves with it;
Space after `J` then `K` plays backwards; `I`/`O` step, Shift+`I`/`O` step
five, and holding them repeats; the status bar shows `FIT` or the zoom.

## Deviations
