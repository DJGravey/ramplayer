# Faster feedback while scrubbing — rapid plan

**Mode:** rapid   **Date:** 2026-09-16
**Goal:** Dragging the playhead across frames that are not yet in RAM shows
nothing new until the drag stops. Two defects cause it. The viewport only
updates when the exact current frame is resident, and while dragging the
current frame keeps changing to frames that have not landed, so the last
image freezes under a LOADING badge. And the loaders' read-ahead runs in the
play direction, not the drag direction, so dragging left after playing
forwards fetches frames to the right of the cursor. A third, smaller cost: a
long jump waits for every thread to finish the frame it was decoding near the
old position before one is free for the frame under the cursor. After this
change the viewport always shows the resident frame nearest the playhead,
the read-ahead runs the way the drag is going, and a decode whose frame the
playhead has left far behind is abandoned so the thread can take a frame
that matters.

## Steps Summary
1. `app.c`: a drag sets the cache direction from the sign of the playhead move; the direction persists after release.
2. `app.c`/`app.h`/`ui.c`: when the current frame is not resident, show the nearest resident frame on either side as a stand-in, and name it on the badge.
3. `cache.c`/`cache.h`: per-worker abort; `cache_set_focus()` cancels any decode whose frame is now out of the loaders' reach in both directions; the entry returns to EMPTY, and `CacheStats` counts it.
4. `test_player`: a case for each behaviour.
5. README: the cache and timeline paragraphs describe the new behaviour.

## Approach
Nothing about the loaders' scheduling changes. Every mouse move already
re-points the cache at the frame under the cursor, and that frame is always
the first one a free thread takes; the others take the frames next to it,
which is read-ahead. The fix is to make that read-ahead point the right way:
`app_mouse_move()` sets `last_dir` from the sign of the move before
`goto_frame()`, so `cache_set_focus()` sees the drag direction.

The stand-in is the nearest resident frame, found by scanning outward from
the playhead over the lock-free state array, not wrapping (the drag is on a
linear timeline). `app_update_shown()` takes it when the current frame is not
resident, and the App keeps `shown_frame` as the frame actually on screen, so
the existing badge test `shown_frame != current` still means "not the real
frame". The badge reads `LOADING 1042, showing 1039` using sequence numbers.
Every landing already wakes the main loop, so the picture converges on the
cursor as nearer frames arrive. The scan is bounded by the sequence length
and costs one relaxed atomic load per step.

Cancelling: each worker owns an `_Atomic int` abort slot and records the
frame it is decoding; `reader_load()` polls that slot between chunks exactly
as it polls the quit flag today (the flag's values gained a soft/hard
distinction later; see Deviations). When the
focus moves, `cache_set_focus()` raises the slot of any worker whose frame is
at least the loaders' reach (`cap_frames + n_threads + 4`, the same bound
`pick_locked()` scans to) away in both directions. Measuring in both
directions rather than in the direction of play means a reversal over the
frames just ahead lets them finish and land, where they are admitted anyway,
instead of throwing them away; only a genuine jump cancels. A cancelled load
leaves its entry EMPTY, is not a failure, and is counted in `CacheStats.aborted`.
`cache_destroy()` raises every slot in place of the old single flag.

Alternatives considered, one line each:
- Disable read-ahead while dragging: every free thread but one would idle, and the frame under the cursor would always lag by a decode; read-ahead in the drag direction is what removes the lag.
- A display queue of landed frames: the cache state array already is that queue and "nearest resident" is the skip rule.
- A callback for the abort check in `reader.h`: changes the interface for every caller; a per-worker flag is the same poll with no interface change.
- Cancel on distance in the direction of play: throws away frames just behind a reversed drag that would have landed and been kept.

## Files
**Source:** `src/app.c`, `src/app.h`, `src/ui.c`, `src/cache.c`, `src/cache.h`, `src/reader.h`, `src/reader_exr.c` (the last two per the Deviation)
**Tests:** `tests/test_player.c`
**Docs:** `README.md`, this plan
**Dependencies added:** none.

## Steps
- [x] 1. Drag direction. Test: after a small budget fills ahead of a press, dragging leftwards frame by frame leaves the region below the cursor resident and the region above given up, and `last_dir` is -1 after release.
- [x] 2. Nearest-resident stand-in and badge. Test: with frames 0..k resident and the playhead dragged to a cold frame, the shown frame is resident and no resident frame is nearer; once the cold frame lands it is shown.
- [x] 3. Per-worker abort in the cache, raised from `cache_set_focus()`; `aborted` in `CacheStats`. Test: with one loader, jumping the focus to a far frame and straight back, repeated, cancels at least one decode; cancelled frames are EMPTY, not failed, and no error is reported.
- [x] 4. README.

## Tests
**Unit:** `test_player`: three new cases as above; existing cases unchanged.
`test_reader` and `test_draw` unchanged.
**Manual:** `build\windows\Release\ramplayer.exe --mem 200M test\seq_a`: drag
across the timeline into the grey region and the image should follow the
cursor with a short lag, the badge naming the stand-in frame; the green band
should grow in the drag direction; release, and the exact frame replaces the
stand-in. Drag back and forth over a loaded region and the image should
track the cursor without lag.

## Deviations

### Cancelling every stale decode starved the cache
**What came up:** The existing scrub stress case (random focus jumps as fast
as the loop runs) failed: nothing was ever displayed. Every decode was
cancelled before it finished, so no frame ever landed. A fast drag on a long
sequence with a small budget would do the same to a lesser degree, and would
also throw away decodes that were nearly done, halving the landing rate in
exactly the scenario this change is for.
**Options:** leave it and slow the stress test to a realistic rate; cancel
only in the first half of a decode (finishing is cheaper than redoing, and
the landed frame is useful as a stand-in); never cancel twice in a row on one
thread, so churn cannot starve the cache; both.
**Chose:** both. `reader.h` gains `READER_CANCEL_SOFT`, honoured by the decoder
only while less than half the chunks are done (and before the read in
whole-file mode, where nothing is done yet), and `READER_CANCEL_HARD` for
quitting. A worker whose last decode was cancelled lets the next one finish.
**Why:** the soft cancel puts the sunk-cost judgement where progress is
known, the decoder, with no timing heuristics; the once-in-a-row rule is a
hard guarantee that at least half of all decodes land whatever the input
does. The stress test passes unchanged.

### Self-review findings (code-review, medium)
Seven findings, all confirmed against the code and all fixed:
- A real decode failure that happened after a soft cancel the decoder chose to ignore was classed as a cancel and never reported. The worker now judges by the decoder's own report (`READER_ERR_CANCELLED` in `reader.h`) rather than by the flag.
- In whole-file mode (`--readers`) a soft cancel during the decode from memory threw away a read already paid for through the permit. The decoder ignores soft cancels once the file is in memory; the check before the read stays.
- The once-in-a-row exemption was a latch that survived idle periods, so a single loader could be pinned to a useless decode long after the churn that set it. It is cleared when the worker goes idle.
- The FAILED badge did not name the stand-in frame under it; it now does, like the LOADING badge.
- The new tests repeated one poll loop six times; `wait_all_ready()` replaces them.
- A duplicate `typedef struct Cache` in `cache.c`; removed.
- A dead branch in `reach_locked()`; both it and `stale_locked()` are one `RP_MIN` now.
