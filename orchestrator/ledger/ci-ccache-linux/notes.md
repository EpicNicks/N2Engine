Accepted by the user 2026-10-13:
- Cache save step also requires event_name == push (workflow_dispatch on master does not save)
- Key suffix is run_id-run_attempt; ccache --zero-stats after restore so the summary shows this run's hit rate
- Added a Reset ccache statistics step that also prints ccache config (grep now matches origin-prefixed lines and never fails)
- Docs now state old ccache entries are not deleted; total is quota-filling via LRU
