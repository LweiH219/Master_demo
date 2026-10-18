# ECAT2CAN 主站项目架构

> 文档类型：设计说明  | 适用范围：目录、依赖和测试分层  | 状态：当前

## 项目边界

本项目用于验证 ECAT2CAN 从站，不是通用电机控制器。核心链路是：

```text
测试场景 → 主站测试程序 → SOEM → EtherCAT PDO → ECAT2CAN 从站 → CAN 总线设备
                                      ↑                         ↓
                                      └────── CAN 反馈 PDO ─────┘
```

电机只是 CAN 总线设备的一种。`motor_pos` 用于验证 DJI 电机控制帧和反馈帧，
不应成为协议层或实时性测试的依赖。

## 目录职责

```text
Master_demo/
├── src/
│   ├── ecat2can_proto.h       PDO/CAN 帧协议契约
│   ├── ecat2can_master.c      主站测试入口和测试场景
│   ├── motor_pos.c             DJI 电机应用示例入口
│   ├── motor_control.c/.h      DJI 电机示例适配逻辑
│   └── rt_probe.h              实时性测试辅助接口
├── scripts/                    CAN 链路和主机实时性脚本
├── docs/                       设计和测试说明
├── third_party/SOEM/           EtherCAT 通信依赖
└── build/                      CMake 生成目录，不是源码
```

## 依赖方向

```text
ecat2can_proto.h
       ↑
ecat2can_master.c ───────→ SOEM

motor_pos.c ──────────────→ SOEM
       ↑
motor_control.c/.h
```

协议头只能描述从站协议，不依赖 SOEM、UI 或电机控制。

## 程序定位

### `ecat2can_master`

核心测试程序，负责：

- 从站扫描、状态切换和 PDO 映射
- LOOP_TEST、WORK_MODE、ONLY_FDB、HW_RESET
- CAN 通道扫描和多通道发送
- WKC、周期抖动、吞吐和端到端延迟测试
- 从站错误标志和反馈帧打印

### `motor_pos`

默认随完整项目一起构建的应用示例，用于验证 DJI M3508/GM6020 的 CAN 控制帧和反馈帧。需要只构建主站核心时可以关闭：

```bash
cmake --build --preset default --target clean
cmake --preset default -DBUILD_DJI_MOTOR_TEST=OFF
cmake --build --preset default
```

## 测试分层

1. **软件协议层**：运行 `ecat2can_master <iface> loop`。
2. **CAN 转发层**：运行 `scan`、`work`、`fdb`。
3. **多通道层**：运行 `workall` 和多通道扫描。
4. **实时性层**：运行 `bench`、`rt`、`looplat`、`bench_lat`。
5. **设备应用层**：最后运行 `motor_pos` 或其他 CAN 设备测试。

每一层只依赖前一层通过的结果。这样电机故障不会被误判成从站协议故障。

## 构建目录说明

`build/` 是 CMake 的构建缓存和中间文件：

- 默认配置包含 `ecat2can_master` 和 `motor_pos`。
- 关闭 `BUILD_DJI_MOTOR_TEST` 后只生成 `ecat2can_master`。
- 该目录可以随时删除和重新生成，不应提交到版本库。

推荐使用 `CMakePresets.json` 管理默认构建；核心构建只需在同一个 `build/` 目录临时关闭电机示例。
