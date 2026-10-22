Accepted by the user 2026-10-13:
- Concurrency group uses the run id for non-PR events (not github.ref) so master/dispatch runs are never cancelled nor replaced as pending
- New job named 'changes (docs-only check)'; protocol job left unconditional and without needs
- Empty diff is treated as not docs-only
- Windows matrix job is not skipped at job level (that reports one unexpanded-name skipped check); it runs and each of its 9 steps skips itself on docs-only, so the 3 leg names are kept; linux x3 and docker still use job-level if
- Run lists 9 check-runs (8 legs + changes); accepted, documented
- Docs no longer claim tag runs (Tests has no tag trigger)
