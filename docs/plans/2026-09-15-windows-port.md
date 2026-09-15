# Windows port — rapid plan

**Mode:** rapid   **Date:** 2026-09-15
**Goal:** Make ramplayer build and run on Windows with MSVC and vcpkg, without
changing how it builds on Linux. The Linux-only surface is small: pthreads in
the cache, dirent/stat and forward-slash path splitting in sequence discovery,
clock_gettime in the clock, nanosleep in one test, and a CMake file that assumes
pkg-config and GCC flags. All of it moves behind one small platform module, and
the CMake grows a config-mode dependency lookup with the existing pkg-config
path kept as the fallback. The change is meant to go back upstream as-is.

## Steps Summary
1. Add `vcpkg.json` (sdl2, openexr; optional `fixtures` feature for the OpenEXR tools), `CMakePresets.json` with a `windows` preset, and `.gitattributes` so shell scripts stay LF on Windows checkouts.
2. Add `src/platform.h` / `src/platform.c`: mutex, condition variable, thread, monotonic clock, sleep, directory listing, file-kind test. POSIX backend keeps pthreads; Windows backend uses SRWLOCK, CONDITION_VARIABLE, `_beginthreadex`, QueryPerformanceCounter, FindFirstFile. `windows.h` appears only in `platform.c`.
3. Port the callers: `cache.c` to the platform threads, `util.c` clock to the platform clock, `sequence.c` to the platform directory API with separator-agnostic path helpers and a local case-insensitive extension test, `tests/test_player.c` to the platform sleep.
4. CMake: `find_package(SDL2 CONFIG)` and `find_package(OpenEXR CONFIG)` first, pkg-config as fallback; MSVC flag block (`/W4`, `/experimental:c11atomics`, `/utf-8`, `_CRT_SECURE_NO_WARNINGS`); guard the GCC-only flags and `m`; link `SDL2::SDL2main`; add `platform.c` to every target.
5. Configure and build Release with the Visual Studio 2026 bundled vcpkg; fix whatever MSVC reports until the build is warning-free at `/W4`.
6. Make `tools/make_test_data.sh` runnable from Git Bash (overrides for the compiler, font, and `exrmaketiled` paths) and generate the fixtures under `test/`.
7. Build and run `test_draw`, `test_reader`, `test_player`; add a Windows-only backslash-path case to `test_reader`.
8. Add a Windows section to the README, then hand the exe over for the manual check.

## Approach
One platform module with two backends selected by `#ifdef _WIN32`, and the rest
of the program unchanged in structure. The Windows types are declared in
`platform.h` as structs holding a single `void *`, which is what SRWLOCK,
CONDITION_VARIABLE and HANDLE are, so `cache.c` keeps embedding its mutex and
condition variable by value without pulling `windows.h` into a header (a
`static_assert` in `platform.c` pins the sizes). On POSIX the same structs wrap
the pthread types, so Linux behaviour is byte-for-byte what it was.

C11 atomics stay as they are: MSVC 19.51 compiles `<stdatomic.h>`, including
`_Atomic unsigned char` struct members and `atomic_compare_exchange_strong`,
under `/std:c11 /experimental:c11atomics` (verified with a probe on
2026-09-15). Without the flag the header refuses to compile, so the flag is
added for MSVC only.

Dependencies come from the Visual Studio bundled vcpkg (registry baseline
`e03dc9b2`, openexr 3.4.12, sdl2 2.32.10). OpenEXR's CMake config exports
`OpenEXR::OpenEXRCore`, which is exactly the library the program wants; SDL2's
exports `SDL2::SDL2` and `SDL2::SDL2main`. Both configs also exist on Linux
distributions, so config-mode lookup goes first everywhere and the current
pkg-config code stays as the fallback rather than being deleted.

