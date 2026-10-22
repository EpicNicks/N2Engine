Accepted by the user 2026-10-13:
- Player passes isHeadless=true (engine windowless software path) instead of changing Window::UsesNoWindow, which WindowlessTests pin; audio loopback init is attempted and refused by the stub, a warning in the log.
- Application::Run split into BeginRun/RunFrame/EndRun (Run is exactly those) rather than a web-only loop copy; touches native code path with identical statements.
- Real GLFW replacement not done: input via KeySource/InjectPointer/AccumulateScroll from html5.h callbacks; GLFW stays the stub.
- Render size capped at 1280 wide (browser scales the canvas), devicePixelRatio ignored; a judgement call.
- Player linked with MODULARIZE (createN2Player factory) so the page and the node smoke can pass settings.
- web-size-report.sh also lists .data and .html (not counted in budgets); existing CI step names unchanged, web_tests added to the Build target list and three steps added.
- No golden image for the web render (scene is clock-animated): smoke asserts frame count, size and non-blank only.
- No native unit test of RunFrame/Quit: Application::Quit flag persists in the test process (CoreRobustnessTests), covered by the web smoke and native Run users.
- Review fixes: PresentFrame forces alpha 255 before putImageData; mouse presses latched until the next frame samples them
