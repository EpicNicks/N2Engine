# Asset import: image decoding (#3 P1), glTF model parsing (#3 P3) and optional FBX/OBJ (#3 P5). CPU-only: depends on
# math and the vendored stb_image, cgltf, MikkTSpace and (optionally) ufbx, never on the engine, the renderer, GLFW,
# PhysX or nlohmann, so its tests run headless without them (as the text library's do).
file(GLOB_RECURSE ASSETIMPORT_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
    "src/*.c" # the cgltf, MikkTSpace and ufbx implementations, compiled as C
)

# FBX and OBJ import (#3 P5): ufbx, vendored under external/ufbx, is optional (like the text library's FreeType). OFF
# leaves out its importer and its one C translation unit, and the engine then loads glTF only.
option(N2ENGINE_MODEL_UFBX "Build the optional FBX/OBJ model importer (ufbx, vendored in external/ufbx)" OFF)
if(NOT N2ENGINE_MODEL_UFBX)
    list(FILTER ASSETIMPORT_SOURCES EXCLUDE REGEX ".*/(UfbxImporter\\.cpp|UfbxImplementation\\.c)$")
endif()

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
        ${CMAKE_SOURCE_DIR}/external/mikktspace/include
)

if(N2ENGINE_MODEL_UFBX)
    # Private, like the other parsers: no ufbx type or header reaches the library's interface
    target_include_directories(assetimport PRIVATE ${CMAKE_SOURCE_DIR}/external/ufbx/include)
    target_compile_definitions(assetimport PRIVATE N2ENGINE_MODEL_UFBX=1)
    message(STATUS "Asset import: FBX/OBJ importer enabled (ufbx, vendored)")
endif()

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
