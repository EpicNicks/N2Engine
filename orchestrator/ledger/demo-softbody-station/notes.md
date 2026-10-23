Accepted by the user 2026-10-13:
- Robot: used the robot's Body mesh and Face material on its own object (smoke_robot.gltf#mesh/Body, #material/Face) since the Body node has child nodes that would not follow a soft body; no change to make_smoke_assets.py or the gltf
- Added five hand-written lit colour materials (softbody_*.mat) so the cube/sphere variants are told apart; not generated assets
- No tet count shown (no Lua getter): signs show the resolution instead
- Keys: station Minus (Demo Station 4), R drop again, J backend cycle, V push up (all checked free; Minus is in the Key enum table)
- HUD keys line now reads 'Keys 1-7, 8 9 0 -: stations' and comments say Demo N/4
- Added one headless test (SoftBodyStationBodiesStandOnAFloorWithAStaticCollider) besides the BehaviourScriptsLoad entry
- Vulkan checked: draws nothing yet (stage A only uploads meshes), docs say so
- A body with no backend for over 1 s shows 'inert', before that 'starting'
- Fix round 2: the two raw-newline sign strings in scene.lua (syntax error behind every LuaProjectSceneTest failure) now use 
 escapes
