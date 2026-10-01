# Fonts, SDF atlases and text layout. CPU-only: depends on math and the vendored stb_truetype, never on
# the engine, the renderer, GLFW or nlohmann, so its tests run headless without them.
file(GLOB_RECURSE TEXT_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

# Font backends: stb_truetype is always built (StbTrueTypeBackend.cpp). FreeType is optional and adds
# itself to the list in FontBackendFactory.cpp when this is ON.
option(N2ENGINE_TEXT_FREETYPE "Build the optional FreeType font backend (fetched with FetchContent)" OFF)
if(NOT N2ENGINE_TEXT_FREETYPE)
    list(FILTER TEXT_SOURCES EXCLUDE REGEX ".*/FreeTypeBackend\\.cpp$")
endif()

# The default font is embedded at build time; the generated source lives in the build tree, not git
set(TEXT_DEFAULT_FONT "${CMAKE_SOURCE_DIR}/engine/assets/fonts/NotoSans-Regular.ttf")
set(TEXT_DEFAULT_FONT_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedDefaultFont.cpp")
add_custom_command(
    OUTPUT "${TEXT_DEFAULT_FONT_CPP}"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=${TEXT_DEFAULT_FONT}"
        "-DOUTPUT=${TEXT_DEFAULT_FONT_CPP}"
        "-DNAMESPACE=N2Engine::Text"
        "-DFUNCTION=GetDefaultFontData"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    DEPENDS "${TEXT_DEFAULT_FONT}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    COMMENT "Embedding the default font"
    VERBATIM
)

add_library(text STATIC ${TEXT_SOURCES} "${TEXT_DEFAULT_FONT_CPP}")
add_library(N2Engine::text ALIAS text)

target_include_directories(text
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/external/stb_truetype/include
)

set_target_properties(text PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(WIN32)
    set_target_properties(text PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

target_link_libraries(text PUBLIC MathCore::math)

# FreeType (see N2ENGINE_TEXT_FREETYPE above): fetched, built static and linked privately
if(N2ENGINE_TEXT_FREETYPE)
    include(FetchContent)

    # Every optional FreeType dependency is off, so nothing is searched for on the build machine and
    # the result is the same everywhere. The FT_* switches are option()s in FreeType's CMakeLists, which
    # ignore plain variables under its policy level, so they're forced into the cache.
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    # Belt and braces: even if a switch above stops working, these find_package calls find nothing.
    # Plain variables, so they only apply in this directory and FreeType's (not to OpenAL and friends).
    set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_BZip2 TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_PNG TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_HarfBuzz TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_BrotliDec TRUE)
    # A static library with no install rules
    set(BUILD_SHARED_LIBS OFF)
    set(SKIP_INSTALL_ALL ON)
    # FreeType declares cmake_minimum_required(VERSION 3.0...3.5), which leaves CMP0091 unset (OLD), and
    # then CMAKE_MSVC_RUNTIME_LIBRARY would be ignored. NEW makes it honour the repo's static runtime.
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

    FetchContent_Declare(
        freetype
        URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
        URL_HASH SHA256=bc5c898e4756d373e0d991bab053036c5eb2aa7c0d5c67e8662ddc6da40c4103
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(freetype)

    set_target_properties(freetype PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    # Private: no FreeType type or header reaches the text library's interface
    target_link_libraries(text PRIVATE freetype)
    target_compile_definitions(text PRIVATE N2ENGINE_TEXT_FREETYPE=1)
    message(STATUS "Text: FreeType font backend enabled (FreeType 2.13.3)")
endif()
