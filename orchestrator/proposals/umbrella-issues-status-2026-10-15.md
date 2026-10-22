# Umbrella issues status: N2Engine #3 and #6 (2026-10-15)

Nothing was closed, edited or commented on. GitHub was read 3 times (issue 3 body, issue 6 body, issue 62 body and state). Everything else comes from local sources: `git log origin/master` (fetched; 861 commits), docs/meshes.html and docs/editor.html, the progress log, mesh-plan-issue3.md, editor-plan-issue6.md, and the orchestrator proposals.

Note: the bodies of #3 and #6 are one-liners with no checklist. The promised items come from the approved plans (proposal plus Decisions comments) and their sub-issue lists, as recorded in the memory files. I did not re-read those comments on GitHub (request budget), so the sub-issue list is taken from memory and the progress log.

## #3 Mesh rendering and 3D model import

Body: "Add full mesh rendering and import support for 3D models such as blender."

| Item | Sub-issue | Status | PR |
|---|---|---|---|
| Camera::LookAt fix | #57 | DONE | #63 |
| P1: stb_image, assetimport target, Texture asset, GpuCache, SW bilinear, Image sprite | #58 | DONE | #64 |
| P2: Mesh/Material assets, MeshRenderer, IndexRange draw, primitives rebuilt | #59 | DONE | #65 |
| P3: glTF import (cgltf), Model asset, sub-assets, .meta index | #60 | DONE | #68 |
| P4a: SW spot lights, emissive, occlusion, linear colour (user decision) | #61 | DONE | #94 |
| P4b: tangents, MikkTSpace, normal maps, optional PBR | #61 | DONE | #97 |
| P5: optional ufbx FBX/OBJ behind N2ENGINE_MODEL_UFBX, ON in the FreeType CI leg, axis/unit conversion, consistency tests | #62 | DONE in code | #107 |
| P5: a real Blender-exported binary FBX test fixture | #62 | NOT DONE | (#109 only documents it) |

Only #62 is still open, per the live API; its state is "open". The engine-side P5 work is merged: the docs page badge reads "PR #107", and the ufbx design tradeoffs are documented. The issue stayed open because of the Blender fixture. The committed `textured_cube.fbx` is generated ASCII, so it proves ufbx reads our reading of the format, not what Blender writes. `tests/assets/models/README.md` has an "Outstanding: a real Blender binary FBX" section. Orchestrator item `fbx-blender-fixture` is unapproved until the file arrives.

Remaining items and who owns them:
- (b) Blocked on the user: a Blender binary FBX (exporter quirks, binary encoding, PBR properties, multiple UV sets). Only the repo owner can produce it.
- (b) Blocked on the user: GPU verification. The P4a GL path (SRGB8_ALPHA8 plus per-draw GL_FRAMEBUFFER_SRGB, spot cone) and the P4b GLSL (normal maps, PBR) are documented as unverified on a GPU. CI only compiles them. See user-verification-checklist.md line 7 and the docs callout `design-p4b-gpu-unverified`.
- (c) Intentionally out of scope and documented as "later work": skinning, morph targets, animation, glTF cameras/lights, TEXCOORD_1+, IBL/tone mapping, MeshCollider, and roughness/metalness/opacity textures from FBX.
- (c) Vulkan drawing of lit meshes is tracked under #43 (stages A and B draw unlit meshes only; lit waits for stage C). This is not part of #3.
- (a) Nothing the orchestrator can do alone. It could add more synthetic fixtures, but the plan said test assets are self-authored.

Recommendation for #3: close, once #62 is resolved or deliberately split off. Keep #62 open as the single remaining tracker, since the body of #3 ("full mesh rendering and import for models such as Blender") is met for glTF (the format Blender exports natively) and for FBX/OBJ. If you want #3 closed now, first move the fixture to #62 (it is already there) and the GPU checks to the verification checklist (already there). So: close #3 now, with a comment pointing to #62 and the checklist. Suggested comment:

> Closing as delivered: #57 (PR #63), #58 (#64), #59 (#65), #60 (#68), #61 (#94 P4a, #97 P4b) and the engine side of #62 (#107) are merged. docs/meshes.html describes the result. Still open elsewhere: #62 stays open only for a Blender-exported binary FBX fixture (needs the repo owner, see tests/assets/models/README.md), and the OpenGL lighting/normal-map/PBR shaders have not been run on a GPU (tracked in the verification checklist). Skinning/animation and lit meshes on Vulkan are intentionally out of scope here (the latter is #43).

