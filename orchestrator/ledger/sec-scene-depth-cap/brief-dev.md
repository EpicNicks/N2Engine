# Task sec-scene-depth-cap: Editor host security S2: nesting-depth cap for scene/prefab/snapshot JSON and a hierarchy depth limit in GameObject::Deserialize (audit F4)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: sec-scene-depth-cap.

## Issue / plan
Source: the security audit at C:/Method Notes Obsidian/orchestrator/proposals/editor-host-security-audit-2026-10-15.md. Read finding F4, section 6 item 6 and section 5 (unverified recursion) first. Line numbers below are from origin/master at a1aa1005; find the code again at current master.

THREAT CLOSED (F4, Medium, an authenticated editor A3).
- GameObject::Deserialize (engine/src/gameobject/GameObject.cpp ~919-1013) recurses once per `children` level with no depth limit (the 'Deserialize Children (recursive)' block, ~1002-1010).
- HandleLoadScene (editor-server/src/EditorServer.cpp ~3545-3560) parses up to 64 MiB of client JSON with nlohmann::json::parse and no depth check. WriteTextAsset accepts a .scene file of up to 4 MiB, which OpenSceneFile and WritePlaySnapshot then parse unchecked.
- So `LoadScene` with `children` nested about 100,000 deep (about 3 MB) crashes the host with SIGSEGV (stack overflow).
- Saving that scene and setting it with SetStartupScene turns it into a crash loop.
- Other recursive walkers can also overflow once such a tree exists: copying a deep nlohmann::json, dump(), operator==, GameObject::Serialize (~874-916), and SetScene/GetChildren/Find recursion (~419-446, ~669-672, ~746-752).

