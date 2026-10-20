# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

project(flutter_pty_library VERSION 0.0.1 LANGUAGES C)

add_library(flutter_pty_new SHARED
  "flutter_pty.c"
)

set_target_properties(flutter_pty_new PROPERTIES
  PUBLIC_HEADER flutter_pty.h
  OUTPUT_NAME "flutter_pty_new"
)

target_compile_definitions(flutter_pty_new PUBLIC DART_SHARED_LIB)

if (ANDROID)
  # Support Android 15 16k page size.
  target_link_options(flutter_pty_new PRIVATE "-Wl,-z,max-page-size=16384")
endif()

if (OHOS)
  # __OHOS__ 决定 src/flutter_pty.c 选哪份实现
  target_compile_definitions(flutter_pty_new PRIVATE __OHOS__)

  # hilog_ndk.z 供 forkpty_ohos.c 的 PTY_LOGE 使用
  target_link_libraries(flutter_pty_new PRIVATE pthread hilog_ndk.z)

  target_compile_options(flutter_pty_new PRIVATE -fPIC)
endif()
