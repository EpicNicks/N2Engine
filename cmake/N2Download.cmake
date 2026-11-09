# A pinned, hash-checked download that survives a flaky host. Used by cmake/DawnPrebuilt.cmake and cmake/EmdawnwebgpuPort.cmake,
# whose files come from Dawn's GitHub releases, which have failed CI with HTTP errors more than once.
#
#   n2_download(URL <url> DEST <file> SHA256 <hash> RESULT <var>)
#
# Sets <var> to "" on success, or to the reason it failed. The caller decides what a failure means.
#
#   - The environment variable N2_DOWNLOAD_CACHE names a folder of earlier downloads. A file there with the pinned hash is copied
#     instead of downloaded, and a new download is put there. CI restores and saves that folder with actions/cache. Without the
#     variable nothing is cached, as before.
#   - A failed download is tried twice more, after 10 and then 30 seconds.
#   - Whatever its source, the file must have the pinned SHA-256: a cached copy is checked again, and a wrong one is ignored.

function(n2_download)
    cmake_parse_arguments(ARG "" "URL;DEST;SHA256;RESULT" "" ${ARGN})
    if(NOT ARG_URL OR NOT ARG_DEST OR NOT ARG_SHA256 OR NOT ARG_RESULT)
        message(FATAL_ERROR "n2_download needs URL, DEST, SHA256 and RESULT")
    endif()
    get_filename_component(_n2_name "${ARG_URL}" NAME)
    get_filename_component(_n2_dest_dir "${ARG_DEST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_n2_dest_dir}")

    # Named by the hash alone: the pin is what identifies the file, so a new pin never picks up an old file
    set(_n2_cached "")
    if(DEFINED ENV{N2_DOWNLOAD_CACHE} AND NOT "$ENV{N2_DOWNLOAD_CACHE}" STREQUAL "")
        set(_n2_cached "$ENV{N2_DOWNLOAD_CACHE}/${ARG_SHA256}")
        if(EXISTS "${_n2_cached}")
            file(SHA256 "${_n2_cached}" _n2_cached_hash)
            if(_n2_cached_hash STREQUAL "${ARG_SHA256}")
                file(COPY_FILE "${_n2_cached}" "${ARG_DEST}")
                message(STATUS "${_n2_name}: from the download cache ($ENV{N2_DOWNLOAD_CACHE})")
                set(${ARG_RESULT} "" PARENT_SCOPE)
                return()
            endif()
            message(WARNING "${_n2_cached} does not have the pinned SHA-256; downloading instead")
            file(REMOVE "${_n2_cached}")
        endif()
    endif()

    set(_n2_reason "")
    foreach(_n2_pause 0 10 30)
        if(_n2_pause GREATER 0)
            message(STATUS "${_n2_name}: download failed (${_n2_reason}); trying again in ${_n2_pause} s")
            execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${_n2_pause})
        endif()
        # The hash is checked here, not with EXPECTED_HASH: that raises a CMake error on any failed attempt, which fails the
        # configure even when a retry then succeeds
        file(DOWNLOAD "${ARG_URL}" "${ARG_DEST}"
             TLS_VERIFY ON
             STATUS _n2_status)
        list(GET _n2_status 0 _n2_code)
        list(GET _n2_status 1 _n2_reason)
        if(_n2_code EQUAL 0)
            file(SHA256 "${ARG_DEST}" _n2_hash)
            if(NOT _n2_hash STREQUAL "${ARG_SHA256}")
                set(_n2_code 1)
                set(_n2_reason "SHA-256 ${_n2_hash}, not the pinned ${ARG_SHA256}")
            endif()
        endif()
        if(_n2_code EQUAL 0)
            if(_n2_cached)
                get_filename_component(_n2_cache_dir "${_n2_cached}" DIRECTORY)
                file(MAKE_DIRECTORY "${_n2_cache_dir}")
                file(COPY_FILE "${ARG_DEST}" "${_n2_cached}")
            endif()
            set(${ARG_RESULT} "" PARENT_SCOPE)
            return()
        endif()
        file(REMOVE "${ARG_DEST}")
    endforeach()
    set(${ARG_RESULT} "${_n2_reason}" PARENT_SCOPE)
endfunction()
