Accepted by the user 2026-10-13:
- ServiceTokens NOT moved to the accept loop: with one thread a SIGHUP revoking an open session would no longer end it (EditorTokensTests); it stays on the wait path via ConnectionEnv::beforeWait, comment in EditorServer.hpp says why
- _helloTokenRefused/_helloVersionRefused moved into Connection but reached from HandleHello (main thread) through a new atomic EditorServer::_activeConnection pointer (set only while HandleClient runs; null for commands run without a socket)
- Accept-loop wait uses a short-lived Connection around the listening socket (never closed by it) so Connection::WaitUntilReady is the only wait implementation
- EINTR on the wait still fails the wait (select and poll both return -1): kept identical; flagged in the PR as a latent issue to decide before S-3
- POLLNVAL mapped to wait failure to match select erroring on a bad descriptor
- No existing test edited; docs: logging-and-editor.html (member names, select to poll, new design callout, PR #168) and testing.html (new test file row); EditorTls.hpp comment only
