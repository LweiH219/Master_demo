# 恢复供电后的 CPU 绑定端到端复测（2026-10-15）

> 文档类型：历史测试报告  | 测试日期：2026-10-15  | 状态：历史记录


用户确认先前误关从站电源并已重新开启。本轮确认 enp3s0 carrier=1，扫描到单个从站并进入OP；先前断电期间的异常数据不用于配置优劣比较。

## 测试设置

主机 i7-13620H，PREEMPT_RT 6.8.1-1060-realtime；单从站 CAN0↔CAN2 物理短接，无电机。基础方案：应用CPU4/FIFO40，EtherCAT IRQ和NAPI在CPU6，NAPI FIFO90，performance，关闭Turbo。核心预留方案在此基础上临时关闭SMT兄弟线程CPU5/7，公共工作队列移到CPU0–3、8–15，并尝试迁移其他位于CPU4/6的可写IRQ。

本轮IRQ140/142写入返回EIO，脚本明确保留原值。/proc/interrupts确认它们为NVMe的nvme0q5/nvme0q7，有效目标分别仍为CPU4/6，因此该候选不是完整的CPU独占隔离。普通业务进程没有被统一移出控制核。没有修改BIOS、GRUB、内核版本或重启。

每组60秒、1 kHz，每周期一帧8字节序号/token探测帧，加4帧CAN背景流量。主机负载是4个nice10间歇计算进程，约50%占空比，不是全机满载；两种配置分别启动相同负载。按绝对计划release→有效匹配回环响应评价1 ms截止期；RTT不是单向延迟。各组顺序执行，非随机交错实验。

## 结果

表内延迟单位均为µs，响应分位数和最大值只统计匹配样本，必须同时看失约数。

| 配置 | 拍数 | 1 ms失约 | 响应p99.9 | 匹配响应最大 | 唤醒p99.9 | 背景帧收到 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 基础绑定 | 60000 | 0 | 464.517 | 541.505 | 4.88 | 239996/240000 |
| 核心预留 | 60000 | 0 | 445.735 | 521.019 | 8.146 | 240000/240000 |
| 核心预留＋主机负载 | 60000 | 0 | 455.233 | 493.076 | 3.64 | 240000/240000 |
| 基础绑定＋主机负载 | 60000 | 0 | 461.697 | 558.213 | 4.523 | 240000/240000 |
| 基础绑定复测 | 60000 | 0 | 439.546 | 515.494 | 7.999 | 240000/240000 |

合计 **300000拍、0次1 ms失约、0次WKC异常交换**。各组测量段minor/major page faults均为0。每秒采样的CPU温度最高61°C，没有触发85°C停止线；网卡统计前后没有新增错误。首组基础绑定的背景帧少4帧，不能将“探测帧零失约”解释为所有背景流量都可靠送达；其余计数见表。尚未通过从站CAN错误计数或固件队列时间戳确定缺帧位置。

## 选型与后续

保留基础绑定作为当前已验证的部署候选。核心预留方案在短测中的分位数变化，不足以证明长期更优；先前主机cyclictest的p99.9/最大值为基础绑定7/11µs，核心预留8/17µs，也没有显示稳定收益。不将关闭SMT或BIOS改动固化为“最佳配置”。所有临时参数在每组结束后恢复，最终状态和恢复日志已保存。

下一步最值得验证的是：真实策略推理与传感器负载下的1–2小时分段长测；使用cgroup v2 cpuset isolated partition完整规划两个物理核；必要时再分别比较nohz_full/RCU卸载。NVMe中断仍在控制核上，应进一步核查managed IRQ约束与队列映射，不能假定写入亲和性就能迁移成功；需要时在有恢复入口的维护窗口单独验证启动期managed_irq隔离的实际效果。BIOS维持现状，逐项A/B验证Turbo、SMT、深度C-state，不关闭温控保护。具体候选参数、代价和回退说明参见此前《CPU_KERNEL_BIOS_RT_REPORT_2026-10-15.md》。

对RL Sim2Real，仍建议50–100 Hz策略配合1 kHz本地执行与反馈年龄监控。此处是通信回环，未包含推理、驱动器与电机动态；短测零失约不能作为严格1 kHz端到端硬实时保证。

## 可复核证据与参考

远端原始CSV、逐组日志、网卡统计、状态快照：/home/tide/Master_demo/build/rt-tests/20261015-core-restored/。每组电源/NAPI/温度快照位于日志所引用的build/rt-control-*目录。

- [Linux 6.8 cpuset](https://www.kernel.org/doc/html/v6.8/admin-guide/cgroup-v2.html)：完整CPU分区与isolated partition。
- [Linux 6.8内核参数](https://www.kernel.org/doc/html/v6.8/admin-guide/kernel-parameters.html)：isolcpus、nohz_full、RCU与managed_irq的限制。
- [IRQ affinity](https://docs.kernel.org/core-api/irq/irq-affinity.html)、[Threaded NAPI](https://docs.kernel.org/networking/napi.html#threaded-napi)：有效IRQ亲和性与接收线程绑定。
- [Intel pstate 6.8](https://www.kernel.org/doc/html/v6.8/admin-guide/pm/intel_pstate.html)：频率策略、EPP和Turbo控制。
