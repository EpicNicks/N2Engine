Accepted by the user 2026-10-13:
- On busy, ProjectSession rethrows the HostBusyError itself (no 'Couldn't connect to' prefix) and sets host state failed with message null so no console error line per retry
- SSH path: busy is passed through only when the tunnel did not exit; the tunnel is still killed on each busy attempt
