# The iOS player (build targets stage 10; docs/apple.html): an app bundle around the engine, with the game's pack as a
# bundle resource. Added by the root CMakeLists.txt for -DCMAKE_SYSTEM_NAME=iOS (the Xcode generator). n2build's apple
# flow (tools/n2build/lib/apple.mjs) configures it with:
#   N2ENGINE_IOS_PACK         the game's pack (n2build's cook step); without it the app is built with no game
#   N2ENGINE_IOS_BUNDLE_ID    build.targets.ios.appId (com.n2engine.game by default)
#   N2ENGINE_IOS_NAME         the name under the icon (build.targets.ios.displayName, else the project's name)
#   N2ENGINE_IOS_VERSION, N2ENGINE_IOS_BUILD   CFBundleShortVersionString and CFBundleVersion
#   N2ENGINE_IOS_ORIENTATION  any, landscape or portrait
set(N2ENGINE_IOS_PACK "" CACHE FILEPATH "The game's pack, put in the app bundle as game.n2pack")
set(N2ENGINE_IOS_BUNDLE_ID "com.n2engine.game" CACHE STRING "The app's bundle identifier")
set(N2ENGINE_IOS_NAME "N2Engine Game" CACHE STRING "The app's display name")
set(N2ENGINE_IOS_VERSION "1.0.0" CACHE STRING "The app's version")
set(N2ENGINE_IOS_BUILD "1" CACHE STRING "The app's build number")
set(N2ENGINE_IOS_ORIENTATION "any" CACHE STRING "any, landscape or portrait")

if(N2ENGINE_IOS_ORIENTATION STREQUAL "landscape")
    set(N2_IOS_ORIENTATIONS "<string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>")
elseif(N2ENGINE_IOS_ORIENTATION STREQUAL "portrait")
    set(N2_IOS_ORIENTATIONS "<string>UIInterfaceOrientationPortrait</string>")
else()
    set(N2_IOS_ORIENTATIONS "<string>UIInterfaceOrientationPortrait</string><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>")
endif()

set(_n2_ios_resources "")
if(N2ENGINE_IOS_PACK)
    if(NOT EXISTS "${N2ENGINE_IOS_PACK}")
        message(FATAL_ERROR "N2ENGINE_IOS_PACK: ${N2ENGINE_IOS_PACK} does not exist (run n2build's cook step first)")
    endif()
    # Copied under the name the player looks for, whatever the cook step called it
    configure_file("${N2ENGINE_IOS_PACK}" "${CMAKE_CURRENT_BINARY_DIR}/bundle/game.n2pack" COPYONLY)
    set(_n2_ios_resources "${CMAKE_CURRENT_BINARY_DIR}/bundle/game.n2pack")
endif()

add_executable(n2ios MACOSX_BUNDLE
    src/IosMain.mm
    src/IosHost.hpp
    ${_n2_ios_resources}
)
set_source_files_properties(${_n2_ios_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_include_directories(n2ios PRIVATE src)
target_link_libraries(n2ios PRIVATE
    engine
    renderer
    MathCore::math
    "-framework UIKit"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework AVFoundation"
    "-framework Foundation"
)
set_target_properties(n2ios PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "N2Game"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${N2ENGINE_IOS_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "${N2ENGINE_IOS_NAME}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${N2ENGINE_IOS_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${N2ENGINE_IOS_BUILD}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${N2ENGINE_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
)
target_compile_options(n2ios PRIVATE -fobjc-arc)
n2_check_project_modules(n2ios)
