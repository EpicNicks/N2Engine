MCP3 of the MCP design (`mcp-engine-design.md`, M1-M12 accepted): play tools for a `--launch` bridge, resources and subscriptions over the event pump, and prompts.

## Play tools (`--launch` only)
- `n2_play_start` / `n2_play_stop` go through `src/main/play-session.ts` (snapshot of the open scene on the edit host, `N2EditorHost --play` with a fresh token in the child's environment only, a second connection). The play controller lives in `mcp/src/play.ts`.
- `n2_play_control`: pause, resume, step 1 to 600 frames of a paused game, in `Step` chunks of 100 with a progress notification and a cancellation check between them.
- `n2_play_input` (up to 256 events, typed schema per event kind), `n2_play_observe` (state, the game's log since the last observe, up to 8 live entities, optional picture), `n2_play_script` (up to 50 ops: input, step, screenshot (at most 4), expectLog; a progress notification per op; cancellation between ops; the game is paused first and left paused; a script that ends early sends `releaseAll`).
- Registered only when the connector is `launch` and play options are given: a `--connect` bridge does not register, list or accept them, whatever it is given (test). They are write-class: listed with the write tools (editor, not read-only, operator flags), so a viewer gets none (test).
- Budgets per agent session: host write commands (`--max-writes`), 20,000 stepped frames, 50 game pictures; per script 3000 frames, 4 pictures, 1024 px, 6 MB of PNG. New error codes `play_not_running`, `play_failed`, `cancelled`. The game's token is registered as a secret; the snapshot path never reaches the model.

## Resources and subscriptions
- `n2://project`, `n2://scene`, `n2://scene/hierarchy`, `n2://console`, `n2://history`, `n2://component-types`, templates `n2://entity/{uuid}` and `n2://asset/{+path}`. A read runs the matching read tool's handler and renders it with the same function as tool results (`renderOutput`): same envelope, scrubbing, size guard, secret check; rate limited with tool calls; `cacheScope: private`.
- `resources/subscribe` (2025-11-25) and `subscriptions/listen` (2026-07-28, served by the SDK's stdio entry; the bridge sees acknowledged filters and cancels through a transport tap, `serveBridge`) feed one `ResourceWatch` per connection: at most 32 watched URIs and 8 listen streams. The frontend's `EventPump` polls the host only while something is watched (first poll primes the cursor, no queue), maps `sceneChanged` / `historyChanged` / `projectChanged` / `assetsChanged` / `log` to URIs, announces each URI once per poll and the console at most once a second; notifications carry the URI only.

## Prompts
`n2_build_level(description)`, `n2_inspect_scene`, `n2_playtest(goal)` (only with the play tools), `n2_fix_console_errors`, `n2_write_lua_component(behaviour)`; each restates the data-is-not-instructions rule; arguments capped at 2000 characters and quoted.

## Other
- `mcp/tool-schemas.golden.json` regenerated as a `--launch` bridge; it now also pins prompts, resources and resource templates.
- Tests: `mcp/test/play.test.ts` (every play flow, cancellation, failures, budgets, output schemas, `--connect` absence, viewer), `mcp/test/resources.test.ts` (reads, both subscription eras, unsubscribe stops polling, bounds, console debounce, prompt listing and bodies), `mcp/test/play-host.ts` (fake play host and child). `npm run typecheck` and `npm test` pass locally (1877 + 396 + 211).
- README: one new section "Play tools, resources and prompts (MCP3)" in the MCP chapter; the intro sentence and "Not there yet" no longer say these are missing.

## Review fixes (3606019)
- Resources follow `--disable-tools`: a resource whose read tool is disabled is not registered (not listed or readable) and `resources/subscribe` / `subscriptions/listen` refuse or skip it. `n2://scene` and `n2://history` map to `n2_status` (which reports both). Test in `resources.test.ts`.
- A call the bridge times out is aborted: `HostContext.signal` is aborted on the timeout and combined with the client's cancellation signal, so a play step/script stops at its next chunk and the script releases its input; the serial queue waits for the timed-out handler to settle (at most one more `callTimeoutMs`) before the next call runs. Test in `play.test.ts` (fails without the fix).
- A script stopped by a failed `expectLog` (without `continueOnFailure`) now sends `releaseAll`. Test in `play.test.ts`.
- `npm run typecheck` and `npm test` pass locally (1877 + 396 + 214).

## Deviations
- The bridge lives in `mcp/`, which the brief's allowed paths did not list (it named `src/mcp/`, which does not exist); all bridge changes are under `mcp/`.
- The playtest script pauses the game first and leaves it paused (Step needs a paused game; this makes scripts repeatable).
- Play budgets (frames, pictures) are per agent session, like the write budget.

## Not verified
- Play against a real N2EditorHost (`--play` child, `GetEntity` live values on a play host, `RenderFrame` cost): the tests use the real play-session code with a fake child process and a second fake host written from the protocol docs.
- Whether Claude Code or Claude Desktop shows resources, subscriptions, prompts or progress notifications to the person or the model.
- A real client's `subscriptions/listen` over stdio beyond the SDK's own client.

## Approved deviations
Accepted by the user 2026-10-13:
- all bridge changes are under mcp/ (where MCP1/MCP2 put the bridge), a path the brief's allowed-path list omits (it names src/mcp/, which does not exist); ratify will flag mcp/ files
- n2_play_script pauses a playing game before its ops and leaves it paused (Step needs a paused game; makes scripts repeatable)
- play budgets (20000 frames, 50 pictures) are counted per agent session like the write budget; each Step chunk, SetPaused, SendInput and the snapshot also spend host write commands
- resource reads return the read tool's text (summary + n2-data envelope) as text/plain, with n2/untrusted and n2/source in _meta
- subscriptions/listen is observed through a transport tap on the SDK stdio entry (serveBridge), since the SDK serves listen streams outside the server instance
- n2_playtest prompt is listed only when the play tools are (--launch)
- README.md: besides the new MCP3 section, the MCP intro sentence and 'Not there yet' were updated so they no longer say play/resources/prompts are missing
- review fix: n2://scene and n2://history are gated on n2_status (no dedicated read tool); a timed-out call's queue successor waits at most one more callTimeoutMs for the abandoned handler
