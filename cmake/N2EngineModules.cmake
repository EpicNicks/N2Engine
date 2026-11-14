# Building one game without the engine modules it does not use (docs/project-modules.html). Included by the root CMakeLists.txt
# before its options, so that the project's list decides them.
#
#   cmake -S . -B build-mygame -DN2ENGINE_PROJECT=path/to/game
#
# reads "build.modules" from the project's project.n2proj and turns it into the options below. A project without a list, or a
# configure without N2ENGINE_PROJECT, builds everything, as before. The engine is one static library, so a list applies to the
# whole build tree: a game gets a tree of its own, and an editor's tree never sets this, because a project can use anything while
# it is being edited.
#
#   "physics3d": "none" | "physx" | "jolt" | "both"   N2ENGINE_USE_PHYSX and N2ENGINE_USE_JOLT
#   "physics2d": true | false                         N2ENGINE_USE_BOX2D
#   "particles": true | false                         N2ENGINE_USE_PARTICLES
#   "softbodies": true | false                        N2ENGINE_USE_SOFTBODIES
#   "audio": true | false                             N2ENGINE_USE_AUDIO
#   "ui": true | false                                N2ENGINE_USE_UI
#
# With -DN2ENGINE_TARGET=<name> (n2build sets it; build targets stage 3, docs/build-targets.html#settings), the target's own
# "build.targets.<name>.modules" is read first, key by key, and "build.modules" fills in the keys it does not name.
#
# Normal variables, which win over option() (policy CMP0077) and over a cached value from an earlier configure, so the list is
# the one thing that decides. scripts/project-modules.mjs checks the list against what the project uses.

