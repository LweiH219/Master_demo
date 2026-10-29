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

### 4. 发送 CAN 帧

#### 单帧 `send`

`send` 默认只发 1 帧，并且**逐帧**打印往返时间、WKC、从站状态和本拍从 CAN
收到的帧数，适合"我就要发这一帧，看清楚到底发生了什么"的场合。

```bash
build/ecat2can_master <网卡> send <通道> <标准ID> <字节0> ... <字节7>
```

示例：

```bash
build/ecat2can_master enp3s0 send 3 0x123 11 22 33 44 55 66 77 88
```

输出样例：

```text
SEND：通道 3  ID 0x123  DLC 8  data = 11 22 33 44 55 66 77 88  共 1 帧
  第   1 帧  往返  55 us  WKC 3/3  从站状态 0x00  本拍从 CAN 收到 0 帧
完成：1/1 帧 WKC 正常（从站在这些拍里共收到 0 帧 CAN 报文）
```

> **单帧很难观察**：一帧标准帧在 1 Mbps 总线上只持续约 130 µs，`candump`
> 必须提前启动才抓得到，板上的活动指示灯也只亮 50 ms。要稳定观察发送，
> 请用下面的多帧方式。
>
> 另外，输出里的"本拍从 CAN 收到 N 帧"是**接收**指标（从站这一拍从总线上
> 收到了几帧），不是发送计数。

#### 多帧 `send` + `ECAT2CAN_SEND_COUNT`

帧数用环境变量控制，不占用 8 个数据字节的参数位置：

```bash
# 连发 100 帧（约 100 ms 持续流量，candump 和指示灯都能稳定观察到）
ECAT2CAN_SEND_COUNT=100 build/ecat2can_master enp3s0 send 3 0x123 11 22 33 44 55 66 77 88

# 连发 15000 帧（约 15 秒）
ECAT2CAN_SEND_COUNT=15000 build/ecat2can_master enp3s0 send 3 0x123 11 22
```

配合另一台 CAN 分析仪观察：

```bash
# 终端 1：先启动监听
candump can0

# 终端 2：连发
ECAT2CAN_SEND_COUNT=100 build/ecat2can_master enp3s0 send 3 0x123 11 22 33 44 55 66 77 88
```

#### 压测 `work`

`work` 固定发 2000 轮（约 2 秒、1 kHz），只打印前几轮，用来压测和检查长时稳定性：

```bash
build/ecat2can_master <网卡> work <通道> <标准ID> <字节0> ... <字节7>
build/ecat2can_master enp3s0 work 3 0x123 11 22 33 44 55 66 77 88
```

### 5. 只读反馈

```bash
build/ecat2can_master <网卡> fdb 100
```

### 6. 多通道发送

```bash
build/ecat2can_master <网卡> workall 0x123 00 00 00 00 00 00 00 00
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
| 主站向 CAN 发**单帧**（逐帧打印细节） | `send` |
| 主站向 CAN 连续发帧 / 压测 | `work`（2000 帧） / `workall`（四路各一帧）|
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
