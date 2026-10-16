# The web player (stage 2 of issue #155; docs/web-build.html). Added by the root CMakeLists.txt only under
# N2ENGINE_PLATFORM_WEB.
#
#   n2_web_input   the pure translation from browser events to the engine's input types (no DOM, so it is unit tested
#                  under node: tests/web)
#   N2WebPlayer    the player: the engine on the software renderer, drawing to the page's canvas. It links straight into
#                  the package folder, ${CMAKE_BINARY_DIR}/web-package, as index.html + index.js + index.wasm + index.data:
#                  static files only, every URL relative, nothing the host has to be configured for
#   web-package    builds that folder (the target to build; scripts/web-package-smoke.mjs checks and runs it)
#
# The page (shell.html) is emcc's --shell-file: emcc fills in the script tag, which names index.js relative to the page,
# and index.js finds index.wasm and index.data the same way, so the folder works from any path or subdomain.

add_library(n2_web_input STATIC player/WebInputMap.cpp)
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
if(NOT EXISTS "${N2_WEB_PROJECT_DIR}/assets" OR NOT EXISTS "${N2_WEB_PROJECT_DIR}/project.n2proj")
    message(FATAL_ERROR "N2_WEB_PROJECT_DIR (${N2_WEB_PROJECT_DIR}) needs project.n2proj and assets/")
endif()
file(GLOB_RECURSE N2_WEB_PROJECT_FILES CONFIGURE_DEPENDS "${N2_WEB_PROJECT_DIR}/assets/*")

# Single-threaded and without cross-origin isolation: no pthreads, no SharedArrayBuffer (cmake/N2EngineWeb.cmake).
# MODULARIZE: index.js is a factory (createN2Player), which the page calls when it is ready and node's smoke test calls
# with its own arguments and hooks; callMain is what the page's Start button runs.
target_link_options(N2WebPlayer PRIVATE
    -sMODULARIZE=1
    -sEXPORT_NAME=createN2Player
    -sEXPORTED_RUNTIME_METHODS=callMain
    "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
    "SHELL:--preload-file ${N2_WEB_PROJECT_DIR}/assets@/project/assets"
    "SHELL:--preload-file ${N2_WEB_PROJECT_DIR}/project.n2proj@/project/project.n2proj"
)
set_property(TARGET N2WebPlayer APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shell.html" "${N2_WEB_PROJECT_DIR}/project.n2proj" ${N2_WEB_PROJECT_FILES})

add_custom_target(web-package DEPENDS N2WebPlayer
    COMMENT "The web package is in ${CMAKE_BINARY_DIR}/web-package"
)
