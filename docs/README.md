# 文档索引

项目文档按用途分组，避免把操作步骤、设计说明和实验原始资料混在一起。

| 目录 | 内容 | 适合先看什么 |
| --- | --- | --- |
| [`guide/`](guide/) | 构建、命令、实时性、电机和故障排查 | 日常开发和现场测试 |
| [`design/`](design/) | 项目边界、目录职责和依赖方向 | 理解代码结构 |
| [`../benchmarks/`](../benchmarks/) | 可复现的主机基准测试档案 | 比较不同主机或配置 |
| [`reports/`](reports/) | 已完成实验的结论和历史记录 | 查看过去的测试结果 |

## 文档约定

- `guide/` 描述当前可执行的命令和操作步骤。
- `design/` 描述代码边界，不记录一次实验的临时参数。
- `benchmarks/` 保存一组基准测试的报告、参数快照、原始结果和校验文件。
- `reports/` 保存历史结论；历史报告中的主机路径和参数以当时记录为准。
- 文档标题下的元信息用于标明文档类型、适用范围和状态。

推荐阅读顺序：先看 [`guide/COMMANDS.md`](guide/COMMANDS.md)，再看
[`design/ARCHITECTURE.md`](design/ARCHITECTURE.md)；需要做实时性对比时，再进入
[`../benchmarks/`](../benchmarks/) 和 [`guide/RT_GUIDE.md`](guide/RT_GUIDE.md)。
