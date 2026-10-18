# Master_demo：主机配置选择与 EtherCAT/CAN 端到端验证

> 文档类型：历史测试报告  | 测试日期：见正文  | 状态：历史记录


日期：2026-10-15（北京时间）；主机 `tide@10.191.242.222`。项目 `/home/tide/Master_demo`。

## 推荐配置

在本次实测的配置中，推荐以下组合用于当前 **1 kHz、单从站、CAN0→CAN2 物理回环**：

| 项目 | 推荐值 |
| --- | --- |
| 内核 | 当前 Ubuntu PREEMPT_RT `6.8.1-1060-realtime` |
| CPU 调频 | `intel_pstate=active`，所有 policy 为 `performance` |
| 睿频 | `no_turbo=1`，运行控制任务时关闭 |
| 应用线程 | `SCHED_FIFO 40`，绑定逻辑 CPU4（P 核） |
| 网卡接收 | `enp3s0/threaded=1`，NAPI 线程 `SCHED_FIFO 90`，绑定 CPU6 |
| 网卡中断 | 保留实际 IRQ134→CPU6；启动脚本检查此拓扑 |
| 网卡 | r8169 / RTL8168h，100 Mb/s Full；现有 `rx-usecs=0, rx-frames=1` |
| 测试内存 | `mlockall(MCL_CURRENT|MCL_FUTURE)`，预触碰样本内存和栈 |
| 周期 | `CLOCK_MONOTONIC`、绝对时间表、1000 µs，严格按计划释放时间判定截止期 |

这里的“推荐”是已测试候选中的工程选择，不是所有可能配置的全局最优，也不是硬实时保证。CPU4 与 CPU6 是不同物理 P 核；其 SMT 兄弟 CPU5、CPU7 尚未隔离。关闭睿频降低了热风险，但真实控制计算量较大时需要复核执行时间。

以命令作用域启用配置，退出自动恢复：

```bash
cd /home/tide/Master_demo
sudo env ECAT2CAN_PROBE_CSV="$PWD/build/e2e-repeat.csv" \
  ./scripts/rt-control-run.sh \
  ./build/ecat2can_master enp3s0 probe 60 0 2 1000 4
```

参数：60 秒、CAN0 发→CAN2 收、1000 Hz、每周期另加 4 帧背景数据。这个脚本的 profile 专为本机验证；网卡 IRQ 实际位置变化时会拒绝使用，要求重新评估。它不会修改启动参数或自动开机服务。

## 测试拓扑与口径

用户确认：目前只有一个从站，CAN0 与 CAN2 的 CANH/CANL 短接，无电机或驱动器。读取的 PDO 为上下行各 192 字节，完整过程数据期望 WKC 为 3。

测量路径：主机发送→EtherCAT→从站 CAN0→真实 CAN 物理线→CAN2→EtherCAT→主机读取匹配回声。这里报告 **完整回环 RTT**，以及 **计划释放时刻到收到回声的响应时间**；不将 RTT/2 当作已测单程延迟。轮询引入一个 EtherCAT 交换粒度的检测延迟；它不是 CAN 总线准确到达时间。

新增 `probe` 命令使用 CAN ID `0x600`，背景帧 `0x601..0x610`，每帧携带 32 位序号和每次运行随机 token，校验 DLC=8 与全部 payload。避免沿用旧工具的 `0x200` 电机控制 ID。它仍然只能在明确允许发包的回环环境使用。

每个计划周期都有 CSV：唤醒延迟、CPU、首次 EtherCAT 交换耗时、完整 WKC 异常数、RTT、绝对响应时间、截止期状态和后续观测到的迟到回声。**严格 1 ms 截止期未满足**包括未匹配回声、晚匹配和错过释放时刻；不把无回声直接归因成物理 CAN 丢帧。延迟分位数仅基于匹配成功样本，必须同时看失约率，防止幸存者偏差。

预热 200 拍，测量段无打印、无动态分配，结束后写 CSV；测量页错误计数记录在日志。非满 WKC 的 PDO 不作为有效匹配。背景帧按“原始周期序号 + ID”跨轮询去重，结束后再排空 20 ms。排空只代表这段观测窗口内的接收覆盖，不证明物理线路逐帧送达。

从站现有 DC 同步配置仍为 1 ms。本次仅使用 1000 Hz；没有以更高主机频率冒充已验证的 DC 频率。

## 为什么选择它

