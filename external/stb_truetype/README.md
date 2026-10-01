# stb_truetype

`include/stb_truetype/stb_truetype.h` is stb_truetype v1.26, copied unmodified from
https://github.com/nothings/stb at commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`
(public domain or MIT; the licence text is at the end of the header).

The implementation is compiled once, in `text/src/text/StbTrueTypeBackend.cpp`, with `STBTT_STATIC`
so its symbols stay private to that file. Nothing else includes the header: the `text` library's
public API exposes no stb types.
