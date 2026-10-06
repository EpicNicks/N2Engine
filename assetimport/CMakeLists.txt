# Asset import: image decoding (#3 P1) and glTF model parsing (#3 P3). CPU-only: depends on math and the
# vendored stb_image and cgltf, never on the engine, the renderer, GLFW, PhysX or nlohmann, so its tests run
# headless without them (as the text library's do).
file(GLOB_RECURSE ASSETIMPORT_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
    "src/*.c" # cgltf's implementation, compiled as C
)

add_library(assetimport STATIC ${ASSETIMPORT_SOURCES})
add_library(N2Engine::assetimport ALIAS assetimport)

target_include_directories(assetimport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
        # Private: no stb or cgltf type or header reaches the library's interface
        ${CMAKE_SOURCE_DIR}/external/stb_image/include
        ${CMAKE_SOURCE_DIR}/external/cgltf/include
)

set_target_properties(assetimport PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(WIN32)
    set_target_properties(assetimport PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

target_link_libraries(assetimport PUBLIC MathCore::math)
