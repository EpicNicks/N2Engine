# Notes (linux-docs-deploy, 2026-10-11)
Approved deviations (accepted by the user):
- Added a non-required CI job 'docker (editor host image, x86_64)' that builds the image and waits for the tls=1 ready line (full recompile, about 9 min, not cheap)
- Dockerfile default CMD binds 0.0.0.0 (a container needs it) with --tls and --token-env N2_EDITOR_TOKEN, so the host refuses to start without a token; publish on the Docker host loopback
- Used deploy/Dockerfile.dockerignore because a root .dockerignore is outside the allowed paths
- Shared libopenal.so is copied into the runtime image (found by the ldd check in CI)
- systemd unit uses EnvironmentFile for the token (host reads an env var, so LoadCredential is not used) and DynamicUser with XDG_DATA_HOME=/var/lib/n2engine, so the TLS identity is in /var/lib/n2engine/n2engine/editor-tls
- Updated the stale 'Next: Linux' callout in docs/logging-and-editor.html (review finding; page is under docs/)
- systemd unit: libopenal.so* installed to /opt/n2engine/lib and found via LD_LIBRARY_PATH
- Docker example uses --env-file token.env instead of -e on the command line
Unverified: arm64 image build; systemd unit on a real machine; real remote editor over an SSH tunnel; fix commit not CI-run yet (docs and unit comments only)
