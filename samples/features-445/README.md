# features-445

A small Lua project that uses the features of issue #445 together in one scene:

| Feature | Where in `assets/scripts/Director.lua` |
|---|---|
| RectTransform rotation and scale, a turned mask | the turned panel |
| Canvas scaler (laid out for 400 x 300) | the HUD canvas |
| Text overflow (ellipsis) and rich text | the title and the line under the menu |
| Keyboard and gamepad UI navigation | the three buttons |
| Post-processing: blur, lens distortion, wave | the `Effects` object |
| A user shader with its software-renderer stand-in | the `Lamp` sprite and `assets/shaders/pulse.n2shader` |
| 2D soft body | the `Jelly` object |
| Per-particle collision | the `Sparks` object |

`assets/main.lua` runs in an empty scene. After 120 frames the script checks what happened and shows the result.

## Running it headless

`Features445SampleTest` (`tests/engine/scripting/Features445SampleTests.cpp`) runs the project with the software renderer,
no window and a fixed 60 frames a second, fails on any failed check or logged error (but for the one notice that this renderer draws the shader's
stand-in), and looks at a few pixels:

```sh
./bin/engine_tests --gtest_filter='Features445SampleTest.*'
```

It needs the UI, particle and Box2D modules. How it looks with a GPU is not covered by any test.
