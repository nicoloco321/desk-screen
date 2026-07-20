# Desktop emulator for the desk display: renders the exact firmware UI
# (firmware/src/ui.cpp) at 1920x480 in an SDL window, with fake data.
#
#   brew install sdl2 cmake        # once, macOS
#   cmake -B build && cmake --build build -j
#   ./build/desk-screen-emulator --mode split
#
# LovyanGFX is fetched automatically (same library the ESP32-P4 build uses),
# so what you see here is the same renderer the panel will run.

cmake_minimum_required(VERSION 3.16)
project(desk_screen_emulator CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
# SOURCE_SUBDIR points at a directory with no CMakeLists so MakeAvailable
# only downloads the sources (LovyanGFX's own CMakeLists is an ESP-IDF
# component; we compile the files we need ourselves below).
FetchContent_Declare(
  lovyangfx
  GIT_REPOSITORY https://github.com/lovyan03/LovyanGFX.git
  GIT_TAG        1.2.25
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  cmake_download_only
)
FetchContent_MakeAvailable(lovyangfx)

add_definitions(-DLGFX_SDL)

file(GLOB LGFX_SRC CONFIGURE_DEPENDS
  ${lovyangfx_SOURCE_DIR}/src/lgfx/Fonts/efont/*.c
  ${lovyangfx_SOURCE_DIR}/src/lgfx/Fonts/IPA/*.c
  ${lovyangfx_SOURCE_DIR}/src/lgfx/utility/*.c
  ${lovyangfx_SOURCE_DIR}/src/lgfx/v1/*.cpp
  ${lovyangfx_SOURCE_DIR}/src/lgfx/v1/misc/*.cpp
  ${lovyangfx_SOURCE_DIR}/src/lgfx/v1/panel/Panel_Device.cpp
  ${lovyangfx_SOURCE_DIR}/src/lgfx/v1/panel/Panel_FrameBufferBase.cpp
  ${lovyangfx_SOURCE_DIR}/src/lgfx/v1/platforms/sdl/*.cpp
)

add_executable(desk-screen-emulator
  main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src/ui.cpp
  ${LGFX_SRC}
)

target_include_directories(desk-screen-emulator PRIVATE
  ${lovyangfx_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src
)

find_package(SDL2 REQUIRED COMPONENTS SDL2)
if(APPLE)
  target_link_libraries(desk-screen-emulator PRIVATE SDL2::SDL2)
else()
  include_directories(${SDL2_INCLUDE_DIRS})
  target_link_libraries(desk-screen-emulator PRIVATE ${SDL2_LIBRARIES} pthread)
endif()
