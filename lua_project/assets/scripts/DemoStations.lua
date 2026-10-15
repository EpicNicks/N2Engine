-- The demo stations that follow the smoke test's seven (SmokeStations.lua): where the camera goes for each (keys 8, 9,
-- 0 and Minus, in CameraController.lua) and what the HUD says there (SmokeHud.lua). They are a separate list so the smoke
-- test keeps its seven stations ("Station 1/7") and its checklist (docs/testing.html). Every camera looks straight
-- down -Z, placed so the station sits in the top four fifths of the window, clear of the HUD.
-- require("scripts.DemoStations") returns the list; entry i is on the key named by its `key` field.

return {
    {
        name = "Particles",
        key = "Key8",
        camera = Vector3(120.0, 1.6, 16.0),
        look = "Three ParticleSystems: a blue water fountain, a sand burst that lands and lies on the ground, a grey "
            .. "smoke column that rises. Each sign says which backend it asked for and which is RUNNING (CPU, or PhysX "
            .. "GPU) and its live particle count. B: burst. P: play / stop. C: clear. K: backend Auto, CPU, PhysX. "
            .. "PhysX with no GPU particle backend runs on the CPU, and the sign says so.",
    },
    {
        name = "World canvas and colliders",
        key = "Key9",
        camera = Vector3(150.0, -1.0, 9.0),
        look = "A world-space panel with two buttons. Button A has a yellow trigger volume in front of it: it still "
            .. "clicks, because triggers never block a world canvas. Button B has a solid green block in front: "
            .. "clicking it hits the block, so its counter stays 0. Hover: darker. Hold: darker still.",
    },
    {
        name = "Text effect passes",
        key = "Key0",
        camera = Vector3(180.0, -0.6, 14.5),
        look = "TextRenderer:AddEffectPass on a grey board: an outline, a shadow and a glow, each one pass; then the "
            .. "same yellow and red passes twice. Before: added order puts red in front and hides the yellow. "
            .. "After: explicit orders bring the yellow back in front. Last: a pass at order -300 behind the "
            .. "settings shadow. The glow needs OpenGL.",
    },
    {
        name = "Soft bodies",
        key = "Minus",
        camera = Vector3(210.0, 1.6, 19.0),
        look = "SoftBody: cubes of 1e4, 1e5 and 1e6 Pa (soft squashes most), two spheres (damping 0.1, 3) and a "
            .. "robot body drop and wobble. Each sign says which backend it asked for and which is RUNNING (CPU or "
            .. "PhysX GPU). R: drop again. J: backend Auto, CPU, PhysX. V: push up. PhysX with no GPU soft-body "
            .. "backend runs on the CPU, and the sign says so.",
    },
}
