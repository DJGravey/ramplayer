# Evict the farthest frame on either side — rapid plan

**Mode:** rapid   **Date:** 2026-09-16
**Goal:** Keep recently shown frames resident so that reversing play direction,
or scrubbing back over a region, finds them still in RAM. Today a frame's value
is its distance from the playhead in the direction of play, wrapping at the
ends, so the frame just behind the playhead counts as the farthest of all and
is the first evicted. Observed by the user: play forwards, then backwards, and
the green nearest the playhead on the right disappears first.

## Steps Summary
1. In `cache.c`, a second distance measure: how far a frame is from the playhead in either direction, wrapping. Eviction and the admission comparison use it; the loader's scan order stays ahead-only, so no loader effort goes to frames behind the playhead.
2. A `test_player` case: fill a small budget playing forwards, then reverse, and check the frames just shown are still resident without waiting, while the frame farthest away is gone.
3. README: the cache paragraph describes the new rule.

## Approach
Two measures, one job each. `prio_of()` (distance ahead in the direction of
play) still orders the loader's scan, so loading keeps filling ahead. A new
`dist_of()` (the smaller of the distance ahead and the distance behind, both
wrapping) chooses the eviction victim and is what a candidate's own distance is
compared against for admission. Frames behind the playhead are never loaded on
purpose; they are kept because they are already there, until they are farther
away than the next frame ahead that needs their space.

Steady state while playing is a window about half behind and half ahead of
the playhead. Reversing finds the behind half resident and the loaders turn
around to fill the other side. The read-ahead is therefore about half the
budget instead of all of it, which matters only when the budget cannot hold
the sequence and the storage is slower than playback and the user never
reverses; the average rate is unchanged in that case since playback is paced
by loading either way.

Alternatives considered, one line each:
- A biased ratio (behind counts as three times as far): needs a knob, and spends loader effort behind the playhead; the pure measure spends none.
- A fully symmetric window including loading: wastes half the loader's effort on frames already seen.
- Leave it: the current rule is optimal only for looping in one direction with a budget smaller than the sequence.

## Files
**Source:** `src/cache.c`
**Tests:** `tests/test_player.c`
**Docs:** `README.md`, this plan
**Dependencies added:** none.

## Steps
- [ ] 1. `dist_of()`; use it in `find_victim_locked()`, the admission test in `pick_locked()`, and `insert_locked()`. Test intent: existing budget and scrub-stress cases unchanged; new reversal case.
- [ ] 2. Reversal case in `test_player`.
- [ ] 3. README paragraph.

## Tests
**Unit:** `test_player`: existing memory budget, minimum budget and scrub
stress cases, plus the new reversal case. `test_reader` and `test_draw`
unchanged.
**Manual:** `build\windows\Release\ramplayer.exe --mem 200M test\seq_a` (opens a
window; the budget holds about 54 of the 96 frames): play forwards past the
middle, and the green should trail behind the playhead as well as lead it;
press `b`, and playback should reverse without a pause while the band refills
on the left.

## Deviations
(empty until implementation)

## Changelog
- `src/cache.c`: eviction takes the resident frame farthest from the playhead in either direction; loading order unchanged.
