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
- [x] 1. `dist_of()`; use it in `find_victim_locked()`, the admission test in `pick_locked()`, and `insert_locked()`. Test intent: existing budget and scrub-stress cases unchanged; new reversal case.
- [x] 2. Reversal case in `test_player`. Verified to fail against the committed cache.c on the two residency assertions, and pass with the change.
- [x] 3. README paragraph.

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

### The symmetric window was the wrong reading
**What came up:** The first implementation scored frames by distance in
either direction, which settles into a window half behind and half ahead of
the playhead and stops sliding once full. The user's manual check showed
exactly that and it was not what was asked for: the region should keep running
ahead, with the leftmost frames going as the rightmost load, and only the
*order* of recycling the frames behind should change, farthest first.
**Options:** keep the symmetric window; score frames ahead by distance and
frames behind above all of them, farthest behind highest.
**Chose:** the latter. Loading is untouched; the resident region behaves as
before while playing; the one difference from the original code is that
frames behind are recycled farthest first instead of nearest first.
**Why:** it is what was asked for, it has no read-ahead cost, and the change is
one comparison. The plan's Goal and Approach above describe the first reading
and are superseded by this entry. The test was rewritten to check both halves:
the region runs ahead while playing, and after reversing the far end is what
gets recycled.

### Ties at equal distance (under the first reading)
**What came up:** Self-review of the symmetric window found a deadlock with a
budget of exactly two frames, the floor `cache_create()` raises smaller
budgets to: the frame just shown and the next frame ahead tied at distance
one, the tie went to the incumbent, and nothing ever woke the loaders again.
**Chose:** under the final rule there is no tie: any frame behind scores above
any frame ahead, so the frame just shown is always the one given up. The
minimum-budget test advances three times to cover it and was verified to
fail against the symmetric measure.

### Loads admitted with nowhere to land
**What came up:** Self-review noticed, on the admission line the change
touched, that when the cache is full and the playhead steps forward, one slot
is freed but every loader thread starts a load, since the admission test did
not count loads already in flight. All but one arrival was decoded and then
discarded; under `--readers` that is a wasted disk read per thread per step.
Pre-existing, but on the line being changed.
**Options:** leave it; count in-flight loads against free room and evictable
frames.
**Chose:** count them: a frame is admitted only if free slots plus resident
frames worth less than it exceed the loads in flight. `CacheStats` gains a
`discarded` count so the recycling test can assert it stays at zero through a
forward run.

### Split at the budget's reach, not at half the loop
**What came up:** The user's second manual check, with a 56-frame budget in
the 96-frame sequence, still showed seven frames kept behind the playhead.
The score split ahead from behind at half the loop, so the frame 49 ahead was
scored as behind and lost to the frames just shown; the region could never
extend past the midpoint.
**Chose:** a frame counts as ahead if it is within the budget's reach
(`cap_frames`, kept in step with the frame size estimate), else behind. The
region then runs the whole budget ahead at any budget, and the far end of the
loop is kept on reversal only when the loader would reach it anyway. A test
with a budget over half the sequence covers it.

### Reversal test raced the loaders
**What came up:** The first reversal test checked which old frames remained at
one instant after the refill began; the loaders keep recycling until the old
region is gone, so the check depended on timing.
**Chose:** test the invariant instead: sampled repeatedly during the refill,
the surviving old frames must always be a contiguous run from the nearest
one, which nearest-first eviction breaks on its first eviction.

## Changelog
- `src/cache.c`: eviction recycles frames behind the playhead before frames ahead, farthest behind first; loading order unchanged. Admission counts loads in flight.
- `src/cache.h`: `CacheStats` gains `discarded`.
