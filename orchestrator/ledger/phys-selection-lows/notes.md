Accepted by the user 2026-10-13:
- Default rule changed: AUTO/unset selects PhysX if built, else Jolt, else none; a Jolt-only build now runs Jolt for a project without physics.backend via fallback from the hosts' PHYSX request (warning names the host option)
- Hosts (editor-server, lua_project, web player, test_project) still pass PHYSX and are outside allowed paths
- Non-Deterministic project whose request is missing with no other backend built runs without physics (error, Physics row Disabled), app still starts
- netcode.html has no Deterministic requirement list; the requirement table lives in physics.html, so netcode.html is unchanged
