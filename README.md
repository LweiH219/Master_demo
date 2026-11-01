# Master_demo —— ECAT2CAN 主站测试工具

本项目用于验证 ECAT2CAN EtherCAT↔CAN 从站。默认完整构建会同时生成核心程序 `ecat2can_master` 和 DJI 电机应用示例 `motor_pos`；前者负责从站状态、PDO、CAN 转发、多通道和实时性测试，后者用于验证 DJI 电机报文链路。

## 项目链路

```text
测试命令 → ecat2can_master → SOEM → EtherCAT PDO → ECAT2CAN 从站 → CAN 设备
                                                        ↑
                                                        └── CAN 反馈 PDO
```

协议固定为 192 字节 PDO，协议定义位于 [`src/ecat2can_proto.h`](src/ecat2can_proto.h)。SOEM 已内置在 `third_party/SOEM/`，无需单独安装。

## 目录

```text
src/                    主站、协议头和可选电机示例
scripts/                CAN 链路与主机实时性脚本
docs/guide/             命令、实时性、电机和链路操作指南
docs/design/            架构与依赖说明
docs/reports/           历史测试报告与基线资料
third_party/SOEM/       EtherCAT 依赖
build/                  CMake 构建目录（自动生成）
```

## 构建

默认使用 CMake Preset，所有构建产物统一放在 `build/`：

```bash
cmake --preset default
cmake --build --preset default
```

只构建主站核心时，在同一个目录关闭电机示例：

```bash
cmake --build --preset default --target clean
cmake --preset default -DBUILD_DJI_MOTOR_TEST=OFF
cmake --build --preset default
```

恢复完整构建时重新执行 `cmake --preset default`。`Makefile` 仅为旧脚本提供兼容包装。

重新构建后如需直接访问 EtherCAT 网卡，重新设置权限：

```bash
sudo setcap cap_net_raw,cap_net_admin+eip build/ecat2can_master
```

## 快速开始

```bash
# 查看从站和 PDO 状态
build/ecat2can_master <网卡> info

# 验证 192 字节 PDO 往返
build/ecat2can_master <网卡> loop 1000

# 扫描四路 CAN 反馈
build/ecat2can_master <网卡> scan 3000

# 指定通道发送单帧（逐帧打印往返 / WKC / 从站状态）
build/ecat2can_master <网卡> send 0 0x123 11 22 33 44 55 66 77 88

# 连发多帧（单帧太快，candump 和指示灯都难捕捉，连发才看得清）
ECAT2CAN_SEND_COUNT=100 build/ecat2can_master <网卡> send 0 0x123 11 22

# 压测：连续 2000 帧（约 2 秒）
build/ecat2can_master <网卡> work 0 0x123 11 22 33 44 55 66 77 88

# 查看反馈
build/ecat2can_master <网卡> fdb 100
```

EtherCAT 网卡应处于 `LOWER_UP`，并避免被 NetworkManager 管理。完整参数可直接查看：

```bash
build/ecat2can_master --help
build/motor_pos --help
```

## 文档导航

| 需求                                      | 文档                                                              |
| ----------------------------------------- | ----------------------------------------------------------------- |
| **★★ 接手维护（先读这篇）**               | **[`HANDOFF.md`](HANDOFF.md)**                                   |
| 浏览完整文档分类                          | [`docs/README.md`](docs/README.md)                               |
| 快速查命令和推荐测试顺序                  | [`docs/guide/COMMANDS.md`](docs/guide/COMMANDS.md)               |
| 理解目录、依赖和测试分层                  | [`docs/design/ARCHITECTURE.md`](docs/design/ARCHITECTURE.md)     |
| `looplat`、`rt`、`probe` 和指标解读 | [`docs/guide/RT_GUIDE.md`](docs/guide/RT_GUIDE.md)               |
| DJI `motor_pos` 使用和安全机制          | [`docs/guide/MOTOR_EXAMPLE.md`](docs/guide/MOTOR_EXAMPLE.md)     |
| SocketCAN 链路自测                        | [`docs/guide/CAN_LINKTEST.md`](docs/guide/CAN_LINKTEST.md)       |
| 常见故障、寄存器和排查顺序                | [`docs/guide/TROUBLESHOOTING.md`](docs/guide/TROUBLESHOOTING.md) |
| 主机基准测试档案                          | [`benchmarks/`](benchmarks/)                                     |
| 实测结论和历史报告                        | [`docs/reports/`](docs/reports/)                                 |

建议按“`info` → `loop` → `scan` → `fdb` → `work` → 实时性 → `motor_pos`”顺序定位问题，先确认从站和 CAN 转发，再测试具体设备。
