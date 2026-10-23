# Verification notes, 2026-10-14 (local only, read-only; no GitHub calls)

## JOB 1: TypeScript generated string reader and the UTF-8 BOM

### Result: the bug is already fixed and merged. The progress-log line 145 ("Open: possible engine generator bug") is stale.

- Progress log lines 146-147 and 152-153 record the fix: engine PR #104 (merge b3e06b7, commits 587b4df, 6714895) and frontend PR #24 (engine-ref sync, BOM workaround removed).
- Generator: editor-server/protocol/generators/generate_typescript.py lines 70-71 now read
  `const textDecoder = new TextDecoder("utf-8", { ignoreBOM: true });` with a comment explaining why.
- Generated copies carry it: N2Engine editor-server/clients/web/protocol.generated.ts and the frontend src/protocol/protocol.generated.ts (line 802) both have `ignoreBOM: true`. (The task text said clients/web at the repo root; the real path is editor-server/clients/web/.)
- Node demonstration (scratchpad bom.js, bytes EF BB BF 68 69):
  - `new TextDecoder('utf-8')` -> "hi", length 2 (BOM stripped)
  - `new TextDecoder('utf-8', {ignoreBOM:true})` -> U+FEFF 'h' 'i', length 3 (BOM kept)
  So the premise (default options strip) is confirmed as a platform fact, and the repo already uses the safe option.
- Tests exist: engine editor-server/protocol/tests/typescript-codecs.test.mjs line 152 ("a string keeps a leading byte order mark ... through encode and decode", covers Hello clientName/token/user and Error message); test-vectors.json already has U+FEFF vectors (payloads containing `efbbbf`/`feff`-style bytes, e.g. "c01dfeffbf1dfeff"). Frontend src/test/codec.test.ts lines 185-196 and engine-client-assets.test.ts line 120-132 assert BOM round trips.
- Frontend workaround: no longer exists. grep for `decodeTextData` in the frontend src finds only the generated `decodeTextDataResponse`; no BOM special-casing remains outside tests and host-launcher.ts decoders (host-launcher.ts lines 68 and 559 use default `TextDecoder("utf-8")`, but those decode host stdout/log text, not protocol string fields, so BOM stripping there is harmless).
- C++ and C# clients: not affected. generate_csharp.py states it emits declarations only, no encoding code, and nothing uses it (unsupported). generate_cpp.py maps string to std::string (bytes, no decoding step). The host in C++ treats strings as raw bytes, so a BOM passes through untouched. Only the TS client decoded text.
- Fields that can carry BOM-bearing or user text (all go through the same string reader, now fixed): ReadTextAsset/TextData response text, WriteTextAsset content, script and scene text, entity/asset names, Hello clientName/token/user, Error messages.

### What remains (small, optional)
Nothing needs the originally requested change. Optional cleanups only:
1. Correct the progress log: remove/replace line 145's "Open: possible engine generator bug" (it was resolved 2026-10-09, #104/#24).
2. Optional: host-launcher.ts decoders could use ignoreBOM for symmetry (not needed; they decode process output).
3. If wanted, a test vector for a lone-BOM-prefixed string in test-vectors.json already exists, so no new vector is needed.

Proposed orchestrator item: none (do not create the "generator change + codec test + frontend sync" item; it is done).

## JOB 2: engine issue #108 (native TLS, remote editing phase 2)

Caveat: I could not read the issue body (no GitHub access). The scope below comes from the progress log ("self-signed cert with fingerprint pinning") and the ledger notes. Compare against the issue's checklist before closing.

### Delivered and merged
- Engine PR #112 (merge c0eee5e, 2026-10-10, user-merged by hand because ratify's secret scan false-positives on vendored Mbed TLS): vendored Mbed TLS 3.6.7 (26ff6d6), `--tls` / N2ENGINE_EDITOR_TLS, off by default (0693321), self-signed identity per user in <UserDataBase()>/editor-tls (cert valid 2024-2074, race-safe creation, key 0600/dir 0700), fingerprint in the ready line, thread-safe Mbed TLS, tests and CI incl. a TLS-ON leg (be5fcf4), docs on the trust model and what TLS does not give (7f90d34, 021d08d, 3848de9, 1ae584f).
- Frontend PR #26 (2026-10-10): fingerprint pinning via Node tls (rejectUnauthorized false + fingerprint256 compare), TOFU pin store (pin saved only after a successful Hello, never auto-trust), direct-address remote beside the SSH tunnel, plaintext only for loopback, Inspect and Forget pin, handshake timeout.
- Follow-ons that cover the items #112/#26 notes listed as "deferred hardening": per-user tokens and read-only viewer role (#130, protocol 1.12.0), rate limiting (#132), Linux host/Docker/systemd docs and CI (#113-#117, #133), Linux POSIX TLS paths exercised by CI (TLS smoke on Clang legs).

### Still missing or unverified
- Real-world verification only: a real `N2EditorHost --tls` on a real cloud host over a non-loopback network, first-connect fingerprint confirmation, Forget pin, changed-fingerprint refusal, WAN throughput (user-verification-checklist.md line 23).
- Windows ACLs on the TLS key file never checked (checklist line 24; permission check is POSIX only).
- The direct-remote form was never run in Electron (typechecked/bundled only; checklist line 29; DOM tests #27/#30 cover other panels).
- No CA-signed or user-supplied certificate option (self-signed with pinning is what was filed; cert rotation = delete the two files).
- SIGHUP reload and arm64 image build listed as unverified in docs.
These are verification gaps, not missing features.

### Recommendation
Close #108 as completed (engine #112 + frontend #26, plus hardening #130/#132), after the user glances at the issue's own checklist for anything not in this list. If the user prefers issues to track verification, keep a new small issue "Verify TLS against a real cloud host" (or the checklist item) and close #108. Not closed by me.
