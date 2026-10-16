# 主机实时性基准档案

基准编号：host-d26848c813ec82e6_2026-10-16_new-can

| 项目 | 绑定值 |
| --- | --- |
| 主机型号 | Thunderobot MIX NUC II |
| CPU | Intel Core i7-13620H，6P＋4E，16逻辑CPU |
| 内存 | 24 GB DDR4-3200（16 GB＋8 GB）；详见硬件档案中的SMBIOS记录 |
| 系统盘 | KINGSTON SNV2S250G，标称250 GB NVMe |
| BIOS | American Megatrends International, LLC. 5.27，08/21/2025 |
| 内核 | 6.8.1-1060-realtime，PREEMPT_RT |
| EtherCAT NIC | enp3s0，Realtek RTL8168h/r8169，测试链路100 Mbps全双工 |
| 身份依据 | DMI UUID SHA-256，详见manifest.json；未将IP当身份 |

report.md为绑定主机后的报告，host-inventory.json记录硬件、内核、内存、磁盘、PCI设备及当前参数；manifest.json明确实际测试参数及源文件/二进制校验值；results.json为原始CSV统计。evidence.tar.gz含CSV、日志、源码、运行脚本和每组参数/温度快照。restored-state.json表明测试后已恢复原参数，不能将当前powersave、Turbo开启误认为测试时的配置。

## 更换主机后的比较步骤

1. 保留相同从站、固件、CAN0↔CAN2、1 Mbps、线缆和终端电阻；记录模块型号、序列号及固件版本，本次没有独立读取到这些身份信息。
2. 将Master_demo代码、测试工具及本档案一并转移。证据包包含本次源码与工具；保留Git提交和未提交修改，不能只按提交号认为代码一致。先检查构建和运行依赖。
3. 新主机先采集硬件型号、BIOS、内核、CPU物理核/SMT拓扑、网卡驱动与IRQ分布；CPU4/6仅适用于本机，不应直接套用。应用与接收线程各选择独立物理核，并验证IRQ有效亲和性。现有rt-control-run.sh含本机CPU和网卡约束，必须适配后才能运行。
4. 用相同测量工具、1 kHz、1 ms截止期和120/60/60/30秒四组测试比较；区分默认配置和各主机经过优化的配置，避免把硬件差异与调参差异混为一谈。使用相同主机负载程序，并同时记录实际CPU占用及温度，不保证不同CPU上相同程序等于相同算力负载。
5. 同时比较失约数、最长连续失约、匹配响应p99.9/最大、WKC、背景帧完整率、页面错误、温度和网卡/CAN错误。不能只比较平均值或cyclictest。最后加入真实RL推理与传感器负载做1–2小时长测。

本档案生成没有改动BIOS、内核启动参数或主机运行配置。capture-host.py是本机采集脚本，当前固定Master_demo路径和enp3s0名称，移植前应相应调整。
