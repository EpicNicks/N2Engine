# stb_image (and stb_image_write, tests only)

`include/stb_image/stb_image.h` is stb_image v2.30 and `include/stb_image/stb_image_write.h` is
stb_image_write v1.16, both copied unmodified from https://github.com/nothings/stb at commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20` (public domain or MIT; the licence text is at the end of each
header).

stb_image is compiled once, in `assetimport/src/assetimport/ImageDecoder.cpp`, with `STB_IMAGE_STATIC` so
its symbols stay private to that file, and configured there: PNG, JPEG, TGA and BMP only, from memory only
(`STBI_NO_STDIO`), `STBI_MAX_DIMENSIONS 16384`, and `STBI_ASSERT` a no-op in Release builds. Nothing else
includes it: the `assetimport` library's public API exposes no stb types.

stb_image_write is used only by the `assetimport_tests` executable (`tests/assetimport/TestImages.cpp`), to
encode test images in memory. It is never compiled into the engine.
