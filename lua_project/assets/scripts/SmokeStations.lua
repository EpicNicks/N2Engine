-- The GPU smoke test's stations: where each one is built (scene.lua), where the camera goes for it (keys 1-6, in
-- CameraController.lua) and what the HUD says to look for (SmokeHud.lua). Every station faces +Z, seen from a
-- camera on its +Z side looking down -Z; the camera sits a little right of the centre, so the station clears the
-- screen-space UI panel on the right of the window.
-- require("scripts.SmokeStations") returns the list; the station on key i is entry i.

return {
    {
        name = "Physics",
        centre = Vector3(0.0, 0.0, 0.0),
        camera = Vector3(1.5, 1.0, 16.0),
        look = "The blue cube and the red sphere fall onto the grey floor and come to rest; the sphere rolls off "
            .. "the cube's edge. Nothing passes through the floor.",
    },
    {
        name = "World text",
        centre = Vector3(-30.0, 0.0, 0.0),
        camera = Vector3(-28.5, 0.0, 10.0),
        look = "TextRenderer through the SDF text shader: white text at 1.0, 0.5 and 0.25 units per em, all crisp "
            .. "with smooth edges, no boxes or stray marks around the glyphs; the blue paragraph wraps onto "
            .. "several centred lines.",
    },
    {
        name = "Text effects",
        centre = Vector3(-60.0, 0.0, 0.0),
        camera = Vector3(-58.5, 0.0, 10.0),
        look = "On the grey board: a red outline, a hard black drop shadow, a cyan glow, blurred edges and all of "
            .. "them combined, each as its caption says. No rectangles around glyphs. Glow and softness need "
            .. "OpenGL; the software renderer draws them hard-edged.",
    },
    {
        name = "World canvas",
        centre = Vector3(30.0, 0.0, 0.0),
        camera = Vector3(31.5, 0.0, 8.0),
        look = "A see-through blue panel in the world with a title, the arrow icon and an orange button. Hover "
            .. "the button: darker; hold the left button: darker still; release over it: World clicks +1. The red "
            .. "cube behind shows dimly through the panel; the green cube in front hides its lower-left corner.",
    },
    {
        name = "Materials",
        centre = Vector3(60.0, 0.0, 0.0),
        camera = Vector3(61.5, 0.5, 9.0),
        look = "Top: a spinning lit cube with the checker texture (red top-left on each face), a lit white sphere "
            .. "(bright upper left, dark lower right, a highlight) and an unlit white disc. Bottom: the same "
            .. "striped texture opaque, blended and alpha-tested in front of a yellow bar.",
    },
    {
        name = "glTF model",
        centre = Vector3(90.0, 0.0, 0.0),
        camera = Vector3(91.5, 0.5, 7.0),
        look = "Two robots from smoke_robot.gltf: a grey body with a face (cyan eyes above a red mouth, upright), "
            .. "orange arms (the right one tilted out), a teal pyramid head and an orange antenna. The right one "
            .. "spins as one piece: nothing comes apart.",
    },
}
