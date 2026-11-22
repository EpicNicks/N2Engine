# The web player (stage 2 of issue #155; docs/web-build.html). Added by the root CMakeLists.txt only under
# N2ENGINE_PLATFORM_WEB.
#
#   n2_web_input   the pure translation from browser events to the engine's input types, and the parsing of the player's
#                  --renderer argument (no DOM, so it is unit tested under node: tests/web)
#   N2WebPlayer    the player: the engine drawing to the page's canvas. WebGPU is
#                  linked in (stage G4 of #155, through the renderer: cmake/EmdawnwebgpuPort.cmake); the player
#                  selects WebGPU when the shell acquires a device, otherwise software. It links straight into
#                  the package folder, ${CMAKE_BINARY_DIR}/web-package, as index.html + index.js + index.wasm + index.data
#                  (+ n2-loader.js and manifest.json from web-package):
#                  static files only, every URL relative, nothing the host has to be configured for
#   web-package    builds that folder (the target to build) and finishes it: n2-loader.js, manifest.json and, for the size and
#                  speed profiles, the .br/.gz siblings; scripts/web-package-smoke.mjs checks and runs it
#
# The page (shell.html) is emcc's --shell-file: emcc fills in the script tag, which names index.js relative to the page,
# and index.js finds index.wasm and index.data the same way, so the folder works from any path or subdomain.

