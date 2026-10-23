# Review sec-bind-fail-closed: Editor host security S3: fail closed on a non-loopback --bind without token + TLS; explicit --insecure-bind (audit F5)
Read-only. Do NOT edit files, push, or comment on GitHub. Repo C:/Method Notes Obsidian/N2Engine. First `git fetch`, then review `git diff origin/master...origin/sec-bind-fail-closed` (read C:/Method Notes Obsidian/orchestrator/ledger/sec-bind-fail-closed/dev.json for the PR number and the dev's deviations; if C:/Method Notes Obsidian/orchestrator/ledger/sec-bind-fail-closed/review.json exists, it holds your predecessor's findings: check each is fixed and review only the delta since its "head").
Use the gh wrapper for any GitHub read: `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" ...` from inside C:/Method Notes Obsidian/N2Engine.

## Plan under review
Source: the security audit at C:/Method Notes Obsidian/orchestrator/proposals/editor-host-security-audit-2026-10-15.md. Read finding F5 and section 6 item 7 first. The user APPROVED fail-closed on 2026-10-15. This item is gated on sec-host-dos-limits being merged, because both edit HostOptions, Host.cpp and the editor docs. Line numbers below are from origin/master at a1aa1005; find the code again at current master, after sec-host-dos-limits.

THREAT CLOSED (F5, Medium: a misconfiguration, then the unauthenticated attacker A1).
- Today a non-loopback `--bind` only WARNS (editor-server/src/Host.cpp ~478-500, the `options.bindAddress != EditorServer::DefaultBindAddress` block):
  - with no token: 'anything that can reach it can control this host';
  - with a plaintext token: 'the access token isn't encrypted'.
- EditorServer::Start (EditorServer.cpp ~1518-1525) accepts any IPv4 address, 0.0.0.0 included.
- An operator who runs `N2EditorHost --bind 0.0.0.0 --project ...` therefore gives every peer the admin role, with no Hello needed. That includes DeleteScene, WriteTextAsset, Lua execution on the host (F3) and Shutdown.
- An operator who overrides the Docker CMD and drops --token-env gets the same open host.
- A plaintext token on a network can be sniffed.

SCOPE.
(1) Rule. When the host is going to serve (not --create, --new-token or --help), a bind address that is not loopback is REFUSED at startup unless both of these hold:
  (a) a token is configured (--token-env or --tokens-file), AND --tls is given;
  (b) OR the new flag `--insecure-bind` is given explicitly.
- On refusal: one clear stderr line naming the address, what is missing (token, TLS or both) and the two ways out (add --tls with --token-env or --tokens-file, or pass --insecure-bind to accept the risk). Exit code 1, before the engine starts, like the existing --tls and --tokens-file refusals at Host.cpp ~213-230.
- With --insecure-bind, keep today's warnings unchanged (they still describe the risk), plus one line saying --insecure-bind was given.
- Loopback binds are unchanged: the default 127.0.0.1, any 127.0.0.0/8 address, and nothing else. Only IPv4 is accepted today.
- Decide loopback by parsing the address the way Start does, and reuse PeerAddress::IsLoopback from RateLimiter.hpp if convenient. An address that does not parse counts as non-loopback, and Start still rejects it as today.
- A TLS-less build with --tls is already refused (Host.cpp ~225), so a non-loopback bind on such a build needs --insecure-bind. Say so in the message or docs.
- Put the rule in ParseHostArguments (HostOptions.cpp ~225-265, next to the other combination checks) so it is unit-testable as a pure function. Host.cpp then just reports the error. If the rule needs runtime facts that parsing cannot know (it should not: it only needs which flags were given), explain where you put it.
- `--play` with a non-loopback bind: a play host is always plaintext (help text ~370-376), so it is refused by the rule unless --insecure-bind is given. Add no separate --play rule; just test and document that outcome.
- Add `--insecure-bind` to HostUsage (the --bind help text ~341-344 must describe the new rule) and to the flags table in docs/logging-and-editor.html (~215).