# The repository root, from this file, so the scan is found whoever includes it
get_filename_component(_n2_modules_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(N2ENGINE_MODULES_SCRIPT "${_n2_modules_root}/scripts/project-modules.mjs" CACHE INTERNAL "")

set(N2ENGINE_PROJECT "" CACHE PATH "A game's project folder: its project.n2proj's build.modules decides which engine modules this tree builds")
set(N2ENGINE_TARGET "" CACHE STRING "The build target this tree is for (n2build sets it): build.targets.<target>.modules wins over build.modules")

# The node that runs scripts/project-modules.mjs, or empty. The web build has its own (the Emscripten toolchain's), which is known
# only after the toolchain has run, and that is before this file.
function(_n2_find_module_node out)
    if(CMAKE_CROSSCOMPILING_EMULATOR)
        set(${out} "${CMAKE_CROSSCOMPILING_EMULATOR}" PARENT_SCOPE)
        return()
    endif()
    find_program(N2ENGINE_MODULES_NODE node)
    if(N2ENGINE_MODULES_NODE)
        set(${out} "${N2ENGINE_MODULES_NODE}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# Checks the list against the project again each time this game's target links, because scripts change without a reconfigure.
# A native game calls this on its own executable; the web player does it in web/CMakeLists.txt.
function(n2_check_project_modules target)
    if(NOT N2ENGINE_PROJECT)
        return()
    endif()
    _n2_find_module_node(node)
    if(node)
        set(target_args "")
        if(N2ENGINE_TARGET)
            set(target_args --target "${N2ENGINE_TARGET}")
        endif()
        add_custom_command(TARGET ${target} PRE_LINK
            COMMAND ${node} "${N2ENGINE_MODULES_SCRIPT}" "${N2ENGINE_PROJECT}" ${target_args}
            COMMENT "Checking the project's engine modules against what it uses"
            VERBATIM)
    endif()
endfunction()

if(N2ENGINE_PROJECT)
    get_filename_component(N2ENGINE_PROJECT "${N2ENGINE_PROJECT}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${N2ENGINE_PROJECT}/project.n2proj")
        message(FATAL_ERROR "N2ENGINE_PROJECT (${N2ENGINE_PROJECT}) has no project.n2proj")
    endif()
    file(READ "${N2ENGINE_PROJECT}/project.n2proj" _n2_project_json)

    # Where each key is read from: the target's list first, then build.modules (a missing list is no error: it names nothing)
    set(_n2_lists "")
    set(_n2_list_names "")
    if(N2ENGINE_TARGET)
        string(JSON _n2_modules_type ERROR_VARIABLE _n2_json_error TYPE "${_n2_project_json}" build targets ${N2ENGINE_TARGET} modules)
        if(NOT _n2_json_error)
            if(NOT _n2_modules_type STREQUAL "OBJECT")
                message(FATAL_ERROR "build.targets.${N2ENGINE_TARGET}.modules in ${N2ENGINE_PROJECT}/project.n2proj must be an object")
            endif()
            # '|' between the JSON keys: a CMake list item cannot hold ';'
            list(APPEND _n2_lists "targets|${N2ENGINE_TARGET}|modules")
            list(APPEND _n2_list_names "build.targets.${N2ENGINE_TARGET}.modules")
        endif()
    endif()
    string(JSON _n2_modules_type ERROR_VARIABLE _n2_json_error TYPE "${_n2_project_json}" build modules)
    if(NOT _n2_json_error)
        if(NOT _n2_modules_type STREQUAL "OBJECT")
            message(FATAL_ERROR "build.modules in ${N2ENGINE_PROJECT}/project.n2proj must be an object")
        endif()
        list(APPEND _n2_lists "modules")
        list(APPEND _n2_list_names "build.modules")
    endif()

    # _n2_module_lookup(<key> <found> <value> <where>): the first list naming key wins. Sets _n2_lookup_type to the
    # value's JSON type; a value that is not a string or boolean reads as "<TYPE>", which the checks below refuse.
    macro(_n2_module_lookup key found_var value_var where_var)
        set(${found_var} FALSE)
        set(_n2_index 0)
        foreach(_n2_list_path IN LISTS _n2_lists)
            if(NOT ${found_var})
                string(REPLACE "|" ";" _n2_path_parts "${_n2_list_path}")
                string(JSON _n2_lookup_type ERROR_VARIABLE _n2_json_error TYPE "${_n2_project_json}" build ${_n2_path_parts} ${key})
                if(NOT _n2_json_error)
                    set(${found_var} TRUE)
                    list(GET _n2_list_names ${_n2_index} ${where_var})
                    if(_n2_lookup_type STREQUAL "BOOLEAN" OR _n2_lookup_type STREQUAL "STRING")
                        string(JSON ${value_var} GET "${_n2_project_json}" build ${_n2_path_parts} ${key})
                    else()
                        set(${value_var} "<${_n2_lookup_type}>")
                    endif()
                endif()
            endif()
            math(EXPR _n2_index "${_n2_index} + 1")
        endforeach()
    endmacro()

    if(NOT _n2_lists)
        message(STATUS "Engine modules: ${N2ENGINE_PROJECT} has no build.modules list, so every module is built")
    else()
        # physics3d: absent keeps the options as they are, so a list can name only what it strips
        _n2_module_lookup(physics3d _n2_found _n2_physics3d _n2_where)
        if(_n2_found)
            if(_n2_physics3d STREQUAL "none")
                set(N2ENGINE_USE_PHYSX OFF)
                set(N2ENGINE_USE_JOLT OFF)
            elseif(_n2_physics3d STREQUAL "physx")
                set(N2ENGINE_USE_PHYSX ON)
                set(N2ENGINE_USE_JOLT OFF)
            elseif(_n2_physics3d STREQUAL "jolt")
                set(N2ENGINE_USE_PHYSX OFF)
                set(N2ENGINE_USE_JOLT ON)
            elseif(_n2_physics3d STREQUAL "both")
                set(N2ENGINE_USE_PHYSX ON)
                set(N2ENGINE_USE_JOLT ON)
            else()
                message(FATAL_ERROR "${_n2_where}.physics3d must be none, physx, jolt or both, not '${_n2_physics3d}'")
            endif()
            message(STATUS "Engine modules: physics3d ${_n2_physics3d}, from ${_n2_where} in ${N2ENGINE_PROJECT}")
        endif()

        # The modules that are on or off, each with its option
        foreach(_n2_pair IN ITEMS "physics2d:N2ENGINE_USE_BOX2D" "particles:N2ENGINE_USE_PARTICLES"
                                  "softbodies:N2ENGINE_USE_SOFTBODIES" "audio:N2ENGINE_USE_AUDIO" "ui:N2ENGINE_USE_UI")
            string(REPLACE ":" ";" _n2_pair "${_n2_pair}")
            list(GET _n2_pair 0 _n2_module)
            list(GET _n2_pair 1 _n2_option)
            _n2_module_lookup(${_n2_module} _n2_found _n2_module_value _n2_where)
            if(_n2_found)
                if(NOT _n2_lookup_type STREQUAL "BOOLEAN")
                    message(FATAL_ERROR "${_n2_where}.${_n2_module} must be true or false")
                endif()
                if(_n2_module_value)
                    set(${_n2_option} ON)
                else()
                    set(${_n2_option} OFF)
                endif()
                message(STATUS "Engine modules: ${_n2_module} ${_n2_module_value}, from ${_n2_where} in ${N2ENGINE_PROJECT}")
            endif()
        endforeach()

        # Before anything is compiled: a build this long should not end by reporting a mistake it could have reported now
        _n2_find_module_node(_n2_node)
        set(_n2_target_args "")
        if(N2ENGINE_TARGET)
            set(_n2_target_args --target "${N2ENGINE_TARGET}")
        endif()
        if(_n2_node)
            execute_process(
                COMMAND ${_n2_node} "${N2ENGINE_MODULES_SCRIPT}" "${N2ENGINE_PROJECT}" ${_n2_target_args}
                RESULT_VARIABLE _n2_check_result)
            if(NOT _n2_check_result EQUAL 0)
                message(FATAL_ERROR "The engine modules in ${N2ENGINE_PROJECT}/project.n2proj do not fit the project (see above). "
                                    "Keep the module, or remove what uses it.")
            endif()
        else()
            message(WARNING "No node was found, so the project's engine modules were not checked against what it uses")
        endif()
    endif()
endif()
