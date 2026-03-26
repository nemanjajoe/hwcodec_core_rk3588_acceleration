cmake_minimum_required(VERSION 3.16)
project(hwcodec_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)

option(HWCODEC_FFMPEG_RPATH "Use /usr/lib/axcl/ffmpeg/lib in RPATH" ON)
option(HWCODEC_BUILD_APPS "Build standalone CLI apps" ON)
set(HWCODEC_FFMPEG_ROOT "/usr/lib/axcl/ffmpeg" CACHE PATH "Hardware FFmpeg root")
set(HWCODEC_FFMPEG_PKGCONFIG_DIR "${HWCODEC_FFMPEG_ROOT}/lib/pkgconfig" CACHE PATH "FFmpeg pkg-config dir")

# Force pkg-config to prefer the Rockchip FFmpeg first, so both standalone apps
# and ROS wrappers resolve the same hevc_rkmpp-capable libraries.
if(EXISTS "${HWCODEC_FFMPEG_PKGCONFIG_DIR}")
  if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    set(ENV{PKG_CONFIG_PATH} "${HWCODEC_FFMPEG_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
  else()
    set(ENV{PKG_CONFIG_PATH} "${HWCODEC_FFMPEG_PKGCONFIG_DIR}")
  endif()
endif()

pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec>=60)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat>=60)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil>=58)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(MPP REQUIRED IMPORTED_TARGET rockchip_mpp)

find_library(RGA_LIB rga REQUIRED)

add_library(hwcodec_core STATIC
  src/encoder.cpp
  src/decoder.cpp
  src/rkmpp_decoder.cpp
  src/rga_converter.cpp
)

target_include_directories(hwcodec_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${MPP_INCLUDE_DIRS}
)

target_link_libraries(hwcodec_core
  PUBLIC
    ${OpenCV_LIBRARIES}
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::MPP
    ${RGA_LIB}
    pthread
    dl
)

if(HWCODEC_BUILD_APPS)
  add_executable(hw_encode_jpeg apps/hw_encode_jpeg.cpp)
  target_link_libraries(hw_encode_jpeg PRIVATE hwcodec_core)

  add_executable(hw_decode_h265 apps/hw_decode_h265.cpp)
  target_link_libraries(hw_decode_h265 PRIVATE hwcodec_core)

  add_executable(ffmpeg_rkmpp_probe apps/ffmpeg_rkmpp_probe.cpp)

  if(HWCODEC_FFMPEG_RPATH)
    target_link_directories(hw_encode_jpeg PRIVATE ${HWCODEC_FFMPEG_ROOT}/lib)
    target_link_directories(hw_decode_h265 PRIVATE ${HWCODEC_FFMPEG_ROOT}/lib)
    target_link_directories(ffmpeg_rkmpp_probe PRIVATE ${HWCODEC_FFMPEG_ROOT}/lib)
    set_target_properties(hw_encode_jpeg hw_decode_h265 ffmpeg_rkmpp_probe PROPERTIES
      BUILD_RPATH "${HWCODEC_FFMPEG_ROOT}/lib"
      INSTALL_RPATH "${HWCODEC_FFMPEG_ROOT}/lib"
    )
  endif()
endif()

set(HWCODEC_INSTALL_TARGETS hwcodec_core)
if(HWCODEC_BUILD_APPS)
  list(APPEND HWCODEC_INSTALL_TARGETS hw_encode_jpeg hw_decode_h265 ffmpeg_rkmpp_probe)
endif()

install(TARGETS ${HWCODEC_INSTALL_TARGETS}
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include)
