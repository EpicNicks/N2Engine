---
name: electron-editor-frontend
description: "N2Engine's editor client is a separate repo at C:\\Method Notes Obsidian\\N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend), Electron + TS, follows the editor-server protocol"
metadata:
  node_type: memory
  type: reference
  originSessionId: 6b1b31bc-fe59-40f5-8570-4fcd9e92983f
  modified: 2026-10-06T14:04:09.021Z
---

The editor frontend for N2Engine lives at `C:\Method Notes Obsidian\N2Engine-Electron-Frontend` (GitHub EpicNicks/N2Engine-Electron-Frontend). The user describes it as very barebones, meant to follow the editor-server pattern (it talks to N2Engine's `editor-server` over the `protocol.json` binary protocol).

**Layout and state as of 2026-10-06:**
- Electron 39 and TypeScript 5.9.
- Files: `src/main.ts`, `preload.ts`, `renderer.ts`, `engine-client.ts`, `serialization.ts`, `index.html`.
- The repo has one commit ("first commit") and no `.github` CI.
- Frontend PR #1 (merged 2026-10-06) synced the protocol (`npm run sync-protocol`), added an Engine health panel, and Web Audio playback of GetAudio with a jitter buffer, plus `node --test` tests.

**Building:** Node 22 and npm 10 are installed on this machine, so unlike the C++ engine this repo can be type-checked and built locally (`npm install`, `npm run build` = `tsc`).

**Open work:** N2Engine `docs/audio.html` still says no client plays audio; update it. The #8 client side is now done.

Related: [[github-accounts]] (EpicNicks credential, now stored for path `EpicNicks/N2Engine-Electron-Frontend.git`; use it for REST writes), [[merge-authorization]], [[no-claude-coauthor]], [[n2engine-ci]].

**Update 2026-10-13:** the frontend repo HAS CI (.github/workflows/ci.yml: ubuntu, Windows, protocol on Node 22), contrary to older notes. Merged since: #26 (TLS pinning/TOFU direct remote), #27 (DOM test harness: jsdom + @testing-library/preact, `npm test` runs dist/test then dist/test-dom as separate node --test runs; engines.node >=22.22.2). Engine protocol is now 1.11.0 (ErrorCoded 0x1C, ServerInfo.readOnly, Hello errorCodes flag, optionalTrailing): the frontend has NOT been synced yet (engine-ref/regenerated client, read-only badge, disable writes, map denial to a toast): next frontend item.