先后比较普通调度、FIFO80 自由迁移、关闭睿频、CPU2/CPU4 绑定、仅局部 performance、低 timer slack、FIFO40，以及独立 NAPI。以下是部分筛选结果（负载为零）：

| 配置 | 样本 | 1 ms 失约 | 响应 p99.9 | WKC 异常交换 |
| --- | ---: | ---: | ---: | ---: |
| 原配置普通调度，第一次 | 20000 | 0 | 525.9 µs | 0 |
| 原配置普通调度，复测 | 30000 | 67 | 784.6 µs | 49 |
| powersave + FIFO80，自由迁移 | 20000 | 37 | 702.2 µs | 33 |
| performance/禁睿频 + FIFO80，自由迁移 | 20000 | 2 | 445.0 µs | 1 |
| performance/禁睿频 + FIFO80/CPU4，原 NAPI，复测 | 30000 | 31 | 439.4 µs | 29 |
| 上项 + NAPI FIFO90/CPU6 | 30000 | 0 | 445.5 µs | 0 |
| performance/禁睿频 + FIFO40/CPU4 + NAPI FIFO90/CPU6 | 30000 | 0 | 432.3 µs | 0 |

原配置短测一次零失约，复测却出现明显尾部问题，因此不能凭单组结果选择配置。NAPI 线程化并提高接收线程优先级，消除了上述两组筛选中的 WKC 异常；这个变化与收包调度瓶颈相符，但没有内核 trace 因果证据，不能断言全部异常的唯一根因。

局部 performance 且睿频开启、低 timer slack 普通调度等组没有胜出。各组顺序执行、后台日常服务保留、样本长度不同，没有统计显著性保证。完整候选 CSV 全部保存，可重新计算。

## 最终验证

最终测试于北京时间约 16:51–16:56 完成。以下“响应”均包含计划释放到实际发送的迟到；分位数/最大值不包含未匹配样本。

| 配置与场景 | 秒数/拍数 | 1 ms 失约 | 回环 RTT p99.9 | 响应 p99.9 | 匹配响应最大 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 原配置 + 4 帧 CAN 背景 | 30 / 30000 | 177（0.590%） | 701.8 µs | 761.7 µs | 1062.8 µs |
| 推荐配置，空载回环 | 120 / 120000 | 5（0.004167%） | 431.7 µs | 434.9 µs | 536.8 µs |
| 推荐配置 + 4 帧 CAN 背景 | 60 / 60000 | 0 | 431.6 µs | 436.4 µs | 513.1 µs |
| 推荐配置 + 4 帧 CAN 背景 + 主机负载 | 60 / 60000 | 1（0.001667%） | 448.4 µs | 452.5 µs | 519.2 µs |
| 推荐配置，纯 EtherCAT 交换 | 30 / 30000 | 0 | 72.2 µs（EtherCAT RTT） | 76.7 µs | 153.2 µs |

推荐配置的四个最终测试合计 **270000 拍、6 次失约**；其中 CAN 回环合计 240000 拍、6 次失约。所有推荐配置最终组 **WKC 异常交换为零，测量段 minor/major page fault 都为零**。纯 EtherCAT 组只收不发 CAN。

两组 60 秒 CAN 背景测试均完整收到 **240000/240000** 个背景帧（按序号与 ID 去重），说明生成的 4 帧/拍负载没有被测量工具虚计成更高吞吐。主机负载为 4 个 nice10 间歇计算进程、约50%占空比，属于受控中等负载，不是全 CPU/内存/磁盘综合满载。未触发温度停止线；推荐配置各组观测最高 **54–57°C**。

最终组合测试期间网卡已有历史 align/rx_errors=20，但前后计数没有增加，未见新的网卡统计错误。未读取从站 CAN 错误寄存器，所以不能把零新增以太网错误推广成 CAN 物理层零错误。

相同 CAN 背景下，响应 p99.9 从原配置 761.7 降至 436.4 µs，约降低 **42.7%**；观察到的失约率从 0.590% 降到该 60000 拍的零事件。两组时长与运行时刻不同，属于工程 A/B 观测，没有全局最坏界保证。

120 秒空载中，5 个无回声周期连续出现；对应 WKC 始终完整，首次 EtherCAT 交换约 57–65 µs，且排空窗口未见匹配的迟到回声。只能说明 host→PDO→CAN→PDO 回声链路未在该观测中完成，不能从这些数据证明发生在线缆、发送队列还是接收/PDO 缓冲区。推荐主机配置已明显改善主机侧，但还没有达到“绝不丢一次更新”。

