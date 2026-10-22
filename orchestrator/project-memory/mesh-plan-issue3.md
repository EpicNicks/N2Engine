---
name: mesh-plan-issue3
description: "N2Engine #3 mesh rendering / model import — approved design decisions, sub-issues #57-#62 and work order (2026-10-06)"
metadata:
  node_type: memory
  type: project
  originSessionId: 6b1b31bc-fe59-40f5-8570-4fcd9e92983f
  modified: 2026-10-06T13:30:55.991Z
---

The user approved the #3 proposal and its review on 2026-10-06. The full proposal and the **Decisions** comment are posted on issue #3; read those for detail.

**Sub-issues, in order:**
- #57 Camera::LookAt fix (it faced away from its target; fix is `LookRotation(-forward, up)`). Lands before P2.
- #58 P1: stb_image, a CPU-only `assetimport/` target, the `Texture` asset, `GpuCache` (the text atlas moves onto it), SW bilinear RGBA, and a serialized `Image` sprite.
- #59 P2: `Mesh`/`Material` assets, `MeshRenderer`, the `DrawMesh` IndexRange overload, `DrawsInQueue`, and Cube/Sphere/Quad rebuilt on it.
- #60 P3: glTF import via cgltf, the `Model` asset, sub-assets via `ResourceUUID::FromSubAsset` plus a `.meta` index API.
- #61 P4: materials and lighting (P4a: SW spot lights, emissive, sRGB/linear off by default; P4b: normal maps, PBR).
- #62 P5: optional ufbx (FBX/OBJ) behind `N2ENGINE_MODEL_UFBX`, ON in the FreeType CI leg.

**Key choices:**
- textures are stored bottom row first (`flipY` defaults to true) and glTF v is flipped;
- a single `MeshRenderer` with no `MeshFilter`;
- models become an unpacked hierarchy via PrefabManager;
- `Vertex` stays 48 B until normal maps;
- from P2, `blend` follows the material's alpha mode (Opaque draws stop blending);
- test assets are generated in tests or self-authored, with no third-party samples.

**Status (2026-10-06):** #57 merged as PR #63 (LookAt fixed; Camera::GetForward/GetUp/GetRight added; near-vertical fallback keeps screen up continuous). #58 P1 merged as PR #64 (assetimport target, stb_image 2.30, Texture asset, GpuCache with text atlas moved onto it, SW bilinear + GL_NEAREST-correct nearest, Image sprites). P2 #59 merged as PR #65. P3 #60 merged as PR #68 (cgltf glTF import, Model asset, sub-assets, scene-wide memory budget, cgltf counting allocator). Next: P4 #61 (P4a: SW spot lights, emissive, sRGB/linear off by default; P4b: normal maps, MikkTSpace, PBR), then P5 #62 (ufbx).

**Why:** this carries the agreed mesh plan across sessions.
**How to apply:** one PR per sub-issue, each closing it. Use the dev agent → opus reviewer → CI green → merge flow ([[merge-authorization]]), with no attribution ([[no-claude-coauthor]]). Related plan: [[pending-minor-defects]].
