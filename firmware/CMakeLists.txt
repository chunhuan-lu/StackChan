# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.6.0")
add_definitions(-DFIRMWARE_VERSION=\"${PROJECT_VER}\")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# [StackChan fork] Fork overrides live in sdkconfig.defaults.sc and are applied after the upstream defaults.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.sc"
    CACHE STRING "ESP-IDF sdkconfig default files" FORCE)

# Auto-load a local sdkconfig overlay (git-ignored) when present. Lets
# self-hosted / custom deployments pin CONFIG_STACKCHAN_SERVER_URL,
# CONFIG_OTA_URL, etc. without touching committed defaults.
# Manual equivalent: SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.sc;sdkconfig.defaults.local" idf.py build
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.sc;${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local"
        CACHE STRING "ESP-IDF sdkconfig default files (auto-extended with local overlay)" FORCE)
    message(STATUS "StackChan: detected sdkconfig.defaults.local, applying overlay")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stack-chan)

# [StackChan fork] REQ-008: esp-sr's prebuilt libdl_lib.a carries an old esp-dl whose dl_tie728_* conv2d kernels have
# the same 30 names as esp-dl 3.x but other arguments, and the linker used esp-sr's copies for esp-dl too (face
# detection returned garbage, 25x slower). Rename esp-dl's own kernels while compiling it, so each library keeps its own.
if(TARGET __idf_espressif__esp-dl)
    foreach(bits s8 s16)
        foreach(kernel 11cn 33cn hwcn)
            foreach(tail "" _prelu _relu)
                list(APPEND SC_ESPDL_KERNELS dl_tie728_${bits}_conv2d_${kernel}${tail})
            endforeach()
            if(bits STREQUAL "s16")
                foreach(tail _bias _bias_prelu _bias_relu)
                    list(APPEND SC_ESPDL_KERNELS dl_tie728_s16_conv2d_${kernel}${tail})
                endforeach()
            endif()
        endforeach()
    endforeach()
    list(APPEND SC_ESPDL_KERNELS dl_tie728_s8_unaligned_conv2d_11cn dl_tie728_s8_unaligned_conv2d_33cn
                                 dl_tie728_s8_unaligned_conv2d_hwcn)
    foreach(sym IN LISTS SC_ESPDL_KERNELS)
        target_compile_definitions(__idf_espressif__esp-dl PRIVATE ${sym}=espdl3_${sym})
    endforeach()
    # esp-dl asks for -O3; the app partition is nearly full, and the hot kernels are hand-written assembly anyway.
    # SHELL: keeps it from being de-duplicated against the global -Os, so it lands after esp-dl's -O3.
    target_compile_options(__idf_espressif__esp-dl PRIVATE "SHELL:-Os")
endif()
