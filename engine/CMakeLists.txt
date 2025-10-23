file(GLOB_RECURSE ENGINE_SOURCES
    "src/*.cpp"
    "src/*.cc"
    "src/*.cxx"
)

add_library(engine STATIC ${ENGINE_SOURCES})

target_include_directories(engine 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/nlohmann/include>
        $<BUILD_INTERFACE:${PHYSX_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(engine PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(engine PUBLIC N2ENGINE_DEBUG=1)
endif()

if(WIN32)
    target_link_directories(engine PUBLIC
        ${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/debug
        ${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.mt/release
    )
endif()

target_link_libraries(engine 
    PRIVATE 
        renderer
        MathCore::math
        ${PHYSX_LIBS}
)