## #6 Editor/Project system

Body: hooks exist for Lua projects; needs testing and expansion for a GUI editor; goal is a protocol so an editor client can talk to a local or remote editor host that orchestrates the engine binary.

| Item (approved plan, 20 decisions) | Issue | Status | PR |
|---|---|---|---|
| E0: viewport frame fix; host ResourceUUID/--project/--port 0/ready line; polled-command logging | #69-#71 | DONE | #83, #84, #85 |
| E1a: protocol/generator groundwork | #72 | DONE | #87 |
| E1b: generated codecs | #73 | DONE | #88 |
| E2 | #74 | DONE | #89 |
| E3: project file, --create, scenes, revision/savedRevision | #75 | DONE | #90 (and #91) |
| E4: hierarchy and entity commands | #76 | DONE | #92 |
| E5: reflection / inspector commands | #77 | DONE | #93 |
| E6: undo/redo, autosave | #78 | DONE | #95 |
| E7a: on-demand frames, editor camera | #79 | DONE | #98 |
| E7b: picking and bounds | #80 | DONE | #100 |
| E8: assets, file watcher, Lua hot reload | #81 | DONE | #103 |
| E9: play mode (Electron-spawned child) | #82 | DONE | #102 |
| F1-F7 frontend (repo EpicNicks/N2Engine-Electron-Frontend #2-#8) | frontend | DONE | frontend #9, #10, #11, #14, #15, #17, #19, #20 (F5b #18) |
| Follow-ups: #96, #99 (PR #106), #101 (#105), BOM fix (#104, frontend #24) | | DONE | |
| Extras beyond the plan: remote mode (frontend #22), TLS (#112, frontend #26), per-user tokens, read-only, rate limits (#108 closed) | | DONE | |

Every engine sub-issue and the frontend phases in the plan show a merged PR. I did not check the live state of #69-#82 or the frontend issues on GitHub (budget), so whether those sub-issues are individually closed is unverified; the PRs say "closes #N", so they should be.

Remaining items:
- (b) Blocked on the user: the editor UI (F3-F7), the OpenGL play child, the direct-remote form (TLS checkbox, Inspect, Forget pin), the inspector pass-list editor, and remote mode over a real ssh server have never been run against a live host or in Electron (typecheck, bundle and node/DOM tests only). See user-verification-checklist.md lines 27-29 and verification-notes-2026-10-14.md. The ssh `Allocated port N` readiness line has never been observed on a real server.
- (b) Blocked on the user: any OpenGL path on a GPU (checklist line 7).
- (c) Out of scope or post-MVP, documented: orphaned .meta cleanup (needs a DeleteAsset command), edit commands in play mode (client refuses, v1), physics picking in edit mode (bodies are not created), the screen-space UI overlay not drawn/pickable in the editor view, remote play mode disabled.
- (a) Orchestrator can do: nothing required for the original goal. It could write the live-host test checklist into a runnable smoke script, but it cannot run an Electron UI against a GPU host itself.

Recommendation for #6: keep open, but only until the user's live-host pass. If you prefer a clean tracker, close it as "delivered, verification pending" and file one new issue "Editor: first live-host verification". I recommend split: close #6 and open a verification issue, because the original goal (hooks tested, protocol, GUI editor, local and remote host) is implemented and documented in docs/editor.html, and the only gaps are user-side checks. Suggested comment (post before closing, or keep open with it):

> All planned phases are merged: engine E0-E9 (#83-#85, #87-#93, #95, #98, #100, #102, #103) and frontend F1-F7 (frontend #9-#20, #18). Follow-ups #96/#99/#101 are merged too (#105, #106), as are remote mode, TLS and per-user tokens. Not yet verified by a human: the Electron UI (F3-F7), the OpenGL play child, direct-remote TLS pinning and ssh-tunnel remote mode against a real host. Remaining checklist: (1) run the editor against a live host on a GPU PC and file defects; (2) test remote mode over a real ssh server; (3) decide whether orphaned .meta cleanup (DeleteAsset) is wanted post-MVP. Suggest closing this umbrella and tracking (1)-(3) in a new verification issue.

## What I could not verify
- Live closed/open state of #57-#61, #69-#82 and the frontend issues (only #62 was queried). The merge facts come from `git log origin/master` and the progress log.
- The Decisions comments on #3 and #6 were not re-read; the item lists come from memory files and the docs, which agree with each other.
- Nothing about GPU behaviour, the Electron UI or real-host runs could be checked locally; the docs and checklist themselves say these are unverified.
