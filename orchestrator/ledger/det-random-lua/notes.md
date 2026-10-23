Accepted by the user 2026-10-13:
- luai_makeseed/randseed overridden by two marked patches calling a new external/lua/n2seed.{h,c} switch (not a compile definition: the macro cannot reach math.random's randseed and the static default must remain for the unset case)
- THIRD_PARTY_NOTICES.md updated with the Lua local patches (user decision 2026-10-16)
- Application::Init now loads the project file before LuaRuntime::Instance() (block moved up, otherwise unchanged) so the seed is read before the first lua_State
- Two LuaRuntime instances are impossible (singleton), so the two-state tests use plain sol::states with the engine's libraries; the singleton is covered via Random.Seed
- Seed is Random.Seed(n)/Math::Random::Seed(n) only; no project setting seeds engine Random (plan names only determinism.luaSeed); Random::Unseed()/IsSeeded() added for tests
- Init logs a warning whenever determinism.luaSeed is set (security note)
