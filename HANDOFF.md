# Master_demo 维护交接文档

> 写给接手维护的同事/agent。读完这一篇，你应该能在 10 分钟内跑通、
> 30 分钟内理解结构、并且在改动时避开本文第 8 节列出的所有坑。

---

## 0. 一句话定位

**Master_demo 是 ECAT2CAN EtherCAT↔CAN 网关的"主机侧测试与诊断工具包"。**
它通过网卡直接发 EtherCAT 过程数据，驱动从站把 CAN 帧发到总线上、并把总线
上的反馈收回来，用来验证从站、测实时性、定位链路故障。

**它不是产品，是仪器。** 设计目标依次是：
1. **能看到真相**（诊断信息优先于好看）
2. **测得准**（实时性指标要有说服力）
3. **自包含**（拷到任何机器都能构建）

---

## 1. 五分钟上手

```bash
cd ~/HPM/Master_demo

# ① 构建
make

# ② 加 raw socket 权限（每次重新编译后都要，会清掉 old capability）
make setcap

# ③ 让网卡脱离 NetworkManager（否则它会被抢走，EtherCAT 帧发不出去）
sudo nmcli dev set enp3s0 managed no

# ④ 验证从站在线 + 能进 OP
./build/ecat2can_master enp3s0 info
```

**正常输出（健康状态）：**
```
扫到 1 个从站
  [1] ECAT_Device    Man=0x00000001 ID=0x00000001 Rev=0x00000001
      端口: 活跃=0x1 (port0)  拓扑=线端(1口)  入口=port0
过程数据映射：下行 192 字节 / 上行 192 字节（1 从站 × 192）
等待 SAFE-OP ... 完成（当前 0x04）
切 OPERATIONAL ... 完成，从站已进 OP
往返  64 us  WKC 3/3  状态 0x08 (OP)  AL=0x0000 No error
```

**四项要看：**
- `扫到 N 个从站` —— 网线/拓扑通
- `WKC N/N` —— 过程数据收发工作计数器，必须满
- `状态 0x08 (OP)` —— 从站进了运行态
- `AL=0x0000` —— 无应用层错误

---

## 2. 代码地图

```
src/
├── ecat2can_master.c   2388 行  ★主程序：所有命令的实现 + main/参数分发
├── ecat2can_fieldbus.c  322 行  ★EtherCAT 会话封装：fb_start/fb_stop/fb_roundtrip
├── ecat2can_fieldbus.h   49 行    FB_OUT/FB_IN/FB_OUT_N/FB_IN_N 取 PDO 指针
├── ecat2can_proto.h     118 行  ★192 字节 PDO 协议定义（与从站逐位对应）
├── ecat2can_rt.c        385 行   实时性测量辅助（直方图、截止期统计）
├── ecat2can_rt.h         13 行
├── rt_probe.h           205 行   probe 命令用的 CSV 探针
├── motor_pos.c          958 行   DJI 电机示例（独立可执行文件）
├── motor_control.c      143 行   电机控制环
└── motor_control.h       89 行

third_party/SOEM/                EtherCAT 主站栈（内置，不依赖系统安装）
docs/                            已有文档，见 docs/README.md
scripts/                         CAN 链路自测、实时性测试脚本
benchmarks/                      主机性能基线档案
```

**改动最常落在这两个文件：**
- **`ecat2can_master.c`** —— 加命令 / 改输出 / 改参数解析
- **`ecat2can_fieldbus.c`** —— 改 EtherCAT 启动流程 / 状态机处理

---

## 3. 构建与部署

### 构建

```bash
make                    # CMake + Ninja，产物在 build/
make clean              # 删除 build/
```

**关键技术点：**
- **SOEM 内置在 `third_party/SOEM`** —— 整个仓库自包含，拷到任何机器都能构建
- **`BUILD_DJI_MOTOR_TEST=ON`（默认）** 时额外产出 `motor_pos`
- **编译器警告开着 `-Wall -Wextra`** —— 别引入新警告

### 权限（⚠️ 每次重新链接后必须重做）

```bash
make setcap
```

**为什么：** 内核会清除重新链接产生的二进制上的 file capability。
**症状：** 程序报「打不开网卡（需要 CAP_NET_RAW）」。

**`Makefile` 里 `setcap` 目标为什么用 `sh -c` 循环：**
`setcap <caps> f1 f2` 会把 `f2` 当第二个 caps 串解析并报错，
必须一个文件一次调用。

