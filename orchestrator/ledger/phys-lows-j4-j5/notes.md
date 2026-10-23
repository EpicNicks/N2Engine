Accepted by the user 2026-10-13:
- PhysX single-hit queries now run ONE touching query and fall back to blocking+bounded second query only when the 1024 buffer fills (results same unless a nearest hit is not a live collider: old code tied around that hit, new picks among located hits)
- No-backend log level done in Application.cpp (Warn when refused, no backend compiled in, preset not Deterministic); ChooseBackend/refused flag and HealthTests/strings unchanged
- RaycastAll order via new QueryUtil::SortByPick (repeated PickClosest, O(n^2) over <=1024 hits); DistanceThenHandleLess removed
- Scratch hit buffer is thread_local per hit type in CollectHits
- J4 trigger test is a slow kinematic approach; it does not exercise the key-change (shape rebuild) case directly
