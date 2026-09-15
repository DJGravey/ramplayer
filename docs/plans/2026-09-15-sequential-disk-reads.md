# Sequential disk reads — rapid plan

**Mode:** rapid   **Date:** 2026-09-15
**Goal:** Load frames from a spinning disk at the drive's sequential rate instead
of about half of it. Measured on `D:\test\C01` (3500 frames, 2304x1296 RGBA half,
zip, 12 MB each, Seagate ST4000DM004 over SATA) with frames that were not in the
Windows file cache: one loader thread decodes 15.9 frames/s, the drive's
sequential ceiling; the app's default of 15 threads gets 8.0, and 2 to 8 threads
get 5 to 7. Raw whole-file reads on the same drive hold 13 to 20 frames/s at any
thread count. The cause is the read pattern, not decoding, which runs at 26
frames/s per thread from memory and 261 on 15 threads.

Root cause: `reader_load()` lets OpenEXRCore read the file itself, which issues
one positioned read per chunk, about 81 per frame here. Several loader threads
interleave those small reads across different files, and a spinning disk turns
that into a seek per read.

What this cannot do: 30 fps of 12 MB frames needs 360 MB/s, more than any
spinning drive delivers. The fix roughly doubles the rate this drive loads at,
to about its sequential limit near 16 frames/s. Playback at full rate comes from
frames already resident, so a budget that holds the whole sequence (`--mem 42G`
for this one; the machine has 64 GB) makes the second pass free.

The user's usual source is a 10 Gb SMB share. Measured there with nothing in
the local cache, frames/s at 1, 4, 8, 15 threads: whole-file reads 47.7, 79.6,
80.4, 85.6; the current chunked path 20.7, 66.3, 86.2, 91.2. Both saturate the
link, about 1 GB/s, once enough reads are in flight, and one whole-file read at
a time would halve the share's rate. So the knob is the number of concurrent
readers, not a mode switch: `--readers N` reads whole files with at most N in
flight at once (1 for a spinning disk; several for a share or SSD). Without it
the current path is used, unchanged, because it is the measured best on the
share and stays the default until the whole-file path is measured there too.

## Steps Summary
1. Add whole-file reading to `platform.h`/`platform.c` (open, size, read, close; wide paths on Windows) since `fopen` cannot take UTF-8 paths there.
2. In `reader_exr.c`, a process-wide reader count set by `reader_set_readers(n)`. With n > 0, `reader_load()` takes one of n permits, reads the whole file into a buffer owned by `DecodeScratch`, releases the permit, and decodes from memory through OpenEXRCore's `read_fn`/`size_fn`. With n = 0 (default) the current code runs untouched.
3. `main.c`: `--readers N`, default 0; usage text.
4. Tests: `test_reader` runs its decode, damaged-file and tiled cases with readers 0, 1 and 2, plus a whole-file case that one scratch decodes a small file, then a much larger one, then the small one again (buffer growth and reuse).
5. Re-measure with the benchmark: `D:\test\C01` at 15 threads with readers 0 and 1 (expect 8 and about 16 frames/s); the share at 15 threads with readers 0, 8 and 15 (expect all near 90).
6. README: the option, when to use it, and choosing `--mem`.

## Approach
Whole-file mode is one read per frame with at most N frames being read at
once. The permits are a counting semaphore built from the existing `RpMutex`
and `RpCond` in `reader_exr.c`, held only around the read; decoding runs in
parallel outside it. The buffer lives in `DecodeScratch`, one per loader
thread, grown to the largest file seen and kept, so steady state does no
allocation. OpenEXRCore is given the path only for its error messages;
`read_fn` copies from the buffer with pread semantics and returns a short count
past the end, which is how truncated files keep failing the way they do now.

The default is the existing code path: OpenEXRCore opens and reads the file
itself, chunk by chunk, with every loader thread independent.

