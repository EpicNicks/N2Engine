Accepted by the user 2026-10-14 (and the user chose to FIX the review low in the master-update push: free retired objects directly when no frame is open):
- VMA pinned at v3.4.0 (commit 3aa921224c154a0d2c43912bc88e1c42ce1f7607), the latest stable v3.x release at the time; the header is stored with LF endings as fetched
- srgb applies to 3 and 4 channels only (R8G8B8A8_SRGB, UNORM fallback when unsupported); 1 and 2 channel textures ignore it, as TextureOptions documents and OpenGL does, instead of trying the optional R8/R8G8 sRGB formats
- A mesh with vertices but no indices gets no index buffer (indexCount 0) because Vulkan buffers cannot be empty; stage B must skip index binding for it
- Added extra test hooks beyond ReadTextureForTest: ReadMeshForTest, GetTextureInfoForTest, GetLiveMeshCountForTest, GetLiveTextureCountForTest, GetPendingDeletionCountForTest, GetLiveAllocationCountForTest
- Retired objects go to the open frame's slot, or to the last submitted frame's slot when no frame is open (not simply the current slot), so a frame still in flight is always waited for before the free
- Stage-0 targets and readback buffer left on AllocateMemory (moving them onto VMA was optional)
- VulkanTest fixture and helpers moved to VulkanTestFixture.hpp into namespace VulkanTesting with inline functions; behaviour unchanged; only EverythingThatWouldDrawFailsCleanly was edited in the old test file
- Added SubmitOneTimeFn (std::function wrapper) and LogOnce to VulkanRenderer so the resource code in a second translation unit can use the one-time submit template
- Docs: PR #142 badges on the stage A edits; no index.html row added
