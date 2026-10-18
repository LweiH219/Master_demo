# 主机实时性基准工具

> 工具类型：主机调度基准  | 测试对象：Linux 主机，不访问 EtherCAT/CAN  | 状态：当前

`run.sh` 是本项目的标准主机基准入口。它把项目自身的 `rt ... host` 测试和 `cyclictest` 绝对唤醒测试放在同一个受控运行中，并在退出时恢复 governor、EPP 和 Turbo 设置。

## 前置条件

```bash
cmake --preset default
cmake --build --preset default
sudo apt install rt-tests util-linux python3
```

脚本需要 root 权限、`cyclictest`、`chrt`、`flock` 和已构建的 `build/ecat2can_master`。

## 运行

```bash
# 保持当前电源策略，运行 30 秒
sudo ./scripts/rt-host-test.sh 30 current

# 临时切换 performance
sudo ./scripts/rt-host-test.sh 30 performance

# performance 并临时关闭 Intel Turbo
sudo ./scripts/rt-host-test.sh 30 performance-no-turbo
```

输出目录由脚本打印，位于 `build/rt-host-*/`，包括环境快照、项目周期结果、cyclictest 原始输出、直方图和 JSON 结果。

## 结果判读

至少记录以下指标：

- 项目周期的截止期错失数和最长周期；
- `cyclictest` 的 p50、p99、p99.9 和最大唤醒延迟；
- CPU governor、EPP、Turbo、内核版本和 CPU 拓扑；
- 测试期间温度和后台负载。

该工具用于比较主机配置，不提供硬实时保证。需要比较 EtherCAT/CAN 链路时，使用 [`../../README.md`](../../README.md) 中的端到端基准档案和 `scripts/rt-control-run.sh`。