THE EXISTING HELPER.
- editor-server/include/editor-server/Commands.hpp ~97-133 already has CheckJsonDepth(text, maxDepth), a string-aware pre-scan of [ and {, with MaxJsonDepth = 64. ReadBoundedJson uses it for some command fields only.
- nlohmann's parser itself is iterative, but the trees it produces are not safe to walk recursively. A pre-scan before parse is therefore the simplest robust approach.
- Move the scanner to engine/ (for example engine/include/engine/io/JsonLimits.hpp or next to the serialization code) so both engine and editor-server can use it, and make Commands.hpp call the moved one. The command-field cap of 64 stays as it is.

CHOOSING THE CAPS (do not use 128).
- Each GameObject level costs 2 JSON levels (the object and its children array), plus the nesting inside component data.
- Model instantiation builds GameObject hierarchies from imported glTF/FBX nodes. The importers allow node trees up to kMaxNodeDepth = 256 (assetimport/include/assetimport/ModelImporter.hpp:49; engine/src/rendering/Model.cpp ~647 goes through PrefabManager::InstantiatePrefab). A model placed under a few scene parents must still load and save.
- Suggested caps:
  - a hierarchy cap of 512 GameObject levels, as a named constant in engine/;
  - a JSON text nesting cap derived from it, 2 * 512 + 64 = 1088, as a named constant computed from the hierarchy cap.
- Prove in CI that a tree just under the cap is safe on the smallest stack the tests run on, which is MSVC Debug on Windows with a 1 MiB main-thread stack. That means Deserialize, Serialize and dump of that tree, under the test runner's main thread.
- If that cannot hold 512, lower the hierarchy cap, but keep it at least kMaxNodeDepth + 32. Say in the PR what you measured and chose.

SCOPE.
(i) Run the JSON nesting check BEFORE parsing at every entry point where scene, prefab or snapshot JSON comes from a client or from disk. On failure, give a clear error ('the scene JSON nests more than N levels deep') and make no change to the open scene. The entry points:
- HandleLoadScene (~3550);
- the WritePlaySnapshot validation parse (~4148);
- the play-mode snapshot load (~4200, EnterPlayMode, which the --play child uses);
- RestoreSceneSnapshot (~5859);
- the autosave restore (~6283);
- OpenSceneFile (~6515);
- the edit-history ParseStored (~850; server-made, but cheap to cover);
- engine/src/sceneManagement/SceneFile.cpp SceneFile::Load (~20; it parses from a stream: read it into a string first, or use a parser callback, whichever is simpler);
- the prefab load path in engine/src/prefabs (wherever prefab text is parsed from a file before Prefab::Deserialize, ~34-70, or PrefabManager::InstantiatePrefab, ~161-215).

If ReadTextFile-based paths already hold the text as a string, pre-scan that string. Do not add size caps here; that is audit F15, a separate item.

(ii) Add an explicit depth counter to GameObject::Deserialize: a private overload or a depth parameter with a default, keeping the public signature source-compatible. Past the hierarchy cap, throw std::invalid_argument naming the cap. Callers already turn a throw into a failed load: Scene::FromJSON, Prefab::Deserialize's InvalidRootObject and InstantiatePrefab's error. So:
- a too-deep scene is an Error response, not a crash;
- a partially built object tree is released cleanly;
- the resolver registrations of the failed load do not leak into the live scene. Check what Scene::FromJSON does on a throw.

Give the other recursive hierarchy walkers that a loaded tree reaches the same protection. That means Serialize and prefab instantiate (and clone, if there is one). Because the load cap bounds the tree, they are safe by construction; add a comment or a debug assert, not a second limit.

(iii) If it is a small change in engine/, also refuse a reparent (GameObject::SetParent/AddChild, reached through the SetEntityParent command) that would make the tree deeper than the cap. Otherwise an editor can still build a deep tree one command at a time. If it is not small, leave it and document it as not covered.

DO NOT TOUCH ResolveScenePath or ResolveSceneFile (audit F16 is a separate item), file size caps (F15), or any protocol message.

TESTS.
- Engine (tests/engine/serialization/SerializationRobustnessTests.cpp, tests/engine/sceneManagement/SceneStorageTests.cpp, tests/engine/prefabs/PrefabTests.cpp):
  - a scene whose hierarchy depth is cap-1 deserializes, serializes and dumps;
  - one at cap+1 is rejected with an error and no crash, through GameObject::Deserialize, Scene::FromJSON and SceneFile::Load;
  - JSON text at the nesting cap +1 is refused before parsing;
  - a prefab with a cap+1 root hierarchy is refused (InvalidRootObject or the new error) and one at cap-1 instantiates;
  - a 100,000-deep `children` chain, built as text without recursion, is an Error, not SIGSEGV, on every engine entry point.
- Editor server (tests/editor-server/project, for example EditorProjectTests.cpp or EditorHierarchyTests.cpp):
  - LoadScene with the 100,000-deep chain returns Error, and the host still answers the next command;
  - OpenScene of such a .scene file written to the project returns Error;
  - WritePlaySnapshot of it returns Error;
  - the cap-1 scene loads through LoadScene;
  - if (iii) is done, SetEntityParent past the cap is refused.
- Build deep JSON iteratively as a string; never build it with a recursive helper.

DOCS.
- docs/serialization.html, the #gameobject-json and #failure-handling sections: the hierarchy cap, the JSON nesting cap, why they are those numbers (the glTF node cap) and the error text.
- docs/scenes.html, #loading: one line plus a link.
- docs/editor.html security notes: a short line that scene JSON from clients is depth-checked.
- docs/testing.html: rows for the new tests.

CI. Nothing builds locally; CI is the build. There are 8 checks, and the required one is `linux (Release, x86_64, Clang)`. The Windows Debug leg is the one that proves the cap-1 stack claim, so it must be green.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^editor-server/src/
- ^editor-server/include/
- ^tests/
- ^docs/

## Acceptance
- All Tests-workflow jobs green (8 checks; required linux (Release, x86_64, Clang)); Windows Debug leg proves the cap-1 tree is stack-safe
- JSON nesting pre-check before parse on every untrusted scene/prefab/snapshot entry point (LoadScene, OpenScene, WritePlaySnapshot, play snapshot, RestoreSceneSnapshot, autosave, SceneFile::Load, prefab load)
- GameObject::Deserialize has an explicit depth cap (>= glTF kMaxNodeDepth + margin) and fails the load with an error instead of overflowing
- Tests: cap-1 loads, cap+1 and a 100000-deep chain are rejected without a crash, for Deserialize, LoadScene via the editor server and a prefab
- No protocol change; F16 resolvers untouched; docs document the caps and why
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Security-sensitive (audit F4). The reviewer must try to break it: (1) find any scene, prefab or snapshot JSON entry point (client or disk) that still parses unchecked, including SceneFile::Load, autosave, edit history, the play child's snapshot and Model prefab JSON; (2) check that the pre-scan handles strings with escaped quotes and brackets, and nesting through component data, not only children; (3) check that a failed deep load leaves the open scene and the ReferenceResolver untouched and leaks nothing; (4) check that the hierarchy cap admits every model the importers accept (kMaxNodeDepth 256) under some parents; (5) look for any remaining recursive walk (Serialize, dump, operator==, json copy, SetScene, Find) reachable with a tree deeper than the cap, especially through SetEntityParent if (iii) was skipped, which must then be documented; (6) the deep test inputs must be built iteratively. The audit's example cap of 128 is deliberately not used: each hierarchy level costs 2 JSON levels, and imported models can be 256 nodes deep. Touches EditorServer.cpp at the LoadScene, OpenScene and snapshot sites only; expect a trivial merge with sec-host-dos-limits (ServerLoop and Send, other functions) and on docs/testing.html. May run in parallel with sec-host-dos-limits and sec-lua-budget.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-scene-depth-cap` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref sec-scene-depth-cap` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-scene-depth-cap/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head sec-scene-depth-cap --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-scene-depth-cap/dev.json: {"pr":<number>,"branch":"sec-scene-depth-cap","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/sec-scene-depth-cap` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
