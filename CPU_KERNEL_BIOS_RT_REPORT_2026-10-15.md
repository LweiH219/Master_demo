# CPU 绑定、内核与 BIOS 实时性优化（2026-10-15）

## 主机与实验边界

主机为 Thunderobot MIX NUC II，i7-13620H，BIOS American Megatrends 5.27（2025-08-21），Ubuntu PREEMPT_RT 6.8.1-1060-realtime。CPU4/5 与 CPU6/7 分别为同一物理 P 核的两个 SMT 线程。控制程序绑定 CPU4、FIFO40；enp3s0 接收 NAPI 绑定 CPU6、FIFO90；performance governor、关闭 Turbo。该基础配置来自之前的端到端实测筛选。

单从站，CAN0↔CAN2 物理短接，无电机。每组 1 kHz、每周期 1 帧探测加 4 帧 CAN 背景流量。测量 release→匹配回环响应，1 ms 内未取得有效匹配即失约；响应分位数只统计匹配样本，必须同时看失约数。本次没有测量策略推理、传感器采样、驱动器和电机响应。

## 实际完成的在线实验

在基础绑定上临时关闭 CPU5/7，避免兄弟线程共享执行资源；将公共 unbound workqueue 的允许 CPU 改为 0–3、8–15；将实际位于 CPU4/6 的其他可写设备 IRQ 移到相同 housekeeping 集合，保留 EtherCAT IRQ134/CPU6。失败的 IRQ 写入会明确记录，不会假定迁移成功。所有修改都有快照并在结束时逆序恢复。普通进程和所有内核线程没有被全局隔离，因此这仍不是完整的独占 CPU 分区。

第一次热下线试验暴露原启动脚本会访问已离线 CPU 的 cpufreq policy，返回 EBUSY，未进入测量。已修复为只快照并设置可读取的 policy；失败试验的核心在线状态、中断和工作队列已恢复。失败数据不纳入性能比较。

## 实测结果与结论

基础绑定方案的有效 CAN 组：60000 拍，1 ms 失约 0，异常 WKC 0，背景帧收到 240000/240000。release→回环响应 p99.9=450.124 µs，已匹配响应最大=515.28 µs；唤醒 p99.9=6.128 µs，最大=9.167 µs。测量段 minor/major faults 均为0。

后续基线测试在北京时间17:36:20发生 enp3s0 Link Down，该组不作为性能结论。实际CSV中该组60000拍有40541次无匹配、40541次WKC异常；这是已确认物理链路中断时的记录，整组从配置比较中排除。下一组核心下线始于17:37:01，晚于断链，扫描不到从站，因此尚未取得“核心预留方案”的有效端到端结果。不能据此说关闭SMT改善或恶化CAN实时性。

另做不依赖链路的 cyclictest，CPU4、FIFO40、1 ms周期、锁内存，每组30秒：

| 主机配置 | 直方图样本 | 唤醒p99 | 唤醒p99.9 | 最大延迟 |
| --- | ---: | ---: | ---: | ---: |
| 基础绑定 | 30000 | 4 µs | 7 µs | 11 µs |
| 关闭CPU5/7并迁移工作队列/可迁移IRQ | 30000 | 5 µs | 8 µs | 17 µs |

此次短测的核心预留方案没有显示收益，p99.9和最大值略高；样本较短且顺序执行，不能据此断言SMT下线必然有害。这是主机调度测试，不包含EtherCAT/CAN，不提供最坏延迟保证。当前继续采用已验证的基础绑定配置作为部署候选；更强隔离配置仍待恢复物理链路后完成相同负载的A/B与长测。所有本次CPU在线状态、工作队列、中断亲和性、governor/EPP、Turbo与NAPI临时设置均已检查恢复；未修改BIOS或启动配置。

## 内核下一步

1. 保留现有 PREEMPT_RT；当前配置已支持 CONFIG_NO_HZ_FULL、CONFIG_RCU_NOCB_CPU、HZ1000。优先解决干扰归属，无需为“更实时”立即重编译内核。
2. 真正隔离 CPU4–7 的两个物理核，普通业务与推理线程使用 0–3、8–15；应用用4，EtherCAT IRQ/NAPI用6，兄弟线程5/7继续留空或临时下线。优先使用 cgroup v2 cpuset isolated partition，并与 systemd 的 CPU 资源配置统一管理；仅 taskset 控制线程不会阻止其他任务进入同一核。
3. 若采用启动期隔离，可在维护窗口试验以下候选参数，每次只引入一类变化：

   第一阶段：`isolcpus=domain,managed_irq,4-7 irqaffinity=0-3,8-15`

   第二阶段额外比较：`nohz_full=4-7 rcu_nocbs=4-7`

   `isolcpus=domain` 官方已标记 deprecated，运行期无法撤销调度域隔离，因此优先 cpuset；上述是备选实验配置，不是已验证最优值。`nohz_full` 已隐含相同 CPU 的 RCU 回调卸载，额外写 rcu_nocbs 仅便于明确意图。频繁系统调用、阻塞/唤醒和多 runnable task 会限制免 tick 的收益。
