# ecat2can_host —— ECAT2CAN 主机侧工具（独立工程）

Linux 主机侧的 EtherCAT 主站工具，配合跑**港科大 ECAT2CAN 协议**的网关从站使用。

**这是一个独立工程**：不依赖任何具体的固件 demo 目录，只要从站实现的是
192 字节 PDO 协议（见 `ecat2can_proto.h`）即可通用。

| 程序 | 用途 |
| --- | --- |
| **`ecat2can_master`** | 协议/链路诊断：扫描、回环、收发、复位 |
| **`motor_pos`** | 电机位置闭环（M3508+C620 / GM6020） |
| `can-linktest.sh` | 用 PCAN 之类的 SocketCAN 设备做 CAN 链路自测 |
| `USAGE.md` | `motor_pos` 的完整使用指南（参数、调参、排错） |

---

## 1. 支持的从站

| 固件 | 板子 | CAN 通道 |
| --- | --- | --- |
| 港科广 `ECAT2CAN_5e3y`（原版） | 港科广自制板 | 0~3 四路 |
| `Tide_ECAN`（本仓库移植版） | **ECAN-Lite** | 0~3 四路 |
| `ecat2can`（单路改编版） | hpm5e3ykit + 外接收发器 | 仅 **3** |

从站会把**四路 CAN 总线的收帧**都塞进上行 PDO（每帧带 `can_channel`），
所以主站**不需要预先知道设备插在哪个座子上** —— 用 `scan` 一看便知。

---

## 2. 通道 ↔ 物理座子

| 通道号 | ECAN-Lite 座子 | hpm5e3ykit |
| --- | --- | --- |
| `0` | **CN4** | — |
| `1` | **CN5** | — |
| `2` | **CN9** | — |
| `3` | **CN1** | ✅ 唯一一路 |

> ⚠️ **四个座子是四条独立的 CAN 总线** —— 各自独立的 MCAN 控制器和收发器，
> 彼此电气隔离。设备插在 CN4 就只会出现在通道 0，不会串到别的通道。

---

## 3. 构建

```bash
make                 # 编译
make setcap          # 加 raw socket 权限（需要 sudo 密码，每次重新链接后都要跑）
```

SOEM 依赖默认在 `~/HPM/tools/SOEM`，换位置用 `make SOEM_DIR=/path/to/SOEM`。

> ⚠️ **每次 `make` 之后必须重跑 `make setcap`** —— 重新链接会生成新文件，
> 内核会把文件 capability 清掉。不加会报
> `打不开网卡 enp3s0（需要 CAP_NET_RAW …）`。
>
> 自查：`getcap ecat2can_master` 有输出才是设上了。

网卡准备（EtherCAT 用裸网卡，不能被 NetworkManager 管）：

```bash
sudo nmcli dev set enp3s0 managed no
ip -br link show enp3s0        # 应显示 LOWER_UP
```

---

## 4. `ecat2can_master` 用法

```bash
./ecat2can_master <网卡名> <命令> [参数...]
```

| 命令 | 说明 |
| --- | --- |
| `info` | 进 OP，打印从站信息并周期刷新（Ctrl+C 退出） |
| `loop [次数]` | `LOOP_TEST`：192 字节 PDO 往返一致性校验（默认 1000） |
| **`scan [次数]`** | **多通道扫描：只看不发，统计各通道收帧数与出现过的 ID** |
| `work <通道> <id> [b0..b7]` | 在**指定通道**以 1 kHz 下发一帧 CAN |
| **`workall <id> [b0..b7]`** | **在 4 个通道各发一帧（同一拍 PDO）** |
| `fdb [次数]` | 只收不发，打印收到的帧（含 `ch=`） |
| `reset` | `HW_RESET`，让从站复位 |

也支持环境变量 `ECAT2CAN_CH=0..3` 设定默认通道。

### 典型用法

```bash
# ① 先扫一遍：设备到底挂在哪一路？
./ecat2can_master enp3s0 scan 3000
#   ┌────────────┬──────────┬─────────────────────────────────┐
#   │ 通道       │ 收到帧数 │ 出现过的 ID                     │
#   ├────────────┼──────────┼─────────────────────────────────┤
#   │ 0 (MCAN0)  │     3000 │ 0x205                           │   ← GM6020 在 CN4
#   │ 1 (MCAN1)  │        0 │ —                               │
#   │ 2 (MCAN9)  │        0 │ —                               │
#   │ 3 (MCAN3)  │        0 │ —                               │
#   └────────────┴──────────┴─────────────────────────────────┘

# ② 往通道 0 发一帧（找设备时可以先 workall 广播到四路）
./ecat2can_master enp3s0 work 0 0x123 11 22 33 44 55 66 77 88
./ecat2can_master enp3s0 workall 0x1FF 03 E8 00 00 00 00 00 00

# ③ PDO 往返链路自检（不碰 CAN）
./ecat2can_master enp3s0 loop
```

