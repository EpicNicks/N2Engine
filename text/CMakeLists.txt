# Fonts, SDF atlases and text layout. CPU-only: depends on math and the vendored stb_truetype, never on
# the engine, the renderer, GLFW or nlohmann, so its tests run headless without them.
file(GLOB_RECURSE TEXT_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

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

# Font backends: stb_truetype is always built (StbTrueTypeBackend.cpp). An optional FreeType backend
# goes behind its own option here and adds itself to the list in FontBackendFactory.cpp.
