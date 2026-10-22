Accepted by the user 2026-10-13:
- S-3/S-4 had already documented flags, busy/preemption/takeover, protocol 1.13.0, keepalive and proxy behaviour, so only stale claims, a duplicate row, Docker and sizing notes were added
- deploy/n2editorhost.service and n2editorhost-tokens.service: comments only, the removed shared loopback backoff replaced by 'no loopback backoff; bounded by --preauth-slots and the handshake rate'
- Historical design callouts (PR #130, #132, #153, #168) kept as records, each given a short 'since PR #176/#177' note instead of a rewrite
- Removed the unverified '150 KiB per slot' estimate from the sizing bullet; Docker text now says only that the peer address depends on the network mode and peer= in the log shows it
- Re-review round: also corrected sibling stale phrasings (Shutdown answered on the session thread, handshake deadline counted from when the worker takes the connection); Host.cpp code comments untouched
