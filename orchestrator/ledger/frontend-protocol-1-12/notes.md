Accepted by the user 2026-10-13:
- Hello omits the user (payload cut by the 4-byte empty-string prefix) when none is set, instead of always sending the generated empty string, so 1.11 hosts see the old payload
- Direct form does not remember the user: the direct mode has no recent list (only pins), so only SSH recent remotes store it
- Empty role is treated as admin; an unknown role is treated as viewer (safe side) - not specified in the plan
- User badge is hidden when the host names no user (user empty), e.g. no tokens file
- No admin-class action is exposed for remote hosts (Stop host is local-only, launched admin token), so nothing gated for editor role
- Viewer Play disablement is covered via the shared hostReadOnly guard feeding canStart/canPlay, not a DOM test of the Play button
- User name validated to A-Za-z0-9._@- max 64 (the engine's --new-token charset); new file src/main/editor-user.ts
