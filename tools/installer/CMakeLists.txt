# Graphical installer for the Linux / Steam Deck port: a small standalone SDL3 + Dear ImGui
# program that drives scripts/install/tt-install.sh. See README.md in this directory.
#
#   cmake -S tools/installer -B tools/installer/build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build tools/installer/build/host
#
# SDL3 and Dear ImGui are compiled from the SDK checkout's thirdparty sources so no development
# packages are needed; SDL3 is linked statically and loads X11/Wayland at run time.
cmake_minimum_required(VERSION 3.22)
project(tt_installer LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

get_filename_component(TT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(TT_THIRDPARTY_DIR "${TT_REPO_ROOT}/.tools/rexglue-src/thirdparty" CACHE PATH
    "Directory holding the sdl3/, imgui/ and stb/ sources (the SDK checkout's thirdparty/)")
set(TT_INSTALLER_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin" CACHE PATH
    "Where the finished binary is copied")
option(TT_INSTALLER_STATIC_RUNTIME "Link libstdc++ and libgcc statically (portable binary)" ON)

foreach(dir sdl3 imgui stb)
  if(NOT IS_DIRECTORY "${TT_THIRDPARTY_DIR}/${dir}")
    message(FATAL_ERROR "${TT_THIRDPARTY_DIR}/${dir} is missing; run ./build.sh once or set TT_THIRDPARTY_DIR")
  endif()
endforeach()

# --- SDL3, static, with only what a desktop installer needs -------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_GPU OFF CACHE BOOL "" FORCE)
add_subdirectory("${TT_THIRDPARTY_DIR}/sdl3" "${CMAKE_BINARY_DIR}/sdl3" EXCLUDE_FROM_ALL)

# --- Dear ImGui with the SDL3 platform and SDL_Renderer backends --------------------------------
set(IMGUI_DIR "${TT_THIRDPARTY_DIR}/imgui")
add_library(tt_imgui STATIC
  "${IMGUI_DIR}/imgui.cpp"
  "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_tables.cpp"
  "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(tt_imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_definitions(tt_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(tt_imgui PUBLIC SDL3::SDL3-static)

# --- The installer ------------------------------------------------------------------------------
# Fonts (Noto Sans, SIL OFL; licenses/NotoSans) and the project icon (licenses/Icon) are embedded
# with .incbin so the shipped program is one file. Nothing from the game is included.
set(TT_FONT_BLACK "${TT_REPO_ROOT}/project/assets/fonts/NotoSans-Black.ttf")
set(TT_FONT_REGULAR "${TT_REPO_ROOT}/project/assets/fonts/NotoSans-Regular.ttf")
set(TT_ICON "${TT_REPO_ROOT}/project/assets/tabletennis.ico")
foreach(asset TT_FONT_BLACK TT_FONT_REGULAR TT_ICON)
  if(NOT EXISTS "${${asset}}")
    message(FATAL_ERROR "missing asset ${${asset}}")
  endif()
endforeach()
set_source_files_properties(src/assets.S PROPERTIES
  COMPILE_DEFINITIONS "TT_FONT_BLACK=\"${TT_FONT_BLACK}\";TT_FONT_REGULAR=\"${TT_FONT_REGULAR}\";TT_ICON=\"${TT_ICON}\""
  OBJECT_DEPENDS "${TT_FONT_BLACK};${TT_FONT_REGULAR};${TT_ICON}")

add_executable(install-table-tennis
  src/main.cpp
  src/app.cpp
  src/screens.cpp
  src/ui.cpp
  src/backend.cpp
  src/iso_check.cpp
  src/system.cpp
  src/assets.S
  third_party/monocypher/monocypher.c
  third_party/monocypher/monocypher-ed25519.c)
# Monocypher (2-clause BSD or CC0; third_party/monocypher) checks release manifest signatures.
set_source_files_properties(third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
  PROPERTIES LANGUAGE C COMPILE_OPTIONS "-w")
target_include_directories(install-table-tennis PRIVATE "${TT_THIRDPARTY_DIR}/stb")
target_compile_options(install-table-tennis PRIVATE -Wall -Wextra)
set_source_files_properties(src/main.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
target_link_libraries(install-table-tennis PRIVATE tt_imgui SDL3::SDL3-static)
find_package(Threads REQUIRED)
target_link_libraries(install-table-tennis PRIVATE Threads::Threads)
if(TT_INSTALLER_STATIC_RUNTIME)
  target_link_options(install-table-tennis PRIVATE -static-libstdc++ -static-libgcc)
endif()

add_custom_command(TARGET install-table-tennis POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${TT_INSTALLER_OUTPUT_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:install-table-tennis>" "${TT_INSTALLER_OUTPUT_DIR}/"
  COMMENT "Copying install-table-tennis to ${TT_INSTALLER_OUTPUT_DIR}")