### 网卡准备

```bash
sudo nmcli dev set enp3s0 managed no
```

**否则 NetworkManager 会周期性操作网卡（DHCP、链路探测），
导致 EtherCAT 原始帧收发不稳或直接失败。**

---

## 4. 命令全景

**调用格式：`./build/ecat2can_master <网卡> <命令> [参数...]`**

### 4.1 状态与诊断

| 命令 | 作用 | 关键输出 |
|---|---|---|
| **`info`** | **进 OP 并持续刷新状态（Ctrl+C 退出）** | WKC / AL / 往返 / 状态 |
| **`scan [次数]`** | 只看不发，统计 4 路 CAN 各收到多少帧 | 哪一路有设备 |
| **`regs`** | 读 ESC 寄存器（DL_STAT / STATION_ADDR） | 链路与身份 |
| **`reset`** | HW_RESET，让从站复位 | — |

**`info` 的两个实现细节（很重要）：**
1. **它是无限循环** —— 可以作为"后台 EtherCAT 周期发生器"，
   同时用另一台机器/PCAN 做 CAN 侧观察。
2. **它每轮下发 `WORK_MODE` + `frame_count=0`** ——
   因为从站只有 WORK_MODE/ONLY_FDB 才读 CAN 接收 FIFO。
   详见第 7.3 节。

### 4.2 CAN 发送

| 命令 | 帧数 | 输出 | 用途 |
|---|---|---|---|
| **`send <ch> <id> [b0..b7]`** | **默认 1**（`ECAT2CAN_SEND_COUNT=N`）| **逐帧打印** | **精确发一帧/小批量** |
| **`work <ch> <id> [b0..b7]`** | **固定 2000**（2 秒 @1kHz）| 只前几轮 | 压测 |
| **`workall <id> [b0..b7]`** | 2000 × 4 通道 | 只前几轮 | 四路并发 |

**`send` 单帧几乎无法用肉眼/`candump` 观察**（一帧只有 ~130 µs），
**稳定观察请用 `ECAT2CAN_SEND_COUNT=100`。** 程序在 count=1 时会主动提示。

### 4.3 CAN 接收

| 命令 | 作用 |
|---|---|
| **`fdb [次数]`** | ONLY_FDB 模式，只收不发，打印每拍的 `frame_count` |

**读法：**
```
[上行] command=0x03 seq=15 status=0 frame_count=5 error_flags=0x00(无)
                                    ↑ 这一拍从 CAN 总线收到 5 帧
```

### 4.4 延迟与实时性

| 命令 | 需要什么 | 测什么 |
|---|---|---|
| **`looplat <tx> <rx> [秒] [Hz] [负载]`** | **两个 CAN 口物理短接** | 同从站完整回环延迟 |
| **`looplat2 <从站列表> <tx> <rx> [...]`** | 同上 + 多从站 | 多从站同时回环 |
| **`canloop <发站> <发ch> <收站> <收ch> [...]`** | 两从站互联 | 跨从站 CAN 回环 |
| **`bench_lat <通道> <can口> [次数]`** | 对端 CAN 设备 | 端到端延迟 |
| **`bench [次数] [通道]`** | 从站在线 | EtherCAT 往返 / 抖动 / 吞吐 |
| **`rt [秒] [通道\|host]`** | `host` 模式不需硬件 | 主机调度实时性 |
| **`probe <sec> <tx\|ecat> <rx> [Hz] [load]`** | CSV 探针 | 绝对截止期 / RTT |

**`looplat` 的负载参数（第 6 个）：**
```
0 默认  静默基线
4       4 帧/拍 ≈ 60% 总线负载
8       8 帧/拍 ≈ 108% 饱和（会大量丢帧）
```

### 4.5 环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| **`ECAT2CAN_SLAVE`** | 1 | 选择操作哪个从站 |
| **`ECAT2CAN_SCAN_RETRY`** | 6 | 扫描重试次数（每轮 ~400ms）|
| **`ECAT2CAN_CH`** | 3 | 默认 CAN 通道 |
| **`ECAT2CAN_SEND_COUNT`** | 1 | `send` 的发帧数（1~1000000）|
| **`ECAT2CAN_PROBE_CSV`** | — | `probe` 的 CSV 输出路径 |
| **`ECAT2CAN_DUMP_LAYOUT`** | — | 打印 PDO 字节布局 |

