# Task sec-bind-fail-closed: Editor host security S3: fail closed on a non-loopback --bind without token + TLS; explicit --insecure-bind (audit F5)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: sec-bind-fail-closed.

## Issue / plan
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

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^tests/
- ^docs/
- ^deploy/
- ^\.github/workflows/tests\.yml$

## Acceptance
- All Tests-workflow jobs green (8 checks; required linux (Release, x86_64, Clang)), the docker job included
- A non-loopback bind without (token AND --tls) exits 1 with a clear message before the engine starts, unless --insecure-bind is given; loopback binds unchanged
- HostOptionsTests cover every combination (no token, token without TLS for each token kind, TLS without token, both, --insecure-bind, --play, --create/--new-token, loopback variants)
- Every docs/deploy/Docker/CI example that exposes the host is consistent with the rule; --insecure-bind used only where genuinely needed, with a reason
- No protocol change
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
User-approved 2026-10-15 but gated: the orchestrator flips approved to true once sec-host-dos-limits is merged. Security-sensitive (audit F5). The reviewer must try to break it: (1) find a spelling of a non-loopback address that is treated as loopback (for example 127.0.0.1 followed by junk, leading zeros, 0.0.0.0, an empty string, or 'localhost', which Start does not accept today), and any way to reach the serving path without the check (an argument order, --play, an option given twice); (2) check that the refusal happens before the engine and Lua start and never prints a token; (3) check that --insecure-bind keeps the old warnings; (4) check that no CI, Docker or deploy example quietly gained --insecure-bind where --tls and a token would do; (5) the existing HostOptionsTests that used a bare --bind 0.0.0.0 must have been updated deliberately, not deleted. Expected finding: no change to tests.yml is needed, because every serving docker run already passes --tls and a token. Unverified: operators' own scripts outside the repo that relied on the warning-only behaviour; the release notes should call this a breaking change for them.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs sec-bind-fail-closed` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref sec-bind-fail-closed` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/sec-bind-fail-closed/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head sec-bind-fail-closed --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/sec-bind-fail-closed/dev.json: {"pr":<number>,"branch":"sec-bind-fail-closed","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/sec-bind-fail-closed` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
