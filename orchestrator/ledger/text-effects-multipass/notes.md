# Notes for the next run (text-effects-multipass), 2026-10-10
- Renderer: passes are drawn through the shadow uniforms with face alpha 0; NO renderer/ or shader change was made even though renderer/ was allowed. The user asked for this to be recorded. A cleaner shader-level pass (and in-front passes, order > 0) needs a renderer change that cannot be verified in CI (no GPU): do it only with the user able to run the GPU smoke scene.
- Deferred: negative width (inner outline, inset stroke, hollow text, engraving) and orders above 0; see docs/text.html "Known limits and next steps".
- Inspector: the pass list shows as raw Json (no per-pass widgets); a follow-up for real widgets is worth filing.
- Unverified on a GPU: the OpenGL multi-draw pass path and the order sort.
