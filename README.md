# Master_demo —— ECAT2CAN 主机侧工具包（完全自包含）

Linux 主机侧的 EtherCAT 主站工具，配合跑**港科大 ECAT2CAN 协议**的网关从站使用。

**自包含**：SOEM 已内置在 `third_party/SOEM`，不依赖任何外部路径或环境变量 ——
**拷到任何地方、在任何干净机器上都能一条 `make` 构建完成**。

| 程序 | 用途 |
| --- | --- |
| **`ecat2can_master`** | 协议/链路诊断：扫描、回环、收发、复位、性能基准 |
| **`motor_pos`** | 电机位置闭环（M3508+C620 / GM6020） |
| `scripts/can-linktest.sh` | 用 PCAN 之类的 SocketCAN 设备做 CAN 链路自测 |
| `USAGE.md` | `motor_pos` 的完整使用指南（参数、调参、排错） |

**运行期只依赖系统的 `libc` / `libm`**（`ldd` 可验证），无其他动态库。

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

## 3. 目录结构

```
Master_demo/
├── CMakeLists.txt          顶层构建（把 SOEM 作为子目录引入）
├── Makefile                便捷包装：make / setcap / run / linktest / install
├── README.md
├── USAGE.md
├── .gitignore
├── src/
│   ├── ecat2can_master.c   诊断工具
│   ├── motor_pos.c         电机闭环
│   └── ecat2can_proto.h    192 字节 PDO 协议（与从站逐位对应）
├── scripts/
│   └── can-linktest.sh     CAN 链路自测
└── third_party/
    └── SOEM/               内置 SOEM v2.0.0（仅 Linux 所需部分）
```

> **为什么把 SOEM 内联进来**：SOEM 是 CMake 工程，`add_subdirectory()` 引入后
> 它会自己 `configure_file()` 生成 `ec_options.h`，并通过
> `target_include_directories(PUBLIC ...)` 把 `include` / `osal` / `oshw`
> 路径传播给链接它的目标 —— **我们不需要手写任何 `-I`**，也不需要在
> 目标机器上预装 SOEM。
>
> 只保留了 Linux 平台所需的部分（`osal/linux`、`oshw/linux`），
> 去掉 Windows/RTKernel 支持，整体 **468 KB**。

---

## 4. 构建

```bash
make                 # 编译（内部走 cmake + ninja；没有 ninja 会自动退回 make）
make setcap          # 加 raw socket 权限（需要 sudo 密码，每次重新链接后都要跑）
```

**不需要先装 SOEM，也不需要 source 任何环境脚本。**

### 其他目标

| 命令 | 作用 |
| --- | --- |
| `make` | 编译两个工具到 `build/` |
| `make setcap` | 加 raw socket 权限（逐个文件循环调用 setcap） |
| `make run ARGS="..."` | 加权限后直接跑 `motor_pos` |
| `make linktest ARGS="can0 enp3s0 40"` | 跑 CAN 链路自测 |
| `make install PREFIX=/usr/local` | 安装到系统 |
| `make clean` | 清理 |

### 依赖

| | |
| --- | --- |
| 编译期 | `cmake ≥ 3.28`、C 编译器、`ninja`（可选，没有就用 make） |
| 运行期 | 仅 `libc` / `libm` |

> ⚠️ **每次 `make` 之后必须重跑 `make setcap`** —— 重新链接会生成新文件，
> 内核会把文件 capability 清掉。不加会报
> `打不开网卡 enp3s0（需要 CAP_NET_RAW …）`。
>
> 自查：`getcap build/ecat2can_master` 有输出才是设上了。

网卡准备（EtherCAT 用裸网卡，不能被 NetworkManager 管）：

```bash
sudo nmcli dev set enp3s0 managed no
ip -br link show enp3s0        # 应显示 LOWER_UP
```

---

## 5. `ecat2can_master` 用法

```bash
./build/ecat2can_master <网卡名> <命令> [参数...]
```

| 命令 | 说明 |
| --- | --- |
| `info` | 进 OP，打印从站信息并周期刷新（Ctrl+C 退出） |
| `loop [次数]` | `LOOP_TEST`：192 字节 PDO 往返一致性校验（默认 1000） |
| **`scan [次数]`** | **多通道扫描：只看不发，统计各通道收帧数与出现过的 ID** |
| **`rt [秒数] [通道\|host]`** | **实时性合格性测试：抖动直方图 / 截止期错失 / 主机环境诊断** |
| `work <通道> <id> [b0..b7]` | 在**指定通道**以 1 kHz 下发一帧 CAN |
| **`workall <id> [b0..b7]`** | **在 4 个通道各发一帧（同一拍 PDO）** |
| `fdb [次数]` | 只收不发，打印收到的帧（含 `ch=`） |
| `reset` | `HW_RESET`，让从站复位 |