### 4.6 通道 ↔ 座子映射

```
ECAN-Lite 四路版（本仓库主要验证对象）
  通道 0 = CN4 = MCAN0
  通道 1 = CN5 = MCAN1
  通道 2 = CN9 = MCAN2
  通道 3 = CN1 = MCAN3

hpm5e3ykit 单路版
  只有通道 3
```

---

## 5. 协议：192 字节 PDO

**定义在 `src/ecat2can_proto.h`，与从站 `application/ecat2can.h` 逐位对应。**

```
偏移   长度   字段
──────────────────────────────────────────
 0      1     command       0x00 LOOP_TEST / 0x01 WORK_MODE / 0x02 HW_RESET / 0x03 ONLY_FDB
 1      1     frame_count   有效帧数 0~17
 2      1     seq           主站下发序号（从站回显）
 3      1     status        0x00 OK / 0x01 ERROR
 4      1     error_flags   位掩码，见下
──────────────────────────────────────────
 5     11     can_frames[0]
16     11     can_frames[1]
...
181    11     can_frames[16]
──────────────────────────────────────────
总计 5 + 17×11 = 192
```

**单个帧槽位（11 字节，`#pragma pack(1)`）：**
```c
typedef struct {
    uint8_t  can_channel;   // 0~3
    uint32_t std_id : 11;   // 0x000~0x7FF
    uint16_t dlc : 4;       // 0~8
    uint8_t  data[8];
} can_frame_struct;
```

**`error_flags` 位定义：**
```
bit0 0x01 BAD_COMMAND   命令字非法
bit1 0x02 BAD_CHANNEL   通道号越界
bit2 0x04 BAD_DLC       DLC > 8
bit3 0x08 BAD_STD_ID    ID > 0x7FF
bit4 0x10 TOO_MANY      frame_count > 17
bit5 0x20 RX_DROPPED    接收 FIFO 溢出
```

**上下行语义不同：**
- **下行**：`frame_count` = 要发到 CAN 的帧数；`can_frames` = 待发帧
- **上行**：`frame_count` = **本拍从 CAN 总线收到的帧数**；`can_frames` = 收到的帧

⚠️ **最容易误解的地方：`frame_count` 在上行是"收到几帧"，不是"发出几帧"。**

---

## 6. 关键设计决策（为什么这么写）

### 6.1 为什么 `fb_start()` 的 SAFE-OP 要重试 10 轮

**SOEM 的 `ecx_statecheck()` 返回时，`slavelist[i].state` 有时还是旧值。**
实际从站已经进了 SAFE-OP，但本地结构体显示 PRE-OP，
于是代码把"已经正常起来的从站"误判成失败，直接放弃整条总线。

**修法：**
```c
for (attempt = 0; attempt < 10; attempt++) {
    ecx_statecheck(ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE / 10);
    if (slave->state == EC_STATE_SAFE_OP) break;
    ecx_readstate(ctx);              // ★ 拉一次真实状态
    if (slave->state == EC_STATE_SAFE_OP) break;
    fb_roundtrip(fb);                // 再喂一拍
    osal_usleep(1000);
}
```

**教训：SOEM 的本地缓存状态不可靠，`ecx_readstate()` 才是真相。**

### 6.2 为什么 `send`/`work` 的 `seq` 起始值要随机化

从站的发送触发条件是：
```c
if (msg.seq != last_processed_master_seq) {
    last_processed_master_seq = msg.seq;
    ecat2can_send_can_frames(...);      // seq 变了才发
}
```

如果主站每次都从 `seq=0` 开始：
```
第一次 send（count=1）: seq=0 ≠ 上次的 0xFF → 发帧 ✓，从站记住 0
第二次 send（count=1）: seq=0 == 记住的 0     → 不发帧 ✗
```

**现象：** "第一次能发，第二次就发不出去"。
**`count>1` 时看不出来**，因为 seq 会从 0 递增到 N，下轮从 0 开始必然不等。

**修法：** 起始值取 `clock_gettime(CLOCK_MONOTONIC).tv_nsec / 1000 & 0xFF`。

### 6.3 为什么 `info` 要下发 `WORK_MODE` + `frame_count=0`

