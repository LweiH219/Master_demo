cmake_minimum_required(VERSION 3.28)
project(ecat2can_host LANGUAGES C)

option(BUILD_DJI_MOTOR_TEST "Build the optional DJI motor application example" ON)

# -----------------------------------------------------------------------------
#  ECAT2CAN 主机侧工具包（完全自包含）
#
#  内置 SOEM（third_party/SOEM），不依赖任何外部路径。
#  拷到任何地方、在干净的机器上都能直接构建。
# -----------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

# ---- 内置 SOEM ----------------------------------------------------------
# SOEM 自己的 CMakeLists 会：
#   - 生成 ec_options.h 到构建目录
#   - 通过 target_include_directories(PUBLIC) 把 include / osal / oshw 路径
#     传递给链接它的目标（所以我们不用手写任何 -I）
#   - 按平台 include cmake/${CMAKE_SYSTEM_NAME}.cmake 加入 osal/oshw 源码
#
# 注意：SOEM 的 SOEM_BUILD_SAMPLES 选项包在 if(PROJECT_IS_TOP_LEVEL) 里，
# 作为子目录引入时不会定义，因此 samples 不会被构建。
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SOEM EXCLUDE_FROM_ALL)

# ---- 工具 ---------------------------------------------------------------
add_executable(ecat2can_master
  src/ecat2can_master.c
  src/ecat2can_fieldbus.c
  src/ecat2can_rt.c
)
target_include_directories(ecat2can_master PRIVATE src)
target_link_libraries(ecat2can_master PRIVATE soem m)
# -Wshadow：维护时靠它抓到过两处真实的变量遮蔽（canloop 的轮询计数被内层
# 循环复用、命令参数 rc 遮蔽 main 的返回码），固定开启防止回归。
target_compile_options(ecat2can_master PRIVATE -Wall -Wextra -Wshadow)

if(BUILD_DJI_MOTOR_TEST)
  add_executable(motor_pos src/motor_pos.c src/motor_control.c)
  target_include_directories(motor_pos PRIVATE src)
  target_link_libraries(motor_pos PRIVATE soem m)
  target_compile_options(motor_pos PRIVATE -Wall -Wextra -Wshadow)
endif()

# ---- 安装 ---------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ecat2can_master RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(BUILD_DJI_MOTOR_TEST)
  install(TARGETS motor_pos RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(PROGRAMS scripts/can-linktest.sh DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

message(STATUS "")
message(STATUS "  ECAT2CAN 主机侧工具包")
message(STATUS "  构建类型 : ${CMAKE_BUILD_TYPE}")
message(STATUS "  SOEM     : 内置（third_party/SOEM）")
message(STATUS "")
message(STATUS "  构建后记得给二进制加 raw socket 权限：")
message(STATUS "      sudo setcap cap_net_raw,cap_net_admin+eip <程序>")
message(STATUS "")