也支持环境变量 `ECAT2CAN_CH=0..3` 设定默认通道。

### 典型用法

```bash
# ① 先扫一遍：设备到底挂在哪一路？
./build/ecat2can_master enp3s0 scan 3000
#   ┌────────────┬──────────┬─────────────────────────────────┐
#   │ 通道       │ 收到帧数 │ 出现过的 ID                     │
#   ├────────────┼──────────┼─────────────────────────────────┤
#   │ 0 (MCAN0)  │     3000 │ 0x205                           │   ← GM6020 在 CN4
#   │ 1 (MCAN1)  │        0 │ —                               │
#   │ 2 (MCAN9)  │        0 │ —                               │
#   │ 3 (MCAN3)  │        0 │ —                               │
#   └────────────┴──────────┴─────────────────────────────────┘

# ② 往通道 0 发一帧（找设备时可以先 workall 广播到四路）
./build/ecat2can_master enp3s0 work 0 0x123 11 22 33 44 55 66 77 88
./build/ecat2can_master enp3s0 workall 0x1FF 03 E8 00 00 00 00 00 00

# ③ PDO 往返链路自检（不碰 CAN）
./build/ecat2can_master enp3s0 loop
```

> 💡 `scan` 的默认次数是 3000（约 3 秒）。GM6020 / C620 都是 1 kHz 上报，
> 3 秒足够统计出稳定结果；想看得更细就加大次数。

---

## 5.5 `rt` —— 实时性合格性测试

`bench` 是**一次性快照**；`rt` 回答的是**"这台机器能不能长期跑住 1 kHz"**。

```bash
./build/ecat2can_master enp3s0 rt 60 0        # 带从站，60 秒
./build/ecat2can_master enp3s0 rt 60 host     # 纯主机模式，不需要任何硬件
```

### 输出四样东西

**① 主机环境诊断**（在连从站之前就打印，连不上时更需要）

```
── 主机环境 ──────────────────────────────────
  CPU 调频策略   : performance  ✅
  调度策略       : SCHED_IDLE   优先级 0  ❌ 空闲调度，实时性杀手，必须改
                   → 修：chrt -f 80 <程序>
  CPU 亲和性     : 16 个核可用（首个 cpu0）  ⚠️ 用 taskset -c N 绑到隔离核
  isolcpus       : (未设置) ⚠️ 建议加 isolcpus=2,3 nohz_full=2,3
  系统负载       : 3.24 2.03 1.51 ⚠️ 有负载，会推高抖动
```

> **`SCHED_IDLE` / `SCHED_BATCH` 会被明确标红** —— 这两种调度策略意味着进程
> "只在 CPU 没事时才跑"，对实时性是灾难性的。这个检测是实测踩出来的：
> 最初没处理这两个值，显示成了"其他"，把真问题藏起来了。

**② 抖动直方图** —— 只看 p99 看不出**分布形状**

```
  < T-100us     0.280% │
  T-100~-10     1.260% │
  T±10us  ✅   97.219% │########################################
  T+10~+50      1.040% │
  ...
```

**③ 统计** —— 含**追赶拍**单列

```
  最小 / p50     : 1 / 999 us
  p99 / p99.9    : 1017 / 1088 us
  追赶拍         : 4 次（周期 < 500us，绝对时间表超时后补拍）
  有效抖动 σ     : 36.9 us  ← 排除追赶拍，这才是真实抖动
```

> **「追赶拍」是什么**：本工具用**绝对时间表**调度（`clock_nanosleep` +
  `TIMER_ABSTIME`），某一拍超时后，下一拍的截止时刻已经过去，
> `clock_nanosleep` 立即返回 → 出现极短周期。这是**正常行为**，保证了长期平均
> 频率精确（实测漂移 0.001%），但一个 0 值距均值 -999，**会把 σ 完全带偏**。
> 所以 `rt` 单独统计它们，并给出**排除后的有效 σ**。

**④ 明确判决**

```
  截止期错失     : 4 次（> 1100us，即目标 +10%）
  判决：⚠️ **勉强** —— 错失率 0.080%，对增益敏感的控制环会有影响；
       建议做 CPU 隔离 / 提高 RT 优先级 / 关掉后台负载
```

判据：

| 错失率 | 判决 |
| --- | --- |
| 0 | ✅ 优秀 |
| < 0.01% | ✅ 良好 |
| < 1% | ⚠️ 勉强 |
| ≥ 1% | ❌ 不合格 |

### 纯主机模式（`rt <秒数> host`）