## 下一步优化，按优先级

### 1. 先给从站增加可关联的证据

目前残余事件在完整 WKC 条件下发生，先记录同一 token/序号的：PDO 被 MCU 接收时间、CAN 入发送队列时间、TX complete 时间、CAN RX 时间、写入上行 PDO 时间。同步记录 MCAN 的 TEC/REC、LEC、bus-off、TX FIFO 满、RX FIFO lost、软件队列高水位与丢弃计数；用固定内存环形缓冲区采集，测试后再导出，避免在 ISR 内串口打印。

把收到的每帧保留序号，明确上行按 ID 去重时会不会覆盖尚未被主机读取的数据。主机每个周期仅一帧、却在异常时连续多拍无回声，这比继续微调 governor 更值得调查。需要真实示波器或 CAN 分析仪旁路观察时，分别验证 CAN0 发出了什么、CAN2 看到了什么。

### 2. 验证 DC 与从站 CAN/PDO 的相位关系

代码已调用 `ecx_configdc()` 和 `ecx_dcsync0(...,1000000,0)`，但主机周期只按本机 `CLOCK_MONOTONIC` 推进，没有利用 `ctx.DCtime` 做相位误差反馈。首先确认从站 ESC 确实支持并启用了 DC、MCU 固件是否真的由 SYNC0 触发 PDO/CAN；调用配置 API 不等于整个 CAN 路径已经同步。

若支持，在固定 1 ms 周期内安排“提前送 PDO→SYNC0 采纳指令→CAN TX/RX→下一次 PDO 发布”的相位，记录主机发帧相对 DC 的偏移，再通过慢速 PI 调节绝对唤醒相位。逐步扫描 phase offset，并按失约率和全部样本响应时间评估。ETG 所述亚微秒 DC 同步指节点时钟/触发同步，不能直接当成 CAN 端到端延迟承诺。[6]

### 3. 隔离真实物理核，而不是只 taskset

目前 CPU4 仍可能承受后台任务与中断，CPU5 是它的 SMT 兄弟；CPU6/7 同理。部署时先用 cpuset v2 的 isolated partition 或经过验证的 boot 配置，将整个物理核心的逻辑线程一起纳入资源规划；把普通业务、RCU、workqueue 和无关 IRQ 留在 housekeeping CPU。不要直接照抄 `isolcpus=2,3`：那对本机是同一个物理核心，并不是两个独立核心。

可研究 `nohz_full=4,5 rcu_nocbs=4,5` 与 CPU partition，但高频系统调用、收包和多个可运行线程会限制 full dynticks 效果；CPU 隔离不是自动消除所有 jitter。网卡 NAPI 继续与其有效 IRQ CPU 对齐。改 boot 参数需维护窗口与恢复入口；本次未改 boot 配置。[2][3][7]

### 4. 对真实控制程序使用相同规则

`motor_pos.c` 当前控制循环中仍有控制台输入/输出、格式化和 `fflush`，未见 `mlockall`。将通信/控制线程与 UI/日志线程分离，用有界无锁/单写单读队列交换数据；进入实时段前分配并触碰内存与栈。检查 `clock_nanosleep` 的 EINTR，记录绝对释放迟到与 WCET；对落后周期采用明确的跳过/降级策略，避免无上限追赶。

应用 FIFO40 低于收包 NAPI90，避免高优先级忙轮询压住依赖的网络处理；但优先级分配仍要按真实线程依赖复核，不能将整套 UI 和日志都永久放到高优先级。`mlockall` 只能去掉可控页错误风险，不解决硬件/固件等待。[1][4][5]

### 5. 用 trace 定位极端尾部，并做部署级长测

出现 >50–100 µs 唤醒异常时用本机已有 `rtla timerlat`，区分硬 IRQ latency 和线程调度 latency；若异常只有网络往返增长而唤醒正常，再录 `sched_switch`、IRQ、softirq/NAPI 和网卡事件。追踪有开销，性能验证和故障诊断分开运行。[8]

建议在最终业务并发、散热、供电、网络连接条件下做至少 1–8 小时分段试验；按不同 CAN 占用率分别保留截止期失约、连续失约长度、WKC、TX/RX 错误与温度。零失约只能为测试条件下的观测：N 拍零事件时，独立同分布近似的 95% 单拍概率上界约 3/N；它也不是绝对硬实时证明。

