# tabletennis - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tabletennis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/rexglue.cmake)

# Sources
set(TABLETENNIS_SOURCES
    src/main.cpp
    src/intro_skip.cpp
    src/achievement_ui.cpp
    src/pc_settings.cpp
    src/haptics.cpp
    src/system_link.cpp
    src/pc_features.cpp
)

if(WIN32)
    add_executable(tabletennis WIN32 ${TABLETENNIS_SOURCES})
else()
    add_executable(tabletennis ${TABLETENNIS_SOURCES})
endif()

rexglue_setup_target(tabletennis)

# Optimization level for the generated (recompiled) game code only, e.g. -O2 to compile faster.
# Empty keeps the build type's default (-O3 for Release); the SDK and src/ keep their flags.
set(TT_GENERATED_OPT "" CACHE STRING "Optimization flag for the generated game code (empty: build type default)")
if(TT_GENERATED_OPT AND TARGET tabletennis_recomp)
    target_compile_options(tabletennis_recomp PRIVATE ${TT_GENERATED_OPT})
endif()

# The stock v0.10.0 application accepts directories only. Compile our BSD-licensed
# ISO-capable application adapter instead, without editing the installed SDK.
get_target_property(TABLETENNIS_HOST_SOURCES tabletennis SOURCES)
set(TABLETENNIS_SDK_APP "${REXGLUE_SHARE_DIR}/rex_app.cpp")
if(NOT TABLETENNIS_SDK_APP IN_LIST TABLETENNIS_HOST_SOURCES)
    message(FATAL_ERROR "SDK application source not found; cannot enable ISO support")
endif()
list(REMOVE_ITEM TABLETENNIS_HOST_SOURCES "${TABLETENNIS_SDK_APP}")
set_property(TARGET tabletennis PROPERTY SOURCES "${TABLETENNIS_HOST_SOURCES}")
target_sources(tabletennis PRIVATE sdk-overrides/rex_app.cpp)

# Apply the x64 FP exception-mask correction without modifying the installed SDK.
target_include_directories(tabletennis BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk-overrides")
if(TARGET tabletennis_recomp)
    target_include_directories(tabletennis_recomp BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk-overrides")
endif()

# Linux and macOS: the host loads librexruntime through its $ORIGIN / @executable_path rpath
# and dlopens librexgpu-xenos (Vulkan backend) from its own folder, so stage both next to it.
# On macOS the SDK also stages the Vulkan loader and MoltenVK in vulkan/ next to it.
if(UNIX)
    add_custom_command(TARGET tabletennis POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime>
            $<TARGET_FILE:rex::gpu-xenos>
            $<TARGET_FILE_DIR:tabletennis>
        COMMENT "Staging ReXGlue runtime and Vulkan GPU plugin next to tabletennis")
endif()