(2) Examples, deploy and CI. Read every place that runs the host with a non-loopback bind and keep it working. Pass --insecure-bind ONLY where it is genuinely needed, and say why in a comment.
What I found at a1aa1005 (re-verify):
- deploy/Dockerfile CMD (~76) is `--bind 0.0.0.0 ... --tls --token-env N2_EDITOR_TOKEN`, which passes the rule. The comment's tokens-file example (~71) also has --tls, so it passes.
- deploy/n2editorhost.service and deploy/n2editorhost-tokens.service pass no --bind (loopback), so they pass. Update their 'To listen elsewhere add --bind' comments (~20-23, ~46-49) to say --tls and a token are now required.
- .github/workflows/tests.yml, the docker job (~337-400): every serving `docker run` uses `--bind 0.0.0.0` with --tls and a token (the default CMD with N2_EDITOR_TOKEN, and the tokens-file runs). The `--create` and `--new-token` runs do not serve. So tests.yml should need NO change. Only edit it if CI proves otherwise, and then say why.
- tests/smoke/editor_host_smoke.py passes no --bind (loopback), so it passes.
- tests/editor-server/HostOptionsTests.cpp ~78 parses `--bind 0.0.0.0` with no token and no TLS, and ~447 parses `--tls --bind 0.0.0.0` with no token. These tests will now fail, so update them deliberately: add --insecure-bind, or a token plus --tls, keeping what each test was checking.
- Docs: docs/editor.html (~896-957: the Binding, Firewall and Docker bullets, and the exposed-host examples), docs/logging-and-editor.html (~215 the --bind row, ~892 the no-auth paragraph, ~905 the warning bullet, ~937 the design-loopback text) and docs/index.html (~205 the design-loopback row). Each must state the new rule. Any example that exposes the host without TLS must either gain --tls and a token, or show --insecure-bind with a sentence on why that is unsafe.

TESTS (tests/editor-server/HostOptionsTests.cpp, one test per combination). For a non-loopback bind (0.0.0.0 and 192.0.2.10):
- no token, no TLS: refused;
- --token-env without --tls: refused;
- --tokens-file without --tls: refused;
- --tls without a token: refused;
- --tls with --token-env: accepted;
- --tls with --tokens-file: accepted;
- any of the refused combinations plus --insecure-bind: accepted;
- --play with a non-loopback bind: refused, and accepted with --insecure-bind.
Also:
- loopback (default, 127.0.0.1, 127.1.2.3) with no token and no TLS: accepted;
- --create, --new-token and --help with a non-loopback --bind are not refused by this rule (they do not serve);
- an unparsable bind address follows the non-loopback rule;
- the error text names the missing pieces and --insecure-bind;
- --insecure-bind takes no value.
If there is a Host-level test harness that runs the startup path, add one exit-code-1 case. Otherwise the smoke test may gain one quick case: run the real host with `--bind 0.0.0.0` and nothing else, and require exit code 1 plus the message without the engine starting. Keep it fast.

Docs: docs/testing.html rows for the new tests.

OUT OF SCOPE: no protocol change; no frontend change; no IPv6 bind support; no other deploy hardening (audit F17).

CI. Nothing builds locally; CI is the build. There are 8 checks, and the required one is `linux (Release, x86_64, Clang)`. The docker job must stay green, and it exercises the real CMD.

## Look for
Correctness bugs and concrete failure scenarios; security (injection, secrets); missing tests / test sources that won't compile (missing includes) / non-ASCII in tests; anything outside the allowed paths; docs out of sync; GL/GPU code CI can't run (say so); regressions. Say nothing about style.

## Output
Overwrite C:/Method Notes Obsidian/orchestrator/ledger/sec-bind-fail-closed/review.json: {"head":"<origin/sec-bind-fail-closed sha you reviewed>","findings":[{"severity":"blocker|high|medium|low","file":"","line":0,"summary":"one line","scenario":"one line"}],"verdict":"clean|fixes-needed"}. At most 10 findings, most severe first, only ones you are confident in. Then return the same counts to your caller.
Rules: no attribution anywhere; no tokens in env or commands.