add_library(n2_web_input STATIC player/WebInputMap.cpp player/WebRendererOption.cpp)
target_include_directories(n2_web_input PUBLIC player)
target_link_libraries(n2_web_input PUBLIC engine)
set_target_properties(n2_web_input PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_executable(N2WebPlayer player/WebPlayer.cpp)
target_link_libraries(N2WebPlayer PRIVATE n2_web_input engine renderer MathCore::math)
set_target_properties(N2WebPlayer PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME index
    SUFFIX ".html"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web-package"
)

# The game folder the player runs, packed into the virtual filesystem (a .data file next to the page): lua_project's
# assets and project file, mounted at /project (the player's default --project). Its sources, tools and scenes/ stay out.
set(N2_WEB_PROJECT_DIR "${CMAKE_SOURCE_DIR}/lua_project" CACHE PATH "The project folder the web player packs (needs project.n2proj and assets/)")
# A game built with its module list packs that game (docs/project-modules.html)
if(N2ENGINE_PROJECT)
    set(N2_WEB_PROJECT_DIR "${N2ENGINE_PROJECT}")
endif()
if(NOT EXISTS "${N2_WEB_PROJECT_DIR}/assets" OR NOT EXISTS "${N2_WEB_PROJECT_DIR}/project.n2proj")
    message(FATAL_ERROR "N2_WEB_PROJECT_DIR (${N2_WEB_PROJECT_DIR}) needs project.n2proj and assets/")
endif()
file(GLOB_RECURSE N2_WEB_PROJECT_FILES CONFIGURE_DEPENDS "${N2_WEB_PROJECT_DIR}/assets/*")

# The project's .import folder too, when it has one: an asset's UUID is the one its .meta stores (an asset moved in the
# editor keeps its UUID, which then differs from the hash of its path), so a package without the .meta files would give
# moved assets new UUIDs and break every reference to them; the import settings live there as well. The loader reads it
# exactly as the editor does. A project without the folder (lua_project in this repository: it never moved anything, and
# .gitignore leaves its .import out) packs as before, and every UUID is the hash of its path.
set(N2_WEB_IMPORT_PRELOAD "")
if(EXISTS "${N2_WEB_PROJECT_DIR}/.import")
    set(N2_WEB_IMPORT_PRELOAD "SHELL:--preload-file \"${N2_WEB_PROJECT_DIR}/.import@/project/.import\"")
    file(GLOB_RECURSE N2_WEB_IMPORT_FILES CONFIGURE_DEPENDS "${N2_WEB_PROJECT_DIR}/.import/*")
    list(APPEND N2_WEB_PROJECT_FILES ${N2_WEB_IMPORT_FILES})
    message(STATUS "Web package: packing ${N2_WEB_PROJECT_DIR}/.import (the stored asset UUIDs)")
else()
    message(STATUS "Web package: ${N2_WEB_PROJECT_DIR} has no .import folder; asset UUIDs are the hashes of their paths")
endif()

# Late packs (#425, docs/web-build.html#late-packs): the folders the project lists in build.web.lateAssets (inside assets/)
# are left out of index.data and written as late-<n>.n2pack next to it (scripts/web-late-packs.mjs, at web-package time);
# the loader fetches them once the game runs and the player mounts each at its folder. late-packs.json, which names them,
# is written here and preloaded at /project/late-packs.json. A project without the list packs as before.
file(READ "${N2_WEB_PROJECT_DIR}/project.n2proj" _n2_web_project_text)
string(JSON _n2_late_type ERROR_VARIABLE _n2_late_error TYPE "${_n2_web_project_text}" build web lateAssets)
set(N2_WEB_LATE_FOLDERS "")
if(NOT _n2_late_error AND _n2_late_type STREQUAL "ARRAY")
    string(JSON _n2_late_count LENGTH "${_n2_web_project_text}" build web lateAssets)
    if(_n2_late_count GREATER 0)
        math(EXPR _n2_late_last "${_n2_late_count} - 1")
        foreach(_n2_i RANGE ${_n2_late_last})
            string(JSON _n2_folder GET "${_n2_web_project_text}" build web lateAssets ${_n2_i})
            string(REGEX REPLACE "/+$" "" _n2_folder "${_n2_folder}")
            if(NOT _n2_folder MATCHES "^assets/[^/]" OR _n2_folder MATCHES "(^|/)\\.\\.?(/|$)" OR NOT IS_DIRECTORY "${N2_WEB_PROJECT_DIR}/${_n2_folder}")
                message(FATAL_ERROR "build.web.lateAssets: '${_n2_folder}' is not a folder inside ${N2_WEB_PROJECT_DIR}/assets")
            endif()
            list(APPEND N2_WEB_LATE_FOLDERS "${_n2_folder}")
        endforeach()
    endif()
elseif(NOT _n2_late_error)
    message(FATAL_ERROR "build.web.lateAssets in ${N2_WEB_PROJECT_DIR}/project.n2proj must be an array of folders")
endif()
set(N2_WEB_LATE_PRELOAD "")
set(N2_WEB_LATE_RUNTIME "")
if(N2_WEB_LATE_FOLDERS AND N2ENGINE_WEB_PACK)
    # One game pack (n2build) holds every file: there is nothing to fetch later
    message(WARNING "build.web.lateAssets is ignored with N2ENGINE_WEB_PACK: the game's one pack is downloaded whole before the start")
    set(N2_WEB_LATE_FOLDERS "")
endif()
if(N2_WEB_LATE_FOLDERS)
    # The loader writes each pack into the virtual filesystem and calls the player (FS, ccall)
    set(N2_WEB_LATE_RUNTIME ",FS,ccall")
    set(_n2_late_json "{\n  \"format\": 1,\n  \"packs\": [")
    set(_n2_index 0)
    foreach(_n2_folder IN LISTS N2_WEB_LATE_FOLDERS)
        if(_n2_index GREATER 0)
            string(APPEND _n2_late_json ",")
        endif()
        string(APPEND _n2_late_json "\n    { \"file\": \"late-${_n2_index}.n2pack\", \"folder\": \"${_n2_folder}\" }")
        list(APPEND N2_WEB_LATE_PRELOAD
            "SHELL:--exclude-file \"${N2_WEB_PROJECT_DIR}/${_n2_folder}\""
            "SHELL:--exclude-file \"${N2_WEB_PROJECT_DIR}/${_n2_folder}/*\"")
        math(EXPR _n2_index "${_n2_index} + 1")
    endforeach()
    string(APPEND _n2_late_json "\n  ]\n}\n")
    set(_n2_late_old "")
    if(EXISTS "${CMAKE_BINARY_DIR}/web/late-packs.json")
        file(READ "${CMAKE_BINARY_DIR}/web/late-packs.json" _n2_late_old)
    endif()
    if(NOT _n2_late_old STREQUAL _n2_late_json)
        file(WRITE "${CMAKE_BINARY_DIR}/web/late-packs.json" "${_n2_late_json}")
    endif()
    list(APPEND N2_WEB_LATE_PRELOAD "SHELL:--preload-file \"${CMAKE_BINARY_DIR}/web/late-packs.json@/project/late-packs.json\"")
    target_compile_definitions(N2WebPlayer PRIVATE N2_WEB_LATE_PACKS=1)
    list(JOIN N2_WEB_LATE_FOLDERS ", " _n2_late_shown)
    message(STATUS "Web package: late packs for ${_n2_late_shown} (fetched after the start)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${N2_WEB_PROJECT_DIR}/project.n2proj")

# The game as one pack (build targets stage 4; docs/packs.html): n2build cooks the project into a stored zip and passes it as
# N2ENGINE_WEB_PACK, which is preloaded as /game.n2pack instead of the folders above; the player mounts it at /project. Without
# it the folders are preloaded as before.
set(N2ENGINE_WEB_PACK "" CACHE FILEPATH "A game's pack (n2build cook) to preload as /game.n2pack instead of the project folder")
if(N2ENGINE_WEB_PACK)
    if(NOT EXISTS "${N2ENGINE_WEB_PACK}")
        message(FATAL_ERROR "N2ENGINE_WEB_PACK (${N2ENGINE_WEB_PACK}) does not exist: n2build cooks it before configuring")
    endif()
    set(N2_WEB_GAME_PRELOAD "SHELL:--preload-file \"${N2ENGINE_WEB_PACK}@/game.n2pack\"")
    set(N2_WEB_GAME_DEPENDS "${N2ENGINE_WEB_PACK}")
    # The player's pack mount (WebPlayer.cpp): compiled only into a build that preloads a pack
    target_compile_definitions(N2WebPlayer PRIVATE N2_WEB_GAME_PACK=1)
    message(STATUS "Web package: the game is the pack ${N2ENGINE_WEB_PACK}")
else()
    set(N2_WEB_GAME_PRELOAD
        "SHELL:--preload-file \"${N2_WEB_PROJECT_DIR}/assets@/project/assets\""
        "SHELL:--preload-file \"${N2_WEB_PROJECT_DIR}/project.n2proj@/project/project.n2proj\""
        ${N2_WEB_IMPORT_PRELOAD}
        ${N2_WEB_LATE_PRELOAD})
    set(N2_WEB_GAME_DEPENDS "${N2_WEB_PROJECT_DIR}/project.n2proj" ${N2_WEB_PROJECT_FILES})
endif()

# Single-threaded and without cross-origin isolation: no pthreads, no SharedArrayBuffer (cmake/N2EngineWeb.cmake).
# MODULARIZE: index.js is a factory (createN2Player), which the page calls when it is ready and node's smoke test calls
# with its own arguments and hooks; callMain is what the page's Start button runs.
# Without the WebGPU renderer (build.modules.webgpu false) the page is a copy whose n2-webgpu tag says so
set(N2_WEB_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
if(NOT N2ENGINE_RENDERER_WEBGPU)
    file(READ "${N2_WEB_SHELL}" _n2_shell_text)
    set(_n2_shell_yes "<meta name=\"n2-webgpu\" content=\"yes\">")
    string(FIND "${_n2_shell_text}" "${_n2_shell_yes}" _n2_shell_at)
    if(_n2_shell_at LESS 0)
        message(FATAL_ERROR "web/shell.html has no ${_n2_shell_yes} tag for a build without WebGPU to rewrite")
    endif()
    string(REPLACE "${_n2_shell_yes}" "<meta name=\"n2-webgpu\" content=\"no\">" _n2_shell_text "${_n2_shell_text}")
    # Written only when it changes, so a reconfigure does not relink the player
    set(_n2_shell_old "")
    if(EXISTS "${CMAKE_BINARY_DIR}/web/shell.html")
        file(READ "${CMAKE_BINARY_DIR}/web/shell.html" _n2_shell_old)
    endif()
    if(NOT _n2_shell_old STREQUAL _n2_shell_text)
        file(WRITE "${CMAKE_BINARY_DIR}/web/shell.html" "${_n2_shell_text}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${N2_WEB_SHELL}")
    set(N2_WEB_SHELL "${CMAKE_BINARY_DIR}/web/shell.html")
endif()
target_link_options(N2WebPlayer PRIVATE
    -sMODULARIZE=1
    -sEXPORT_NAME=createN2Player
    -sEXPORTED_RUNTIME_METHODS=callMain,ENV${N2_WEB_LATE_RUNTIME}
    -lidbfs.js
    "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/n2-saves.js\""
    "SHELL:--shell-file \"${N2_WEB_SHELL}\""
    ${N2_WEB_GAME_PRELOAD}
)
n2_check_project_modules(N2WebPlayer)
set_property(TARGET N2WebPlayer APPEND PROPERTY LINK_DEPENDS
    "${N2_WEB_SHELL}" "${CMAKE_CURRENT_SOURCE_DIR}/webgpu-library.js" "${CMAKE_CURRENT_SOURCE_DIR}/n2-saves.js"
    ${N2_WEB_GAME_DEPENDS})

# Not a node/ctest executable: its map callbacks need a browser event loop and
# a real GPUDevice. The browser smoke runs it when N2_READBACK_DIR points here.
if(N2ENGINE_BUILD_TESTS AND N2ENGINE_RENDERER_WEBGPU)
    add_executable(N2WebGpuReadbackTests EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tests/browser/WebGpuReadback.cpp")
    target_link_libraries(N2WebGpuReadbackTests PRIVATE renderer)
    set_target_properties(N2WebGpuReadbackTests PROPERTIES
        OUTPUT_NAME n2-webgpu-readback
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/browser-tests")
    target_link_options(N2WebGpuReadbackTests PRIVATE
        -sMODULARIZE=1 -sEXPORT_NAME=createN2ReadbackTests
        -sEXPORTED_RUNTIME_METHODS=callMain)
endif()

# After the link: the loader next to the page, manifest.json (sizes and sha256 of every file) and, for the size and speed
# profiles, the .br/.gz siblings (scripts/web-package-finalize.mjs). The plain files always stay.
set(N2_WEB_FINALIZE_ARGS "")
if(N2ENGINE_WEB_PRECOMPRESS_EFFECTIVE)
    list(APPEND N2_WEB_FINALIZE_ARGS --precompress)
endif()
if(N2_WEB_LATE_FOLDERS)
    list(APPEND N2_WEB_FINALIZE_ARGS --late-packs "${CMAKE_BINARY_DIR}/web/late-packs.json")
endif()
set(N2_WEB_LATE_COMMAND "")
if(N2_WEB_LATE_FOLDERS)
    list(JOIN N2_WEB_LATE_FOLDERS "," _n2_late_arg)
    set(N2_WEB_LATE_COMMAND COMMAND ${N2ENGINE_WEB_NODE} "${CMAKE_SOURCE_DIR}/scripts/web-late-packs.mjs"
        --project "${N2_WEB_PROJECT_DIR}" --out "${CMAKE_BINARY_DIR}/web-package" --folders "${_n2_late_arg}")
endif()
add_custom_target(web-package
    ${N2_WEB_LATE_COMMAND}
    COMMAND ${N2ENGINE_WEB_NODE} "${CMAKE_SOURCE_DIR}/scripts/web-package-finalize.mjs" "${CMAKE_BINARY_DIR}/web-package"
            --loader "${CMAKE_CURRENT_SOURCE_DIR}/n2-loader.js" --profile ${N2ENGINE_WEB_PROFILE} ${N2_WEB_FINALIZE_ARGS}
    COMMENT "The web package is in ${CMAKE_BINARY_DIR}/web-package"
    VERBATIM
)
add_dependencies(web-package N2WebPlayer)