> 💡 `scan` 的默认次数是 3000（约 3 秒）。GM6020 / C620 都是 1 kHz 上报，
> 3 秒足够统计出稳定结果；想看得更细就加大次数。

---

## 5. `motor_pos` 用法（摘要）

完整说明见 **[USAGE.md](USAGE.md)**。

```bash
./motor_pos enp3s0 --motor gm6020 --ch 0 --id 1 --monitor   # 先只读监视
./motor_pos enp3s0 --motor m3508  --ch 3 --id 1 --limit 10
```

| 参数 | 说明 |
| --- | --- |
| `--motor m3508\|gm6020` | 电机类型 |
| `--ch N` | **CAN 通道号 0~3** |
| `--id N` | m3508: 1~8；gm6020: 1~7 |
| `--limit F` | 输出限幅（m3508 = A；gm6020 电流模式 = A） |
| `--monitor` | 只读监视，硬禁使能 |

---

## 6. CAN 链路自测（`can-linktest.sh`）

用一个已知正常的 SocketCAN 设备（如 PCAN-USB）验证网关某一路的收发：

```bash
sudo ip link set can0 type can bitrate 1000000 sample-point 0.75
sudo ip link set can0 up
./can-linktest.sh can0 enp3s0 40
```

它会自检 `can0` 的网卡层/CAN 层状态、统计 `cansend` 成败、并打印网关收到的帧。
**改线之后先跑这个**，比反复问人要快得多。

> ⚠️ PCAN-USB **每次重新插拔都会掉回 DOWN**，必须先重配 bitrate 再 up。

---

## 7. 排错速查

| 现象 | 原因 / 处理 |
| --- | --- |
| `打不开网卡` | 没 setcap（重新 `make` 后要重设） |
| `没扫到 EtherCAT 从站` | 网关没上电 / 网线没插 Port0 / 网卡没有 `LOWER_UP` |
| `期望 PDO 192/192，实际 x/y` | 从站固件和本工具的协议版本不匹配 |
| `scan` 四路全是 0 | 设备没供电 / 接线通断 / 波特率不是 1 Mbps / 终端电阻缺失 |
| 某一路 0、其他正常 | 那一路上没接设备，或该路硬件有问题 |
| `cansend` 大量失败 | 抢不到 ACK → 控制器重传堆积、队列满 → 查终端电阻与接线 |
| `work` 后那一路 bus-off | 总线上只有网关一个节点，没人 ACK。`Tide_ECAN` 固件带自动恢复 |

### 一个能省大量来回的判据

判断"是没信号还是信号错了"，直接看 MCAN 寄存器：

| 寄存器 | 含义 |
| --- | --- |
| `ECR.REC` 持续上升 + `PSR.LEC` 报错 | **信号来了但解不开** → CANH/CANL 接反、波特率不对 |
| `ECR = 0` + `PSR.ACT = Idle` | **根本没有信号** → 线没通、对端没发、收发器没工作 |
| `PSR.ACT = 2 (Receiver)` + `LEC = 0` | **正在正常收帧** ✅ |

> `PSR` 位定义：`ACT` = bits[4:3]（掩码 `0x18`），`LEC` = bits[2:0]（掩码 `0x07`），
> `ECR`：`TEC` = bits[7:0]、`REC` = bits[14:8]。

---

## 8. 本工程的位置与来历

本工程原先放在 KIT 固件 demo（`~/HPM/ecat2can/master/`）里，
现已**独立**出来，位于 **`~/HPM/Master_demo/`**。

> 同仓库 `~/HPM/Tide_Ecat2CAN/master/soem_gateway_demo/` 是一份**早期骨架**，
> 用的是另一套从未落地的 PDO 布局（下行 288B / 上行 304B），**未经硬件验证**。
> 本工程用的是港科大 ECAT2CAN 的 **192 字节协议**，已在
> hpm5e3ykit 单路版和 ECAN-Lite 四路版上**实测跑通**。