The reader count is process-wide state in `reader_exr.c` rather than a
parameter, since it is a policy about the one storage the process reads from,
and the permits it controls are process-wide anyway. `reader_probe()` (header
only) keeps the default file I/O; it reads a few kilobytes.

Alternatives considered, one line each:
- A reader pool feeding a decoder pool through a queue: both storages measured are slower than the decoders, so the queue would sit empty; the permits give the same concurrency without a pipeline in the cache.
- A mode switch (`--io sequential`): one reader at a time halves the share's rate; the count is the real variable.
- Fewer loader threads on spinning disks: measured worse at every count above one; the pattern is the problem, not the count.
- Larger reads inside OpenEXRCore: no such knob; the library reads per chunk.
- Memory-mapping the file: page faults are still small random reads under concurrency.
- Auto-detecting the drive type: platform-specific and wrong for network shares; an explicit option is honest.

## Files
**Source:** `src/platform.h`, `src/platform.c` (file reading), `src/reader.h`, `src/reader_exr.c`, `src/main.c`
**Tests:** `tests/test_reader.c`
**Docs:** `README.md`, this plan
**Dependencies added:** none.

## Steps
- [x] 1. `rp_file_open/size/read/close` in platform, both backends. Test intent: the UTF-8 path case in `test_reader` exercises the wide-path open in whole-file mode.
- [x] 2. `reader_set_readers()`; whole-file `reader_load()` reads under a permit and decodes from memory. Test intent: every `test_reader` decode case passes with readers 0, 1 and 2; `test_player` unchanged.
- [x] 3. `--readers` option in `main.c`. Test intent: usage text; a bad value is rejected like the other options.
- [x] 4. Reader-count test loop and the scratch growth case in `test_reader`.
- [x] 5. Benchmark: local drive with readers 0 and 1; share with readers 0, 8 and 15; 15 threads throughout, uncached ranges. Results, frames/s at 15 threads: local drive readers 0: 5.9 to 6.2; readers 1: 12.8 to 14.4; readers 2: 9.9 to 10.9. Share readers 0: 83 to 92; readers 8: 80; readers 15: 82.
- [x] 6. README.

## Tests
**Unit:** `test_reader` (decode cases, damaged files, tiled, UTF-8 path, with
readers 0, 1 and 2; growth case in whole-file mode), `test_player` (cache under
the default), `test_draw` unchanged.
**Measured:** the benchmark, both storages, on frame ranges the probe shows as
uncached.
**Manual:** `build\windows\Release\ramplayer.exe --readers 1 --mem 42G D:\test\C01`
(opens a window): the residency band should fill at roughly twice the speed of
the default on this drive, and once a region is green playback through it runs
at the full rate. Then the share without `--readers`, to confirm the default is
unchanged there.

## Deviations

### Read request size on the share
**What came up:** The first whole-file reader issued one `ReadFile` for the
whole 12 MB. On the share that measured 59 to 66 frames/s at 4 to 15 readers,
well under the 80 to 86 the raw benchmark had shown for whole-file reads, which
had used 1 MB `fread` pieces.
**Options:** keep one large request and accept the gap; read in 1 MB requests.
**Chose:** 1 MB requests in `rp_file_read()`.
**Why:** re-measured at 80 to 82 frames/s, within a tenth of the default. The
SMB redirector pipelines requests of that size; one huge request does not.

### Two readers on the spinning disk
**What came up:** `--readers 2` measured 10 to 11 frames/s against 13 to 14
for one reader on the local drive: two sequential streams still make the head
alternate. **Chose:** no code change; the README says to use 1 on a spinning
disk. The count exists for shares and SSDs.

## Changelog
- `src/platform.h`: file reading functions added.
- `src/reader.h`: `reader_set_readers()` added.
- `src/reader_exr.c`: whole-file mode reads under N permits and decodes from memory; `DecodeScratch` gains a file buffer.
- `src/main.c`: `--readers N`.
