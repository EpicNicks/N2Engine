# Task uuid-random-entropy: UUID::Random: one seeded generator per thread with real entropy (closes #140)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-uuid-random-entropy. Closes ##140.

## Issue / plan
GitHub issue #140 (read it first via the gh wrapper: api repos/EpicNicks/N2Engine/issues/140). `UUID::Random()` (math/src/math/UUID.cpp, around lines 82-104) constructs a NEW std::mt19937_64 on every call, seeded from ONE 32-bit std::random_device value, so a UUID has ~32 bits of entropy and two calls collide whenever random_device repeats a value; every Base::Asset (Component, GameObject) draws one in its constructor. This caused a flaky CI failure (PrefabInstantiateTest.ReferencesOutsideThePrefabAreKeptWhenTheSceneIsGiven on the GCC 14 leg: two components got the same UUID). Fix: (1) one generator per thread: `static thread_local std::mt19937_64`, seeded ONCE per thread from a std::seed_seq of several random_device words (e.g. 8 x 32 bit) mixed with a high-resolution time stamp, the thread id hash and an address (ASLR), so a random_device that returns a constant (broken RDSEED) cannot produce identical streams in two threads or two runs; keep the UUID v4 version/variant bits exactly as today (read the current code and keep the byte layout and string format); alternatively, if a portable OS source is easy and well-tested in this codebase, fill the 16 bytes from getrandom (Linux)/BCryptGenRandom (Windows) with the seeded generator as a fallback: choose the simpler one that is correct on Windows MSVC, Linux x86_64/arm64 and macOS-free CI, and say which you chose. No behaviour change for UUID parsing/printing/ordering/hashing. (2) Tests (tests/math, registered like the neighbours; every leg): generating 200k UUIDs on one thread yields all unique; 8 threads x 25k UUIDs in parallel are globally unique; the version/variant bits are valid on every UUID (as the current tests check, extend if they do not); two threads' first UUIDs differ; thread-local state does not break when threads start and exit repeatedly. Make the test fast (< 1 s). Do NOT add tests that depend on the machine's entropy quality. (3) Docs: docs/math.html (around line 380, the UUID section) must describe the new generation honestly (per-thread generator, how it is seeded, that it is not cryptographically secure). (4) PR body: say it closes #140 and quote the root cause. Nothing builds locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`).

## Allowed paths (regex, repo-relative)
- ^math/
- ^tests/math/
- ^docs/math.html$
- ^docs/testing.html$

## Acceptance
- All Tests-workflow jobs green
- UUID::Random uses a per-thread generator seeded once from a multi-word seed_seq mixed with time/thread/address (or an OS source); byte layout and string format unchanged
- Uniqueness tests (200k single-thread, 8x25k multi-thread) pass fast on every leg
- docs/math.html updated; PR closes #140
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: confirm the v4 bits and format are unchanged (existing UUID tests untouched and passing), the thread_local seeding is correct (seed once per thread, no per-call construction), no data races, the tests cannot flake, and the doc matches. Unverified: whether the original failure was a runner RDSEED issue (hypothesis).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs uuid-random-entropy` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-uuid-random-entropy` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/uuid-random-entropy/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-uuid-random-entropy --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/uuid-random-entropy/dev.json: {"pr":<number>,"branch":"fix-uuid-random-entropy","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-uuid-random-entropy` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
