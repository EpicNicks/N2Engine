Accepted by the user 2026-10-13 at head fd50f54:
- Toast is the existing error banner (store.error), no new toast component
- Error code crosses the context bridge via IpcResult.code and the page error name 'EngineError:<code>' (bridge copies only name/message); unverified in Electron
- A readOnly denial sets store deniedAsReadOnly (until the connection changes) so writes disable even if ServerInfo said otherwise
- Open scene, Recover autosave and autosave prompts are also disabled/skipped on a read-only host (OpenScene and restore/discard are writes in the host's table)
- Moved ErrorBanner from index.tsx to ui.tsx and added exported SceneButtons/ReadOnlyBadge in editor.tsx so DOM tests can render them
- New src/renderer/write-guard.ts is CRLF like other src files; new tests are LF
- Play is disabled on a read-only host (WritePlaySnapshot is a write)
