# 主机基准测试

这里保存可复核的主机实时性基准档案；可执行测试入口统一位于 `scripts/`。基准测试用于比较主机、内核、网卡和调度配置，不能替代真实电机控制系统的长时间验收。

## 档案结构

```text
benchmarks/
├── README.md
└── host/
    └── 2026-10-16-host-d26848c813ec82e6/
        ├── README.md             档案说明与复测步骤
        ├── report.md             本次测试结论
        ├── results.json          结构化结果
        ├── host-inventory.json   硬件、内核和网卡信息
        ├── manifest.json         参数及源文件/二进制校验
        ├── restored-state.json   测试结束后的恢复状态
        ├── SHA256SUMS.txt        文件完整性校验
        ├── capture-host.py       主机信息采集脚本
        └── evidence.tar.gz       原始 CSV、日志和快照
```

## 如何使用一份基准

1. 先阅读档案目录中的 `README.md`，确认主机身份、内核、网卡和接线条件。
2. 阅读 `report.md`，确认测试场景、截止期定义、负载和结果边界。
3. 用 `SHA256SUMS.txt` 校验压缩包、JSON 和脚本没有被改动。
4. 对比时同时记录 p99.9、最大延迟、失约数、WKC、背景帧完整率、温度和网卡错误；不要只比较平均值。
5. 新主机使用相同从站、固件、CAN 回环、频率、截止期和负载，并创建新的日期目录，不覆盖旧档案。

## 结果边界

主机基准只能说明“指定配置、指定时间窗口和指定硬件条件下”的表现。短时间零失约不代表硬实时保证；需要结合真实控制程序、传感器负载和 1–2 小时长测进行部署验收。

当前档案：[`2026-10-16-host-d26848c813ec82e6/`](host/2026-10-16-host-d26848c813ec82e6/)。

相关历史结论：[`HOST_RT_REPORT_2026-10-15.md`](../docs/reports/HOST_RT_REPORT_2026-10-15.md)、[`NEW_CAN_RT_REPORT_2026-10-16.md`](../docs/reports/NEW_CAN_RT_REPORT_2026-10-16.md)。
