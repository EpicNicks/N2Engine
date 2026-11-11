# The pack readers' third-party decoders (build targets stage 4; docs/packs.html). Both are C, built as static libraries the
# engine links privately: no engine header includes them.
#
#   n2_miniz   miniz 3.0.2 (external/miniz, MIT): inflate only, for zip packs (stored or deflate). Writing, the zip archive
#              API and stdio are compiled out, so only tinfl and the CRC are in the binary.
#   n2_lzma7z  the LZMA SDK 24.09's 7z decoder (external/lzma-sdk/C, public domain): the 7z archive reader with LZMA,
#              LZMA2 and the branch filters; PPMd is left out (the cook step never writes it, and a pack using it is refused
#              with the method named).

set(N2_MINIZ_DIR "${CMAKE_SOURCE_DIR}/external/miniz")
add_library(n2_miniz STATIC "${N2_MINIZ_DIR}/miniz.c")
target_include_directories(n2_miniz SYSTEM PUBLIC "${N2_MINIZ_DIR}")
target_compile_definitions(n2_miniz PUBLIC
    MINIZ_NO_STDIO=1
    MINIZ_NO_TIME=1
    MINIZ_NO_ARCHIVE_APIS=1
    MINIZ_NO_DEFLATE_APIS=1
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES=1
)

set(N2_LZMA_DIR "${CMAKE_SOURCE_DIR}/external/lzma-sdk/C")
add_library(n2_lzma7z STATIC
    "${N2_LZMA_DIR}/7zAlloc.c"
    "${N2_LZMA_DIR}/7zArcIn.c"
    "${N2_LZMA_DIR}/7zBuf.c"
    "${N2_LZMA_DIR}/7zBuf2.c"
    "${N2_LZMA_DIR}/7zCrc.c"
    "${N2_LZMA_DIR}/7zCrcOpt.c"
    "${N2_LZMA_DIR}/7zDec.c"
    "${N2_LZMA_DIR}/7zStream.c"
    "${N2_LZMA_DIR}/Bcj2.c"
    "${N2_LZMA_DIR}/Bra.c"
    "${N2_LZMA_DIR}/Bra86.c"
    "${N2_LZMA_DIR}/BraIA64.c"
    "${N2_LZMA_DIR}/CpuArch.c"
    "${N2_LZMA_DIR}/Delta.c"
    "${N2_LZMA_DIR}/Lzma2Dec.c"
    "${N2_LZMA_DIR}/LzmaDec.c"
)
target_include_directories(n2_lzma7z SYSTEM PUBLIC "${N2_LZMA_DIR}")
# Single-threaded decoding; no 7zFile.c (the engine gives the reader its own stream)
target_compile_definitions(n2_lzma7z PRIVATE Z7_ST=1)

foreach(_n2_pack_lib n2_miniz n2_lzma7z)
    set_target_properties(${_n2_pack_lib} PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        set_target_properties(${_n2_pack_lib} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # Third-party code: their warnings are not ours to fix
        target_compile_options(${_n2_pack_lib} PRIVATE /W0)
    else()
        target_compile_options(${_n2_pack_lib} PRIVATE -w)
    endif()
endforeach()