### 6. 物理层与带宽预算

CAN 回环要求合适的双端终端电阻、共地和短支线。本文 4 帧背景 + 1 帧探测、1 Mbps、1000 Hz 的“约60%”仅是按每帧约120 bit 粗算；实际位填充、仲裁和错误重传会改变利用率。保持足够余量，避免用接近饱和的成功样本分位数掩盖未送达样本。

当前以太网中断合并已接近最低，不建议再套用通用“关闭所有 offload”清单。若长期仍见 r8169 驱动相关尾部，可在同样线缆/从站/负载下 A/B 测试另一张 Linux 支持良好的专用 NIC，或评估 EtherCAT 原生驱动方案；本次未测，不能承诺更换网卡必然改善。

## 参考资料

2026-10-15 实际读取下列官方文档/手册；其中 Intel pstate 与 cpuset 特意读取了 v6.8 文档。其余现行文档中的较新功能不能直接假设本机 6.8 支持；本次使用的 threaded NAPI 已通过本机 sysfs 和实际接收验证。

1. Linux PREEMPT_RT differences：[Execution context / threaded interrupts](https://docs.kernel.org/core-api/real-time/differences.html)。解释 RT 内核改变中断与锁的上下文，不等于用户线程自动实时。
2. Linux NAPI：[Threaded NAPI](https://docs.kernel.org/networking/napi.html#threaded-napi)。说明独立收包线程与 IRQ CPU 对齐建议；本文未采用较新 threaded busy-poll 功能。
3. Linux IRQ：[SMP IRQ affinity](https://docs.kernel.org/core-api/irq/irq-affinity.html)。区分允许的亲和性与实际中断分布。
4. Linux man-pages：[sched(7)](https://man7.org/linux/man-pages/man7/sched.7.html)、[mlock(2)](https://man7.org/linux/man-pages/man2/mlock.2.html)。FIFO 优先级与驻留内存/预触碰栈。
5. Linux 6.8：[intel_pstate](https://www.kernel.org/doc/html/v6.8/admin-guide/pm/intel_pstate.html)。HWP performance、EPP 与 no_turbo；powersave 不是固定最低频率。
6. EtherCAT Technology Group：[EtherCAT technology §1.5 Distributed Clocks](https://www.ethercat.org/en/technology.html)。DC 同步与提前发送 PDO 的职责。
7. Linux 6.8：[Control Group v2 — cpuset](https://www.kernel.org/doc/html/v6.8/admin-guide/cgroup-v2.html)、[per-CPU kthreads](https://docs.kernel.org/admin-guide/kernel-per-CPU-kthreads.html)。isolated partition、housekeeping、RCU 工作迁移。
8. Linux tracing：[Timerlat tracer](https://docs.kernel.org/trace/timerlat-tracer.html)。区分 IRQ 与线程唤醒尾部。
9. Linux man-pages：[clock_nanosleep(2)](https://man7.org/linux/man-pages/man2/clock_nanosleep.2.html)、[PR_SET_TIMERSLACK](https://man7.org/linux/man-pages/man2/PR_SET_TIMERSLACK.2const.html)。绝对时间睡眠、EINTR 与普通线程 timer slack；RT 策略不使用 timer slack。

## 文件与验证

已通过编译（-Wall -Wextra 无警告）、bash 语法检查、git diff 空白检查；5 组非法参数均在初始化网卡前拒绝。运行脚本的正常退出、命令失败及 TERM 中断路径均验证通过，16 个 governor/EPP、no_turbo 与 NAPI threaded 恢复原值，无遗留测试负载。实际物理回环的序号/token、WKC、背景接收覆盖与零页错误均已在日志验证。

远端新增 `src/rt_probe.h`、`scripts/rt-control-run.sh`，`src/ecat2can_master.c` 增加 probe 入口；README 增加复测说明。原有工具命令及 `motor_pos` 控制逻辑保持原实现，未自动使能电机。原可执行文件备份在测试目录。

所有原始 CSV、构建/参数验证/测试日志：`build/rt-tests/20261015-e2e/`。每次 profile 的原设置、恢复信息和温度位于 `build/rt-control-*/`。系统参数由运行脚本限定作用域，退出恢复；没有持续后台控制任务。未改系统服务或开机配置。
