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
    src/motion.cpp
    src/photo_mode.cpp
    src/native_res.cpp
    src/system_link.cpp
    src/pc_features.cpp
    src/frame_pacer.cpp
    src/button_prompts.cpp
    src/chd_disc.cpp
)

if(WIN32)
    add_executable(tabletennis WIN32 ${TABLETENNIS_SOURCES})
else()
    add_executable(tabletennis ${TABLETENNIS_SOURCES})
endif()

rexglue_setup_target(tabletennis)

# CHD disc images (MAME chdman createdvd) are read through libchdr (BSD 3-Clause, vendored in
# third_party/libchdr with its bundled LZMA, miniz and zstd), built as a static library.
enable_language(C)
set(TT_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libchdr EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${TT_SAVED_BUILD_SHARED_LIBS})
set_target_properties(chdr-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(tabletennis PRIVATE chdr-static)
# The SDK's texture load hook (SDK patch 0017), which button_prompts.cpp uses for PlayStation icons.
target_compile_definitions(tabletennis PRIVATE TT_TEXTURE_LOAD_HOOK=1)

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
