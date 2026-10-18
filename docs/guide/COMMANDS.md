# 命令速查

> 文档类型：操作指南  | 适用范围：日常构建与 ECAT2CAN 主站命令  | 状态：当前

## 构建

```bash
cmake --preset default
cmake --build --preset default
```

只构建 ECAT2CAN 核心：

```bash
cmake --build --preset default --target clean
cmake --preset default -DBUILD_DJI_MOTOR_TEST=OFF
cmake --build --preset default
```

程序位于 `build/`：

| 程序 | 作用 | 需要从站 |
|---|---|---:|
| `ecat2can_master` | 协议、CAN 转发和实时性测试核心 | 是 |
| `motor_pos` | DJI 电机应用示例 | 是 |

## 运行前准备

```bash
ip -br link show <网卡>
sudo nmcli dev set <网卡> managed no
sudo setcap cap_net_raw,cap_net_admin+eip build/ecat2can_master
```

## 推荐流程

### 1. 确认从站在线

```bash
build/ecat2can_master <网卡> info
```

### 2. 验证 PDO 回环

```bash
build/ecat2can_master <网卡> loop 1000
```

### 3. 扫描 CAN 总线

只收不发，查看设备所在通道和反馈 ID：

```bash
build/ecat2can_master <网卡> scan 3000
```

### 4. 发送一帧 CAN

```bash
build/ecat2can_master <网卡> work <通道> <标准ID> <字节0> ... <字节7> --hz=1000 --frames=2000
```

示例：

```bash
build/ecat2can_master enp3s0 work 3 0x123 11 22 33 44 55 66 77 88 --hz=500 --frames=1000
```

`--hz` 设置 CAN 发送频率（1–1000 Hz），`--frames` 设置每个目标通道发送的帧数。EtherCAT PDO 仍以 1 kHz 运行，以保持从站通信周期；默认值为 1000 Hz、2000 帧。

### 5. 只读反馈

```bash
build/ecat2can_master <网卡> fdb 100
```

### 6. 多通道发送

```bash
build/ecat2can_master <网卡> workall 0x123 00 00 00 00 00 00 00 00 --hz=1000 --frames=2000
```

### 7. 实时性和延迟

```bash
# EtherCAT 往返、周期抖动和 CAN 吞吐
build/ecat2can_master <网卡> bench 10000 3

# 只测主机调度，不需要硬件
build/ecat2can_master <网卡> rt 10 host

# 双 CAN 通道回环，需要两路 CAN 短接
build/ecat2can_master <网卡> looplat 2 3 30 1000 0
```

### 8. DJI 电机示例

先完成前面的链路测试，再运行监视模式：

```bash
build/motor_pos <网卡> --motor gm6020 --ch 3 --id 1 --monitor
```

## 功能查找

| 目标 | 命令 |
|---|---|
| 确认从站是否在线 | `info` |
| 验证 192 字节 PDO | `loop` |
| 查找 CAN 设备所在通道 | `scan` |
| 主站向 CAN 发帧 | `work` / `workall` |
| 查看 CAN 反馈 | `fdb` |
| 测量 EtherCAT/CAN 性能 | `bench` |
| 测量主机调度 | `rt ... host` |
| 测量 CAN 回环延迟 | `looplat` |
| 测量 PCAN 端到端延迟 | `bench_lat` |
| 让从站复位 | `reset` |
| 读取 ESC 寄存器 | `regs` |
| 观察 DJI 电机反馈 | `motor_pos --monitor` |

完整参数以程序帮助为准：

```bash
build/ecat2can_master --help
build/motor_pos --help
```

## 故障定位顺序

```text
网卡/权限
  → EtherCAT 从站扫描
  → SAFEOP/OP 状态
  → PDO 回环
  → CAN 通道扫描
  → CAN ID/DLC/数据
  → 实时性和负载
  → 电机应用
```

电机示例报错时，不要先调 PID；先用 `info`、`loop`、`scan` 和 `fdb` 确认
ECAT2CAN 链路正常。
