# Keep the submodule at the upstream revision and patch a private build copy.
find_package(Git REQUIRED)
set(quickjs_original "${CMAKE_CURRENT_SOURCE_DIR}/deps/quickjs")
set(quickjs_source "${CMAKE_CURRENT_BINARY_DIR}/quickjs-patched")
set(quickjs_patches "${CMAKE_CURRENT_SOURCE_DIR}/patches/quickjs-performance.patch")
if(BUILD_WITH_INSPECTOR)
    list(APPEND quickjs_patches "${CMAKE_CURRENT_SOURCE_DIR}/patches/quickjs-debugger.patch")
endif()
set(quickjs_patched_files quickjs.c quickjs.h quickjs-opcode.h)

set(quickjs_fingerprint "")
foreach(patch IN LISTS quickjs_patches)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    file(SHA256 "${patch}" patch_fingerprint)
    string(APPEND quickjs_fingerprint "${patch_fingerprint}")
endforeach()
file(COPY "${quickjs_original}/" DESTINATION "${quickjs_source}"
     PATTERN ".git" EXCLUDE PATTERN "test262" EXCLUDE
     PATTERN "quickjs.c" EXCLUDE PATTERN "quickjs.h" EXCLUDE PATTERN "quickjs-opcode.h" EXCLUDE)
foreach(source IN LISTS quickjs_patched_files)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${quickjs_original}/${source}")
    file(SHA256 "${quickjs_original}/${source}" source_fingerprint)
    string(APPEND quickjs_fingerprint "${source_fingerprint}")
    if(NOT EXISTS "${quickjs_source}/${source}")
        set(quickjs_missing_source TRUE)
    endif()
endforeach()

set(quickjs_stamp "${quickjs_source}/.patch-fingerprint")
if(EXISTS "${quickjs_stamp}")
    file(READ "${quickjs_stamp}" quickjs_previous_fingerprint)
endif()
if(quickjs_missing_source OR NOT quickjs_fingerprint STREQUAL quickjs_previous_fingerprint)
    foreach(source IN LISTS quickjs_patched_files)
        configure_file("${quickjs_original}/${source}" "${quickjs_source}/${source}" COPYONLY)
    endforeach()
    foreach(quickjs_patch IN LISTS quickjs_patches)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
                    "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${quickjs_patch}"
            WORKING_DIRECTORY "${quickjs_source}"
            RESULT_VARIABLE quickjs_patch_result
            ERROR_VARIABLE quickjs_patch_error
        )
        if(NOT quickjs_patch_result STREQUAL "0")
            message(FATAL_ERROR "Could not apply ${quickjs_patch}: ${quickjs_patch_error}")
        endif()
    endforeach()
    file(WRITE "${quickjs_stamp}" "${quickjs_fingerprint}")
endif()