**从站固件 `ecat2can_process()` 的 switch：**
```c
case WORK_MODE:  ... receive_can_frames(); ...   // 只有这两个会读 CAN FIFO
case ONLY_FDB:   ... receive_can_frames(); ...
case LOOP_TEST:  ... （不碰 CAN）
```

原来 `info` 从不设置 `command`，从站按 `LOOP_TEST` 处理、**根本不读 CAN 接收 FIFO**。
于是外部 CAN 设备持续发帧时，从站的 CAN 活动指示灯完全不亮 ——
看起来像"灯坏了"，其实是 `info` 没让从站去读 CAN。

**修法：** `info` 每轮填 `command=WORK_MODE, frame_count=0`
（**纯监听，一个 CAN 帧都不下发**）。

### 6.4 为什么 `looplat` 的 seq 策略是"第一拍变、后续不变"

**一拍要轮询很多次找 CAN 回声，如果每轮都改 seq 就会让从站反复重发同一帧。**

```c
/* 第 1 拍：seq = i（变化 → 从站真发）*/
out->frame_count = 1 + load_n;
out->seq = (uint8_t)i;

/* 后续轮询：frame_count=0、seq 不变（→ 从站不重发）*/
out->frame_count = 0;
out->seq = (uint8_t)i;
```

### 6.5 为什么用 `WKC` 而不是 `memcmp` 验证 PDO

**曾经用过整段 192 字节 `memcmp` 来判断"从站回环是否正确"，
但其中 offset 2/3/4（seq/status/error_flags）是**从站自己写的**，
主站写下去的值从站会覆盖，所以永远不相等。**

**现在 `loop` 命令跳过这 3 个字节再比对。**

### 6.6 为什么 `fb_roundtrip()` 后要立刻检查返回值

它返回 WKC。`< 0` 表示过程数据没回来（EtherCAT 帧丢了）。
**所有测量命令都要先判这个，不能直接读 `in->` 的内容。**

---

## 7. 踩过的坑（改动前务必看）

### 7.1 ⚠️ 别用 `usleep(100000)` 做周期

从站的 **SM 看门狗默认 100 ms**。主站超过 100 ms 没写同步管理器，
从站就掉出 OP 并报 `AL=0x001B (Sync manager watchdog)`，
表现为 `WKC` 从 `3/3` 掉到 `1/3`。

**→ 过程数据必须以 ~1 kHz 持续下发。早先的 `info` 用 100ms 睡眠，
刚好撞上看门狗，把正常从站误判成故障。**

### 7.2 ⚠️ 编译后 capability 会被清掉

每次 `make` 之后都要 `make setcap`。
**症状：** "打不开网卡（需要 CAP_NET_RAW）"。

### 7.3 ⚠️ 上位机进程不能共用一个网卡

**两个 `ecat2can_master` 进程不能同时跑**（都要独占 `enp3s0`）。
但 **PCAN / SocketCAN 是独立设备，可以和它同时跑。**

### 7.4 ⚠️ CAN 单帧是微秒级事件

一帧标准帧 @ 1 Mbps ≈ 130 µs。
- **`candump` 必须提前启动**才能抓到
- **板上的活动灯只亮 50 ms**
- **单帧测试必须用 `ECAT2CAN_SEND_COUNT=100` 才能稳定观察**

### 7.5 ⚠️ 一路 CAN 总线上只有本模块时会 bus-off

MCAN 发帧需要 ACK。如果该路总线上只有网关一个节点 → 没有 ACK →
TEC 累加到 255 → bus-off。

**从站固件有自动恢复，但那一拍会丢帧。**
**测试时必须接一个对端（PCAN 或另一块板）。**

### 7.6 ⚠️ `ncurses`/`\r` 刷新只在终端里启用

`show_progress` 用 `isatty(STDOUT_FILENO)` 判断。
**重定向到文件时不会输出进度行，输出保持干净**（这是刻意的，
方便前后对比）。

### 7.7 ⚠️ setcap 的参数陷阱

```bash
# ❌ 错误用法：f2 会被当成第二个 caps 串
setcap cap_net_raw,cap_net_admin+eip f1 f2

# ✅ 正确：一次一个文件
sudo sh -c 'for f in f1 f2; do setcap cap_net_raw,cap_net_admin+eip "$f"; done'
```

---

## 8. 测试基线（正常应该是什么样）

