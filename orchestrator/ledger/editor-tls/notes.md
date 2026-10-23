# Notes (editor-tls, 2026-10-10)
- NEXT: Linux host build. The user wants it next (cloud hosts are Linux). The editor host builds and runs on Windows only today: EditorServer uses Winsock names and int sockets, and the POSIX branches of EditorTls.cpp (socket shims, 0700/0600 permissions) have never been compiled or run. The TLS layer is meant to be platform-neutral so that work does not redo it. Documented in docs/logging-and-editor.html.
- Deferred hardening (documented in docs): per-user tokens, rate limiting, a read-only mode; WAN throughput unmeasured.
- Frontend half still to do (needs this merged): parseReadyLine returns {port, fingerprint}; Node tls pinning (rejectUnauthorized false, compare getPeerCertificate().fingerprint256 normalised to lowercase hex without colons); a TOFU pin store like recent-remotes.ts; a direct-address remote option beside the SSH tunnel.
- One identity per user: <UserDataBase()>/editor-tls, cert validity 2024-2074. Delete both files to reset.
- Unverified: a real cloud host, non-loopback networks, WAN throughput, hard-link publish on filesystems without hard links, Windows ACLs on the key file.
- Ratify note: the secret scan false-positives on the vendored mbedTLS PEM macros in external/; the user was offered a patch to skip external/ (denied for me by the classifier, the user may apply it).
