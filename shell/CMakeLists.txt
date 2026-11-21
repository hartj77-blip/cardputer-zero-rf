cmake_minimum_required(VERSION 3.16)
project(zeroshell-rf VERSION 0.1.0 LANGUAGES C CXX)

# ZeroShell for the RF edition: the base OS's shell, unchanged, plus the RF
# apps in src/apps and our own launcher list (src/registry.cpp) in place of
# the base's. The base is the pinned submodule in ../base.
#
# This mirrors base/shell/CMakeLists.txt. When the pin moves, check that file
# for changes.
#
# Backends:
#   sim      SDL2 window on a desktop PC (development)
#   device   Linux framebuffer + evdev keyboard (Cardputer Zero)
#   headless no window; renders screens to image files (CI screenshots, tests)
set(ZS_BACKEND "sim" CACHE STRING "sim | device | headless")
set_property(CACHE ZS_BACKEND PROPERTY STRINGS sim device headless)

set(ZEROOS_BASE "${CMAKE_CURRENT_SOURCE_DIR}/../base" CACHE PATH "Checkout of the base OS (cardputer-zero)")
set(BASE_SHELL "${ZEROOS_BASE}/shell")
if(NOT EXISTS "${BASE_SHELL}/src/main.cpp")
  message(FATAL_ERROR "Base OS not found at ${ZEROOS_BASE}. Run: git submodule update --init base")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# LVGL is pinned to the version the base uses.
include(FetchContent)
FetchContent_Declare(lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v9.2.2
  GIT_SHALLOW TRUE)

set(LV_CONF_PATH ${BASE_SHELL}/lv_conf.h CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lvgl)

set(ZS_SDL 0)
set(ZS_DEVICE 0)
if(ZS_BACKEND STREQUAL "sim")
  set(ZS_SDL 1)
elseif(ZS_BACKEND STREQUAL "device")
  set(ZS_DEVICE 1)
endif()
target_compile_definitions(lvgl PUBLIC
  ZS_BACKEND_SDL=${ZS_SDL} ZS_BACKEND_DEVICE=${ZS_DEVICE})

if(ZS_SDL)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
  target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
endif()

# quirc reads QR codes for the base's Camera app (ISC licence), as in the base.
FetchContent_Declare(quirc
  GIT_REPOSITORY https://github.com/dlbeer/quirc.git
  GIT_TAG v1.2
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(quirc)
add_library(quirc STATIC
  ${quirc_SOURCE_DIR}/lib/decode.c ${quirc_SOURCE_DIR}/lib/identify.c
  ${quirc_SOURCE_DIR}/lib/quirc.c ${quirc_SOURCE_DIR}/lib/version_db.c)
target_include_directories(quirc PUBLIC ${quirc_SOURCE_DIR}/lib)

# The radio core: the daemon the RF apps talk to, and the client library.
add_subdirectory(../rf rf)

file(GLOB_RECURSE BASE_SOURCES CONFIGURE_DEPENDS ${BASE_SHELL}/src/*.cpp ${BASE_SHELL}/src/fonts/*.c)
list(FILTER BASE_SOURCES EXCLUDE REGEX "/apps/registry\\.cpp$")
file(GLOB_RECURSE RF_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(zeroshell ${BASE_SOURCES} ${RF_SOURCES})
target_include_directories(zeroshell PRIVATE ${BASE_SHELL}/src src)
target_compile_definitions(zeroshell PRIVATE
  ZS_VERSION="${PROJECT_VERSION}-rf"
  ZS_BACKEND_NAME="${ZS_BACKEND}")
target_compile_options(zeroshell PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(zeroshell PRIVATE lvgl quirc zrfclient pthread m)

install(TARGETS zeroshell RUNTIME DESTINATION bin)
