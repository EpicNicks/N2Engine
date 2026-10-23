Accepted by the user 2026-10-13:
- Per-user backoff is keyed on any syntactically valid user name in Hello (tokens-file hosts only), not only names present in the file, so a refusal is not an oracle for which users exist
- Address threshold: 2 free failures, so the 3rd failed Hello starts the 1 s backoff (doubling to 5 min)
- Loopback global rule: every 10 failures inside a 1 min window is one burst; backoff 1 s doubling per burst to 5 min; a loopback success deliberately does NOT clear it; loopback gets no 2 s prior-offender budget
- Only a Hello refused for its token counts; protocol-version refusals, pre-Hello idling/commands and refused-user Hellos are not counted (documented)
- A Hello naming a backed-off user gets a plain Error 'Too many failed attempts: try again later' and the connection closes (no new error code: no protocol change)
- peer=<addr> is also appended to the connect, disconnect, TLS-failure and pre-Hello-close lines, not only Hello and audit lines; no 'Waiting for editor connection' line after a connection closed at accept
- Added public EditorServer setters SetRateLimiting/SetRateLimitPolicy/SetRateLimitClock/SetPeerAddressProvider and RateLimitedConnections() as test seams; --no-rate-limit added to HostOptions and usage; Host logs rate limiting on/off
- docs/editor.html (Linux section) edited besides docs/logging-and-editor.html; deploy/Docker/systemd untouched