Paths: splitting accepts `\` and `/` on Windows; joining always uses `/`, which
every Windows API accepts. Directory listing uses the ANSI `FindFirstFileA`
family, so non-ASCII path names are out of scope for this port (recorded below).

Alternatives considered, one line each:
- C11 `<threads.h>`: MSVC 17.8+ and glibc ship it, macOS does not; the wrapper costs about the same and leaves Linux untouched.
- vcpkg `pthreads` port (pthreads4w): a new runtime dependency to wrap one file; no.
- SDL threads: `cache.c` and the tests are deliberately SDL-free; keep it that way.
- Windows-only CMake: rejected by the user; the tree must still build on Linux.

## Files
**Source (new):** `src/platform.h`, `src/platform.c`
**Source (modified):** `src/cache.c`, `src/util.c`, `src/sequence.c`
**Tests:** `tests/test_player.c` (sleep), `tests/test_reader.c` (backslash path case)
**Tools:** `tools/make_test_data.sh` (Windows overrides), `tools/mkexr.c` only if MSVC objects
**CMake:** `CMakeLists.txt`, `CMakePresets.json` (new), `vcpkg.json` (new)
**Repo:** `.gitattributes` (new), `README.md` (Windows section), this plan
**Dependencies added:** none new to the program. `vcpkg.json` names the two it already has, sdl2 and openexr. The optional `fixtures` feature adds `openexr[tools]` for `exrmaketiled`, a development-only tool.

The project has no `changelog.md`, `docs/api/` or `docs/dependencies.md`; it is
an upstream project, not a workspace project, so the ledger and changelog
entries live in this plan (see Changelog at the end) rather than in new
scaffold files.

## Steps
- [x] 1. Manifest, preset, gitattributes. Test intent: `cmake --preset windows` resolves both ports.
- [x] 2. `platform.h`/`platform.c` with both backends. Test intent: exercised by `test_player` (threads, wait/broadcast, clock) and `test_reader` (directory listing).
- [x] 3. Port `cache.c`, `util.c`, `sequence.c`, `test_player.c`. Test intent: the three suites unchanged in meaning.
- [x] 4. CMake changes. Test intent: configure succeeds with config-mode packages; fallback branch still reads correctly.
- [x] 5. Release build clean at `/W4`. Test intent: zero warnings; `ramplayer.exe --help` prints usage. (Debug configuration also builds clean.)
- [x] 6. Fixture script under Git Bash; `test/` populated (seq_a, edge, sparse, mixed).
- [x] 7. Run the three suites; add the backslash-path case; all pass (test_draw 36, test_reader 70, test_player 61).
- [x] 8. README Windows section; hand over for manual check (manual check pending).

## Tests
**Unit:** existing `test_draw` (rasteriser), `test_reader` (decoding, sequence
discovery through the new directory API, plus a new `#ifdef _WIN32` case that a
backslash path such as `test\sparse\shot_0001.exr` expands to the same seven
frames as the forward-slash form), `test_player` (cache threads, playback,
budget, scrub stress through the new mutex/cond/thread wrapper and clock).
**Manual:** launch `build\windows\Release\ramplayer.exe test\seq_a` (asks
first; it opens a window). Check: window opens sized to the 1280x720 sequence,
frame numbers burnt into the frames advance on `space`, scrubbing the timeline
tracks the mouse, the residency band fills green, `q` quits and the console
shows no failed-frame message.

## Deviations

### Non-ASCII paths on Windows
**What came up:** The plan declared non-ASCII paths out of scope because the
port used the ANSI file APIs. Self-review showed the reasoning was wrong: SDL2main
hands `main()` UTF-8 arguments and OpenEXRCore opens paths as UTF-8, so an ANSI
directory listing disagrees with both neighbours and even code-page-representable
names fail in one direction or the other.
**Options:** keep ANSI and document it; convert at the boundary and use the wide
file APIs.
**Chose:** the wide APIs with UTF-8 conversion inside `platform.c`, plus a fixture
directory with non-ASCII names and a `test_reader` case that lists, expands and
decodes from it.
**Why:** the fix is a few lines in the one file that already owns the platform
boundary, and it makes the Windows build consistent with what its two
dependencies already assume.

### Drive-relative paths (`C:shot.0001.exr`)
**What came up:** Self-review noted that a drive-relative Windows path with no
separator resolves its directory to `.` (the process directory) while the file
itself resolved against the drive's own current directory.
**Options:** canonicalise with `GetFullPathName` in the platform layer; special-case
`X:` in the path splitting; leave it.
**Chose:** leave it, documented in the README.
**Why:** it is an unusual way to name a file and the fix either spreads platform
conditionals into `sequence.c` or adds a canonicalisation step for every input.
Worth revisiting if anyone hits it.

### Release debug information on MSVC
**What came up:** The upstream build sets `-O2 -g` so Release carries symbols;
guarding that for MSVC lost the intent. **Chose:** `/Zi` and `/DEBUG` for the
Release configuration on MSVC. The sources use no `assert`, so `NDEBUG` makes
no difference.

## Changelog
- CMake: new source files `src/platform.c` compiled into `ramplayer`, `test_player`, `test_reader`, `test_draw`. Dependency lookup prefers CMake config packages, pkg-config remains as fallback. MSVC Release builds carry debug information.
- Public header: `src/platform.h` added (internal to the program; no installed API).
- Limitation: drive-relative paths such as `C:shot.0001.exr` are not handled on Windows; paths longer than `MAX_PATH` are subject to the same limit OpenEXRCore already has.
