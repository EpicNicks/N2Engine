# Notes (linux-runtime-fixes, 2026-10-10)
Approved deviations (accepted by the user):
- Linux user data folder moved from $HOME/.n2engine to $XDG_DATA_HOME/n2engine else $HOME/.local/share/n2engine, per brief; no migration of old folder
- Empty or relative XDG_DATA_HOME/HOME treated as unset (XDG spec); with none usable UserDataBase still returns '.' and TLS DefaultDirectory refuses it (existing behaviour kept, documented)
- Added: failure to chmod a new TLS identity dir to owner-only is now an error
- Dropped-client test queues 400 small requests, resets/closes before the host main loop answers any, then drains so every answer meets a dead connection; best-effort SIGPIPE reproducer (limits stated in a code comment)
Unverified: runs on a real Linux host; whether the dropped-client test fails with the fix reverted (check once the Linux CI leg exists).
