# 新CAN模块实时性复测（2026-10-16）

用户确认仍为单个EtherCAT从站、CAN0↔CAN2物理回环、1 Mbps，无电机或驱动器。主机10.191.242.222，项目/home/tide/Master_demo。未读取模块独立序列号，因此“新模块”的身份以用户确认及本次实际接线为准。

## 配置和方法

沿用此前实测筛选的配置：PREEMPT_RT 6.8.1-1060-realtime，应用CPU4/SCHED_FIFO40，enp3s0 IRQ有效CPU6，线程化NAPI CPU6/FIFO90；CPU governor performance，Turbo关闭。运行脚本在结束时恢复原设置。未修改BIOS、GRUB或内核，也未额外下线SMT线程。

1 kHz绝对周期，120秒空载CAN回环、60秒CAN背景流量、60秒主机＋CAN负载、30秒纯EtherCAT。CAN探测为0x600、8字节序号及随机token，核对完整数据、DLC、WKC；背景4帧/周期使用0x601起的ID，按序号与ID去重。预热200周期；测量段预分配、锁内存并延后写CSV。主机负载为4个nice10间歇计算进程、约50%占空比，属于受控中等负载，不代表真实RL推理或全系统满载。

截止期从计划release算起，1 ms内没有取得有效匹配即算失约。RTT为发送至物理回环匹配，不能除以2当作测得的单向延迟。纯EtherCAT组不下发CAN帧。分位数和最大响应仅统计匹配样本，因此须与失约数同时阅读。

## 结果

延迟单位：µs。

| 场景 | 拍数 | 1 ms失约 | RTT p99.9 | 响应p99.9 | 匹配响应最大 | 唤醒最大 | WKC异常交换 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| CAN回环空载 | 120000 | 0 | 420.676 | 423.833 | 482.718 | 14.872 | 0 |
| CAN背景流量 | 60000 | 0 | 431.711 | 434.964 | 512.936 | 10.41 | 0 |
| 主机＋CAN负载 | 60000 | 0 | 436.498 | 439.627 | 537.56 | 8.537 | 0 |
| 纯EtherCAT | 30000 | 0 | 66.045 | 68.947 | 117.386 | 8.196 | 0 |

合计270000拍、0次失约，WKC异常交换0次；其中CAN物理回环240000拍、0次失约。完整结果以随附CSV、日志及summary.json为准。

CAN背景组收到240000/240000帧；主机＋CAN组收到240000/240000帧。四组测量段minor/major页面错误均为0，网卡统计未新增错误。温度按每秒采样最高53°C，未触发85°C停止线。

## 如何解读

本轮为同一主机配置下的新模块复测。更换模块、时间和运行环境并未随机交错控制，不能仅凭组间变化证明先前问题的唯一原因。短测零事件也不能证明永不失约，尤其先前曾见连续无回声事件。

这项测试覆盖主机→EtherCAT PDO→从站CAN→物理回环→PDO→主机，不包括策略推理、真实传感器、驱动器或电机。用于RL Sim2Real仍建议50–100 Hz策略配合1 kHz本地执行，监控反馈年龄、连续失约并设置降级处理；在真实并发负载下完成1–2小时分段长测后再评价部署可靠性。

本次最后状态：CPU在线=0-15，公共workqueue mask=ffff，no_turbo=0，NAPI threaded=0，EtherCAT carrier=1。恢复检查详情保存在final-state.json和各profile目录。

## 数据与参考

远端原始证据：/home/tide/Master_demo/build/rt-tests/20261016-new-can/；测试脚本run.sh、原始CSV、逐组输出、温度/参数快照和网卡统计均随证据包保留。

- [Linux Threaded NAPI](https://docs.kernel.org/networking/napi.html#threaded-napi)
- [Linux IRQ affinity](https://docs.kernel.org/core-api/irq/irq-affinity.html)
- [Linux 6.8 intel_pstate](https://www.kernel.org/doc/html/v6.8/admin-guide/pm/intel_pstate.html)
- [PREEMPT_RT differences](https://docs.kernel.org/core-api/real-time/differences.html)

以上资料用于解释测试配置，不替代实测保证。
