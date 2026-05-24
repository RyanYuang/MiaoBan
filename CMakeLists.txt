# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Keep this OFF unless you explicitly need minimal build,
# because it can break managed-component dependency resolution.
idf_build_set_property(MINIMAL_BUILD OFF)

set(PROJECT_VER "2.2.6")
project(xiaozhi)

# Re-apply local esp-ml307 patches after Component Manager refreshes managed_components/.
if(EXISTS "${CMAKE_SOURCE_DIR}/managed_components/78__esp-ml307")
    execute_process(
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/apply_esp_ml307_patches.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _esp_ml307_patch_result
    )
    if(NOT _esp_ml307_patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply esp-ml307 patches (see patches/README.md)")
    endif()
endif()
