# tt-chd-read: reads the disc inside a CHD image for project/tools/disc_image.py (see the
# source file). Built with the vendored libchdr; the graphical installer's build includes it,
# and ./build.sh builds it on demand when given a .chd.
#
#   cmake -S tools/chd-read -B build/chd-read -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/chd-read
cmake_minimum_required(VERSION 3.22)
project(tt_chd_read LANGUAGES C)

if(NOT TARGET chdr-static)
  get_filename_component(TT_CHD_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  set(BUILD_SHARED_LIBS OFF)
  set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
  set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${TT_CHD_REPO_ROOT}/project/third_party/libchdr" "${CMAKE_BINARY_DIR}/libchdr"
                   EXCLUDE_FROM_ALL)
endif()

add_executable(tt-chd-read tt_chd_read.c)
target_link_libraries(tt-chd-read PRIVATE chdr-static)
