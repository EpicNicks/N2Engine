-- The GPU smoke test's stations: where the camera goes for each (keys 1-7, in CameraController.lua) and what the HUD
-- says to look for there (SmokeHud.lua). Every camera looks straight down -Z. The cameras are placed so each station
-- sits in the top four fifths of a 16:9 (or 4:3) window, clear of the HUD in the bottom-left corner. The screen-space
-- UI panel, which covers the right of the window, is shown only at station 7 (or with U).
-- require("scripts.SmokeStations") returns the list; the station on key i is entry i.

return {
    {
        name = "Physics",
        camera = Vector3(-3.5, 0.0, 20.0),
        look = "The blue cube and the red sphere fall onto the grey floor and come to rest; the sphere rolls off "
            .. "the cube's edge. Nothing passes through the floor.",
    },
    {
        name = "World text",
        camera = Vector3(-30.8, -0.6, 12.0),
        look = "TextRenderer through the SDF text shader: white text at 1.0, 0.5 and 0.25 units per em, all crisp "
            .. "with smooth edges, no boxes or stray marks around the glyphs; the blue paragraph wraps onto "
            .. "several centred lines.",
    },
    {
        name = "Text effects",
        camera = Vector3(-60.6, -0.6, 13.5),
        look = "On the grey board: a red outline, a hard black drop shadow, a cyan glow, blurred edges and all of "
            .. "them combined, each as its caption says. No rectangles around glyphs. Glow and softness need "
            .. "OpenGL; the software renderer draws them hard-edged.",
    },
    {
        name = "World canvas",
        camera = Vector3(30.0, -1.0, 9.0),
        look = "A see-through blue panel in the world with a title, the arrow icon and an orange button. Hover "
            .. "the button: darker; hold the left button: darker still; release over it: World clicks +1. The red "
            .. "cube behind shows dimly through the panel; the green cube in front hides its lower-left corner.",
    },
    {
        name = "Materials",
        camera = Vector3(59.4, -0.5, 12.0),
        look = "Lit from the front upper left. Top: a spinning checker cube (red top-left on each face; faces "
            .. "toward you and the upper left bright, faces turned away to the right dim), a lit white sphere "
            .. "(bright upper left, darker lower right, a highlight) and a flat unlit white disc. Bottom: one "
            .. "striped texture opaque, blended and alpha-tested in front of a yellow bar.",
    },
    {
        name = "glTF model",
        camera = Vector3(89.75, -0.2, 9.0),
        look = "Two robots from smoke_robot.gltf, lit from the front upper left: a grey body with a bright face "
            .. "(cyan eyes above a red mouth, upright), orange arms (the right one tilted out), a teal pyramid head "
            .. "and an orange antenna. The right one spins as one piece: nothing comes apart.",
    },
    {
        name = "Screen UI",
        camera = Vector3(-3.5, 0.0, 20.0),
        uiPanel = true,
        look = "The panel on the right: the arrow icon upright, labels at left-top, centre and right-bottom of "
            .. "their boxes, the sentence wrapped inside its box. Click me: darker on hover, darker still pressed, "
            .. "Clicks +1 on release; Disable it greys it out. The world shows through the dark panel.",
    },
}
