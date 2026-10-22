Accepted by the user 2026-10-13:
- OnBeforePlay warnings are extensionOutput events that are NOT Admin-tagged (every role gets them); plan did not say who sees them; same keys as an Editor.Log event, no protocol version bump (only the PollEvents description text changed; generators up to date, protocol tests pass)
- A hook declaration is refused at load when its capability is missing (scene.read, or assets.read for OnAssetsChanged)
- --editor-extensions-watch without --editor-extensions is a parse error
- The watcher also loads new files and drops removed ones, not only reloads the changed one
- Asset writes by an extension are not rolled back when its invocation fails
- Hooks are not called inside another extension call (an extension's own asset write does not call OnAssetsChanged)
- Hook-run summaries are logged at info (no debug level in the host); hooks of one trigger share 2 s and each run is capped at 1000 requests (numbers not in the plan)
- Nested policy table now allows ListAssets, ReadTextAsset, WriteTextAsset, CreateFolder (17 commands)
- Review had only 4 low findings (no blocker/high/medium); fixed the docs one (SetImportSettings pushes assetsChanged without calling OnAssetsChanged), left the 3 code lows as declared
