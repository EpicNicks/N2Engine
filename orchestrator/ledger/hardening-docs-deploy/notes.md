Accepted by the user 2026-10-13:
- Tokens-file unit is a NEW file deploy/n2editorhost-tokens.service (static user) beside the unchanged-in-behaviour DynamicUser --token-env unit, since the two modes need different users
- Dockerfile changed by comments only; no image behaviour change
- Release host prints no log lines (no console subscriber): audit/rate-limit/reload lines are visible only via PollEvents, not journal or docker logs; documented as 'no durable audit trail' and listed as not covered rather than fixed (needs C++)
- CI tokens-file step lives in the docker job only; it checks revocation by rewriting the file in place (next-connection re-stat, asserting the Hello refusal text); SIGHUP is NOT tested (host is PID 1, kernel drops unhandled signals)
- Compose secret uid/gid/mode documented with a 'check it applied, else use the bind mount' caveat
