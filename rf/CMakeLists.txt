cmake_minimum_required(VERSION 3.16)
project(zrf CXX)

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

add_library(zrfcore STATIC
    src/dsp/spectrum.cpp
    src/sdr/source.cpp
    src/sdr/sim_source.cpp
    src/sdr/file_source.cpp)
target_include_directories(zrfcore PUBLIC src)
target_link_libraries(zrfcore PUBLIC Threads::Threads)
if(RTLSDR_FOUND)
    target_sources(zrfcore PRIVATE src/sdr/rtlsdr_source.cpp)
    target_compile_definitions(zrfcore PUBLIC ZRF_HAVE_RTLSDR)
    target_link_libraries(zrfcore PUBLIC PkgConfig::RTLSDR)
    message(STATUS "RTL-SDR support: on")
else()
    message(STATUS "RTL-SDR support: off (librtlsdr not found); sim and file sources only")
endif()

add_executable(zrfd src/zrfd.cpp)
target_link_libraries(zrfd zrfcore)

add_executable(zrf src/zrf.cpp)
target_include_directories(zrf PRIVATE src)

add_executable(test_dsp tests/test_dsp.cpp)
target_link_libraries(test_dsp zrfcore)

enable_testing()
add_test(NAME dsp COMMAND test_dsp)
add_test(NAME integration COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh $<TARGET_FILE_DIR:zrfd>)

install(TARGETS zrfd zrf RUNTIME DESTINATION bin)