4. irqaffinity 只设置默认中断目标。重启后必须按设备重新找到 enp3s0 MSI IRQ，显式绑定 CPU6，并核对 effective_affinity_list；不可照抄旧 IRQ134。现有运行脚本遇到实际 IRQ 不在6会拒绝执行。managed IRQ 的隔离是尽力而为，不能据此假定所有中断都已离开控制核。
5. 公共工作队列仍应限定 housekeeping 核；RCU、其他可迁移 IRQ、后台服务和推理任务也要检查。保留 RT 带宽保护、温控和安全缓解。当前 nmi_watchdog=1，只有 trace 明确指向其开销时才单独 A/B 关闭，避免一次性关闭所有诊断功能。
6. 当前 cpuidle 驱动为 none，cpu4 无 cpuidle 目录，因此不把 `intel_idle.max_cstate=1`、`processor.max_cstate=1` 当作已有效优化；`idle=poll` 可能显著增加功耗和热负担，当前不推荐。

本次没有修改 GRUB、切换内核或重启。当前 GRUB 菜单隐藏且超时0；在任何启动参数试验前，应先准备可见菜单/恢复入口，保留当前 RT 和 generic 内核。建议先通过 GRUB 单次编辑测试参数，失败后重启回到未改默认项；确认后才持久化。恢复时删除新增参数并 update-grub，不改 root/启动盘设置。

## BIOS 可优化项

已通过 DMI 识别型号和版本，但 SSH 无法确认本机固件菜单是否暴露下列选项，也没有通过 EFI 变量强写 BIOS。

| 项目 | 建议实验顺序与代价 |
| --- | --- |
| Turbo Boost | 先维持关闭，软件 no_turbo 已可在线实现；之前开启睿频的压力测试温升明显。若未来推理算力不足，再单独比较受限 Turbo 和散热。 |
| Hyper-Threading / SMT | 已用临时关闭5/7测试局部效果。BIOS 关闭通常影响全部P核，会减少推理吞吐；先比较局部保留核心方案。 |
| CPU/Package C-state limit | 若菜单存在，单独测试限制深睡眠前后的长尾、温度和功耗；OS cpuidle=none 并不能证明固件所有节能行为都已关闭，也不能证明限制一定有效。 |
| PCIe ASPM / 网卡节能 | 仅在专用 EtherCAT PCIe 链路确有节能进入/退出证据时测试。当前内核 ASPM 为 default，没有证明它导致异常；不先全局禁用。 |
| 散热与功率 | 使用稳定供电和合适风扇曲线，确保持续负载不触发热/功率限频；保留温度、过流等保护。 |

不建议为追求实时性无依据地关闭 E 核、虚拟化、IOMMU 或全部安全缓解；可将 E 核用于 housekeeping。BIOS 更新须有厂商匹配本机的版本和修复说明，不因版本号较旧就直接刷写。SMI/固件停顿不能由 PREEMPT_RT 消除，需硬件/固件侧验证。

## 如何验收与继续定位

最终部署应在真实 RL 推理、日志、传感器和总线负载下做至少1–2小时分段测试，记录失约率、最长连续失约、反馈年龄、WKC、CAN错误计数、温度和CPU频率。50–100 Hz RL策略配合1 kHz本地控制是当前较稳妥的架构建议，仍需实机闭环验证；尚不能承诺严格1 kHz端到端硬实时。

在控制唤醒异常时单独运行 rtla timerlat，分辨 IRQ 与线程延迟；网络/回环异常而唤醒正常时，采集 NIC/NAPI trace，并在从站增加 PDO接收、CAN入队、TX完成、RX到达、PDO发布的同序号时间戳和溢出计数。之前出现过完整WKC下的连续无回声，继续微调主机不能替代这部分证据。

## 参考资料

- [Linux 6.8 kernel parameters](https://www.kernel.org/doc/html/v6.8/admin-guide/kernel-parameters.html)：本次读取 isolcpus、irqaffinity、nohz_full、RCU 参数的约束。
- [Linux 6.8 cgroup v2 cpuset](https://www.kernel.org/doc/html/v6.8/admin-guide/cgroup-v2.html)：isolated partition 与CPU资源规划。
- [Linux per-CPU kthreads](https://docs.kernel.org/admin-guide/kernel-per-CPU-kthreads.html)：控制核上的内核工作迁移。
- [Linux IRQ affinity](https://docs.kernel.org/core-api/irq/irq-affinity.html) 与 [Threaded NAPI](https://docs.kernel.org/networking/napi.html#threaded-napi)：中断有效亲和性与接收线程配合。
- [Linux 6.8 intel_pstate](https://www.kernel.org/doc/html/v6.8/admin-guide/pm/intel_pstate.html)：governor、EPP、no_turbo；performance不等于频率恒定。
- [PREEMPT_RT differences](https://docs.kernel.org/core-api/real-time/differences.html) 与 [Timerlat](https://docs.kernel.org/trace/timerlat-tracer.html)：RT的作用范围与长尾诊断。

BIOS建议是基于平台机制的候选实验，不是已核实的该型号菜单说明。
