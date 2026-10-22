Accepted by the user 2026-10-13:
- Two sensitivity levels (Public, Admin), not three: no Operator level, nothing sits between editor and admin.
- Untagged engine lines are Public; all 117 Logger::Info/Warn/Error sites in editor-server/src (EditorServer, Host, ExtensionHost) now go through new ServerLog (Admin); five res://-only lines use ServerLog::PublicInfo.
- Sequence leak handled by numbering the public view on its own (own seqs and epoch); the shared byte/count limits still show as a larger dropped, documented.
- Public-view and GetEngineHealth detail text pass a heuristic host-path scrubber (project root to res://, other absolute paths to <host path>); non-log events are not scrubbed.
- Logger gained an optional sensitivity argument and Logger::CurrentSensitivity() (thread-local during delivery); subscriber signature unchanged. Engine file touched: engine/include/engine/Logger.hpp, engine/src/Logger.cpp.
- Accident: a stale script in the shared scratchpad was run once and duplicated edits in another worktree (N2Engine-net-a1-presets-settings, engine/src/Application.cpp); reverted with git checkout of that one file, which held only those duplicates.
- Fix round: ServerLog gained PublicWarn/PublicError; the game-frame failure and the three 'Can't reload' lines are Public (path-scrubbed), so editors see why a game paused.
- Fix round: SetHostRoots now takes <project>/assets (what res:// resolves to); other project-folder paths such as .n2/ scrub to <host path>. Docs updated.
- Fix round 2: docs example path repaired (BEL characters replaced by backslashes); docs-only change
