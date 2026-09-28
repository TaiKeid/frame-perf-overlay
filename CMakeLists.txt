cmake_minimum_required(VERSION 3.16)
project(frame-perf-overlay VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# SteamVR に同梱の OpenVR ライブラリ（Frame の SteamOS 上のパス）
set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "libopenvr_api.so のあるディレクトリ")

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(LIBNL REQUIRED IMPORTED_TARGET libnl-genl-3.0)

add_executable(frame-perf-overlay
  src/main.cpp
  src/placement.cpp
  src/clock.cpp
  src/config.cpp
  src/json.cpp
  src/sensors.cpp
  src/panel.cpp
  src/vr_overlay.cpp
  src/vk_texture.cpp
  src/draw.cpp
  src/settings_panel.cpp
  src/gpu_usage.cpp
  src/wifi_link.cpp
  src/i18n.cpp
  src/autostart.cpp
  src/theme.cpp
  vendor/frame-updater/cpp/update_check.cpp
)
target_include_directories(frame-perf-overlay PRIVATE third_party/openvr vendor/frame-updater/cpp)
target_compile_options(frame-perf-overlay PRIVATE -Wall -Wextra)
# --version で出すバージョン（上の project(VERSION) から）
target_compile_definitions(frame-perf-overlay PRIVATE FRAME_PERF_OVERLAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(frame-perf-overlay PRIVATE
  PkgConfig::CAIRO
  PkgConfig::FREETYPE
  PkgConfig::VULKAN
  PkgConfig::LIBNL
  "${OPENVR_LIB_DIR}/libopenvr_api.so"
)
# 実行時も SteamVR 同梱のライブラリを使う
# （ビルドした時点から INSTALL_RPATH だけにする。ビルド用の RPATH だと末尾に空の要素が付き、
#   空の要素はカレントディレクトリからもライブラリを探す意味になるため）
set_target_properties(frame-perf-overlay PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "${OPENVR_LIB_DIR}"
)

install(TARGETS frame-perf-overlay RUNTIME DESTINATION bin)

# テスト（手首への固定の計算・時計・設定ファイル・性能パネルの高さ・設定パネルの当たり判定）。ctest --test-dir build で動かす。
# リリースのビルド（scripts/package.sh）は -DBUILD_TESTING=OFF でテストを作らない
include(CTest)
if(BUILD_TESTING)
  add_executable(wrist-clock-test
    tests/wrist_clock_test.cpp
    src/placement.cpp
    src/clock.cpp
    src/config.cpp
    src/json.cpp
    src/panel.cpp
    src/settings_panel.cpp
    src/draw.cpp
    src/i18n.cpp
    src/theme.cpp
    src/wifi_link.cpp
  )
  target_include_directories(wrist-clock-test PRIVATE src third_party/openvr vendor/frame-updater/cpp)
  target_compile_options(wrist-clock-test PRIVATE -Wall -Wextra)
  target_link_libraries(wrist-clock-test PRIVATE PkgConfig::CAIRO PkgConfig::FREETYPE PkgConfig::LIBNL)
  add_test(NAME wrist-clock COMMAND wrist-clock-test)
endif()