**主机环境：** CachyOS，内核 7.2.9-2，16 核，governor=performance，
**非 PREEMPT_RT**，shell 继承 SCHED_IDLE。

### 8.1 EtherCAT 往返（`info`）

```
健康:   往返 55~100 us    WKC 3/3    AL=0
```

### 8.2 CAN 回环延迟（`looplat 0 2`，1 kHz，无负载）

```
p50    526~532 us
p90    ~541 us
p99    566~570 us
p99.9  624~636 us
σ      15~18 us
真丢帧 ~0.42~0.59%
```

**`chrt -f 80` 下：**
```
loss   0.127%（降低 78%）
σ      12.8 us
p99.9  588 us
p50    528 us（不变）
```

### 8.3 负载扫描（1 kHz，N 帧/拍）

| N | 丢帧率 | p50 |
|---|---|---|
| 0 | 0.420% | 531 us |
| 2 | 0.433% | — |
| 4 | 0.367% | — |
| 5 | 0.520% | — |
| 6 | 0.507% | — |
| 7 | 0.620% | 662 us |
| **8** | **99.96%** | **805 us** ← 饱和 |

**→ 实际可用上限约 7 帧/拍（≈ 105% 估算负载，估算偏保守）。**

### 8.4 频率扫描

| 频率 | 结果 |
|---|---|
| 100 Hz | 1.13% 丢帧 |
| 500 Hz | 0.81% |
| 1000 Hz | 0.63~0.99% |
| **2000 Hz** | **23787 次跨拍，18.7% 丢帧** |
| 5000 Hz | 零回声 |

**→ 实用上限约 1900 Hz**（往返 520 µs）。

### 8.5 主机实时性（`rt host`）

| 调度 | σ | 截止期错失 |
|---|---|---|
| **`chrt -f 80`** | **2.6 us** | **0** ✅ |
| SCHED_OTHER | 6.7 us | 3 |
| **SCHED_IDLE（默认 shell）** | **25.2 us** | **7（0.070%）** |

**⚠️ 注意：从 GUI 终端启动的 shell 常继承 SCHED_IDLE，
会让实时性测试结果很差 —— 测之前先确认策略。**

---

## 9. 已知问题与待办

### 9.1 已修复（本次）

- ✅ **SAFE-OP 误判**（详见 6.1）
- ✅ **`send` 第二次不发帧**（详见 6.2）
- ✅ **`info` 模式下活动指示灯不亮**（详见 6.3）
- ✅ **SAFE-OP 失败时没有 AL 错误码输出**（原来 `readstate` 了却不打印）
- ✅ **`--help` 缺 `send`/`looplat2`/`canloop`**

### 9.2 待办（按价值排序）

| 优先级 | 事项 | 说明 |
|---|---|---|
| ⭐⭐⭐ | **`info` 加从站停滞检测** | 连续 N 拍 `in->seq` 不变 → 从站应用层卡住。成本 1 次比较 |
| ⭐⭐ | **`send`/`work` 加 seq 回显校验** | `in->seq` 应该等于上一拍的 `out->seq`，不等说明 PDO 错位 |
| ⭐⭐ | **`work` 的 cycles 可配置** | 现在硬编码 2000。建议 `ECAT2CAN_WORK_CYCLES` 环境变量 |
| ⭐ | **`ECAT2CAN_SEND_INTERVAL_MS`** | 现在 `send` 固定 1 kHz，低速周期发送需要这个 |
| ⭐ | **`isolcpus` + PREEMPT_RT 实验** | 残余 0.127% 丢帧的可能优化方向 |
| — | **`looplat` 的 load 估算偏保守** | 实测 7 帧/拍才 0.62%，估算说 8 帧就饱和 |

### 9.3 硬件侧已知问题（不属于本工具，但测试会遇到）

| 问题 | 影响 |
|---|---|
| **某块 ECAN-Lite 的 CAN0 收发器损坏** | TEC 饱和、彻底不能用；CAN1/2/3 正常 |
| **某块板的 PHY0 地址被 strap 成 0** | 不影响 EtherCAT 数据，但 LED 行为异常 |

---

## 10. 故障排查流程

### 10.1 EtherCAT 不通