**不连 EtherCAT、不需要任何硬件**，只测主机本身的定时与调度能力。

用途：
- 工控机选型 / 调优时，**脱离网关单独评估主机**
- 把「主机抖动」和「链路延迟」彻底分开
- 改内核参数（`isolcpus` / governor）后快速验证效果

### 调优顺序（按收益排序）

1. **CPU 调频** → `cpupower frequency-set -g performance`
2. **调度策略** → `chrt -f 80 <程序>`（别让它是 `SCHED_IDLE`/`SCHED_BATCH`）
3. **CPU 隔离** → 内核参数 `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`，再 `taskset -c 2`
4. **关掉后台负载**

---

## 6. `motor_pos` 用法（摘要）

完整说明见 **[USAGE.md](USAGE.md)**。

```bash
./build/motor_pos enp3s0 --motor gm6020 --ch 0 --id 1 --monitor   # 先只读监视
./build/motor_pos enp3s0 --motor m3508  --ch 3 --id 1 --limit 10
```

| 参数 | 说明 |
| --- | --- |
| `--motor m3508\|gm6020` | 电机类型 |
| `--ch N` | **CAN 通道号 0~3** |
| `--id N` | m3508: 1~8；gm6020: 1~7 |
| `--limit F` | 输出限幅（m3508 = A；gm6020 电流模式 = A） |
| `--monitor` | 只读监视，硬禁使能 |

---

## 7. CAN 链路自测（`can-linktest.sh`）

用一个已知正常的 SocketCAN 设备（如 PCAN-USB）验证网关某一路的收发：

```bash
sudo ip link set can0 type can bitrate 1000000 sample-point 0.75
sudo ip link set can0 up
./scripts/can-linktest.sh can0 enp3s0 40
```

它会自检 `can0` 的网卡层/CAN 层状态、统计 `cansend` 成败、并打印网关收到的帧。
**改线之后先跑这个**，比反复问人要快得多。

> ⚠️ PCAN-USB **每次重新插拔都会掉回 DOWN**，必须先重配 bitrate 再 up。

---

## 8. 排错速查

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

## 9. 本工程的位置与来历

本工程最初放在 KIT 固件 demo（`~/HPM/ecat2can/master/`）里，后来独立出来，
**现已完全自包含**（SOEM 内联），可以整个目录拷到任何项目里单独使用。

> 早期还有一份 `soem_gateway_demo/` 骨架，用的是另一套从未落地的 PDO 布局
> （下行 288B / 上行 304B），**未经硬件验证**，与本工程无关。
> 本工程用的是港科大 ECAT2CAN 的 **192 字节协议**，已在 hpm5e3ykit 单路版和
> ECAN-Lite 四路版上**实测跑通**。

### 自包含性验证

模拟"拷到别处"（只复制会提交的文件到全新目录，用 `env -i` 空环境构建）：

```bash
mkdir -p /tmp/toolkit_test
tar -cf - --exclude=build --exclude=.git . | (cd /tmp/toolkit_test && tar -xf -)
cd /tmp/toolkit_test && env -i PATH=/usr/bin:/bin HOME=/tmp make
ldd build/ecat2can_master        # 只依赖 libc / libm
```

**构建成功，两个工具可直接运行** —— 项目体积 **624 KB**（不含构建产物）。


---

## 10. 许可

| 部分 | 许可 |
| --- | --- |
| 本工具包自身代码（`src/`、`scripts/`） | 随你的项目 |
| **内置的 SOEM（`third_party/SOEM/`）** | **GPLv3 或 RT-Labs 商业许可（双许可）** |

> ⚠️ **把 SOEM 内联进本仓库，意味着分发本工具包时也要遵守 SOEM 的许可。**
>
> SOEM 采用 **GPLv3 / 商业许可双授权**：
> - 如果本工具包**开源分发** → 整体按 **GPLv3** 发布即可（`LICENSE.md` 已随源码保留）
> - 如果**闭源分发** → 需要向 [RT-Labs](https://rt-labs.com/) 购买商业许可，
>   或**不要内联 SOEM**，改用下面的方式让用户自行获取
>
> 仅供内部使用、不对外分发时没有这个顾虑。

### 如果不想内联 SOEM

把 `third_party/SOEM` 从仓库移除，改用 CMake 的 `FetchContent` 在配置阶段自动拉取：

```cmake
include(FetchContent)
FetchContent_Declare(SOEM
  GIT_REPOSITORY https://github.com/OpenEtherCATsociety/SOEM.git
  GIT_TAG        v2.0.0)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SOEM)
```

这样仓库里不含 SOEM 源码，但**首次构建需要联网**。
