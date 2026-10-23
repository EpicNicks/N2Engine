Accepted by the user 2026-10-13:
- --log-level debug is refused with an error because the Logger has no Debug level
- Hello claimed user is logged as user=- unless it passes IsValidTokenUserName; four client-text log calls now go through SanitizeForLog
- Sink escapes control characters so client text cannot forge lines
- SIGUSR1 reopen not implemented (documented)
- Fix after the first CI run (local diagnosis, logs unreadable): a Hello whose user field equals its token logs user=- (a 64-hex generated token is also a valid user name); a client that puts a different valid-looking secret in the user field is still logged as the claimed user (known gap, policy decision pending)
- USER DECISION 2026-10-14 after the review (high: rate-limit line could log a token): the claimed user is logged ONLY when it is a user in the current tokens table, else user=?; user=- under --token-env or no authentication; same rule for the per-user rate-limit line; unknown-user test updated to expect user=?. Added a startup WARN when an existing --log-file is group/other readable.