```
1. ./build/ecat2can_master enp3s0 scan
   ├─ 扫到 0 个 → 检查网线 / 从站供电 / 网卡是否被 NetworkManager 接管
   └─ 扫到 N 个 → 继续

2. ./build/ecat2can_master enp3s0 info
   ├─ "未进入 SAFE-OP" → ★ 看它打印的 AL_CODE
   │     0x0016 = 邮箱配置
   │     0x001D = 输出配置   0x001E = 输入配置
   │     0x0024 = FMMU 配置
   └─ WKC 不满 → 有未枚举的从站
```

### 10.2 CAN 发不出去

```
1. candump <对端接口>          ← 先启动监听
2. ECAT2CAN_SEND_COUNT=100 ./build/ecat2can_master <网卡> send <ch> <id> <data...>
   ├─ candump 收到 → CAN 通路正常 ✓
   └─ 没收到 → 继续

3. 量物理层
   ├─ 收发器 VCC 是否 5V
   ├─ CANH/CANL 是否接反
   ├─ CANH-CANL 之间是否 ~60Ω（终端电阻）
   └─ 总线上是否有对端做 ACK（单节点会 bus-off）
```

### 10.3 CAN 收不到

```
1. ./build/ecat2can_master <网卡> fdb 20000    ← 20 秒监听
2. 另一个终端：for i in $(seq 50); do cansend can0 456#AA; sleep 0.1; done
3. 看 fdb 输出里 frame_count 是否 > 0
   ├─ 有 → 接收正常 ✓
   └─ 没有 → 物理层或 filter 问题
```

### 10.4 程序报"打不开网卡"

```bash
make setcap
# 或者手动
sudo setcap cap_net_raw,cap_net_admin+eip build/ecat2can_master
```

### 10.5 实时性结果很差

```bash
# 先看当前调度策略
chrt -p $$

# GUI 终端常继承 SCHED_IDLE，改回普通或实时
chrt -f 80 ./build/ecat2can_master enp3s0 rt 10 host
```

---

## 11. Git 与发布

**远程：** `https://github.com/LweiH219/Master_demo.git`
**分支：** `main`

```bash
git log --oneline -5
git push                    # 需要 GitHub 凭据
```

**提交约定（沿用现有风格）：**
```
feat: 新增 XXX            ← 新功能
fix: 修复 XXX             ← bug
docs: 更新 XXX 文档
```

**提交前自查清单：**
- [ ] `make` 无新警告（`-Wall -Wextra`）
- [ ] `--help` 与实际命令一致
- [ ] `docs/guide/COMMANDS.md` 同步更新
- [ ] 实测跑过至少 `info` / `scan` / `send`
- [ ] 没有把调试用的 printf 留在关键路径上

---

## 12. 快速参考卡片

```bash
# ---- 构建 ----
make && make setcap

# ---- 网络 ----
sudo nmcli dev set enp3s0 managed no

# ---- 基本检查 ----
./build/ecat2can_master enp3s0 info            # 状态（Ctrl+C 退出）
./build/ecat2can_master enp3s0 scan 3000       # 设备挂哪路
./build/ecat2can_master enp3s0 regs            # ESC 寄存器

# ---- 发 CAN ----
./build/ecat2can_master enp3s0 send 2 0x123 11 22
ECAT2CAN_SEND_COUNT=100 ./build/ecat2can_master enp3s0 send 2 0x123 11 22
./build/ecat2can_master enp3s0 work 2 0x123 11 22 33 44 55 66 77 88
./build/ecat2can_master enp3s0 workall 0x123 11 22

# ---- 收 CAN ----
./build/ecat2can_master enp3s0 fdb 20000

# ---- 延迟 ----
./build/ecat2can_master enp3s0 looplat 0 2                 # 30s @1kHz
./build/ecat2can_master enp3s0 looplat 0 2 15 1000 4       # 15s + 负载
./build/ecat2can_master enp3s0 canloop 2 0 2 2             # 跨通道
./build/ecat2can_master enp3s0 looplat2 1,2 0 2            # 多从站

# ---- 实时性 ----
./build/ecat2can_master enp3s0 rt 10 host                  # 纯主机
chrt -f 80 ./build/ecat2can_master enp3s0 rt 10 host       # 实时调度

# ---- PCAN 侧 ----
ip -details link show can0
sudo ip link set can0 up
candump can0
cansend can0 456#AABBCCDD
```

---

**最后一条建议：这个工具的价值全在"能说清真相"。**
**加命令时优先想"这条信息能不能帮人定位问题"，而不是"输出好不好看"。**
