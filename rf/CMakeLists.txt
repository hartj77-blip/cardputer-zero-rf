cmake_minimum_required(VERSION 3.16)
project(zrf C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(RTLSDR IMPORTED_TARGET librtlsdr)
endif()

# FT8 decoding uses ft8_lib by Karlis Goba (MIT licence), pinned to one
# commit and downloaded at configure time. It ships no CMake build of its
# own, so its sources are compiled here. Set FETCHCONTENT_SOURCE_DIR_FT8_LIB
# to build from a local checkout instead of downloading.
include(FetchContent)
FetchContent_Declare(ft8_lib
    GIT_REPOSITORY https://github.com/kgoba/ft8_lib.git
    GIT_TAG 9fec6ca39886edbf96f4f5e71edc76da5074e871)
FetchContent_GetProperties(ft8_lib)
if(NOT ft8_lib_POPULATED)
    FetchContent_Populate(ft8_lib)
endif()
add_library(ft8lib STATIC
    ${ft8_lib_SOURCE_DIR}/ft8/constants.c
    ${ft8_lib_SOURCE_DIR}/ft8/crc.c
    ${ft8_lib_SOURCE_DIR}/ft8/decode.c
    ${ft8_lib_SOURCE_DIR}/ft8/encode.c
    ${ft8_lib_SOURCE_DIR}/ft8/ldpc.c
    ${ft8_lib_SOURCE_DIR}/ft8/message.c
    ${ft8_lib_SOURCE_DIR}/ft8/text.c
    ${ft8_lib_SOURCE_DIR}/common/monitor.c
    ${ft8_lib_SOURCE_DIR}/fft/kiss_fft.c
    ${ft8_lib_SOURCE_DIR}/fft/kiss_fftr.c)
target_include_directories(ft8lib PUBLIC ${ft8_lib_SOURCE_DIR})
target_compile_definitions(ft8lib PRIVATE HAVE_STPCPY)
target_link_libraries(ft8lib PUBLIC m)

add_library(zrfcore STATIC
    src/ft8.cpp
    src/dsp/demod.cpp
    src/dsp/spectrum.cpp
    src/sdr/source.cpp
    src/sdr/sim_source.cpp
    src/sdr/file_source.cpp)
target_include_directories(zrfcore PUBLIC src)
target_link_libraries(zrfcore PUBLIC Threads::Threads ft8lib)
if(RTLSDR_FOUND)
    target_sources(zrfcore PRIVATE src/sdr/rtlsdr_source.cpp)
    target_compile_definitions(zrfcore PUBLIC ZRF_HAVE_RTLSDR)
    target_link_libraries(zrfcore PUBLIC PkgConfig::RTLSDR)
    message(STATUS "RTL-SDR support: on")
else()
    message(STATUS "RTL-SDR support: off (librtlsdr not found); sim and file sources only")
endif()

# What a UI needs to talk to the daemon; no DSP, no receiver code.
add_library(zrfclient STATIC src/client.cpp src/adsb.cpp src/aprs.cpp src/adif.cpp)
target_include_directories(zrfclient PUBLIC src)

add_executable(zrfd src/zrfd.cpp)
target_link_libraries(zrfd zrfcore)

# Stands in for rtl_433 when the radio is simulated.
add_executable(zrf-sim-sensors src/sim_sensors.cpp)
add_executable(zrf-sim-aircraft src/sim_aircraft.cpp)
target_link_libraries(zrf-sim-aircraft zrfclient)
add_executable(zrf-sim-aprs src/sim_aprs.cpp)

add_executable(zrf src/zrf.cpp)
target_include_directories(zrf PRIVATE src)

add_executable(test_dsp tests/test_dsp.cpp)
target_link_libraries(test_dsp zrfcore zrfclient)

enable_testing()
add_test(NAME dsp COMMAND test_dsp)
add_test(NAME integration COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh $<TARGET_FILE_DIR:zrfd>)

install(TARGETS zrfd zrf zrf-sim-sensors zrf-sim-aircraft zrf-sim-aprs RUNTIME DESTINATION bin)
