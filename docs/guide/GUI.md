# 图形测试工作台

> 文档类型：操作指南  | 适用范围：Linux Qt 工作台  | 状态：当前

`ecat2can_gui` 为 ECAT2CAN 从站测试和 DJI 电机调试提供图形入口。它负责选择网卡、收集参数、启动和停止测试进程、展示命令输出；主站诊断由 `ecat2can_master` 执行，电机示例由 `motor_pos` 执行。GUI 不会在界面线程中运行实时通信循环。

## 构建和启动

安装 Qt 5 Widgets 开发包后，使用默认 CMake preset 构建：

```bash
sudo apt install qtbase5-dev
cmake --preset default
cmake --build --preset default
build/ecat2can_gui
```

GUI 与 `ecat2can_master` 应位于同一目录。没有 Qt 时，CMake 会跳过 GUI 目标，命令行主站仍可独立构建。

## 工作台功能

窗口顶部固定显示 EtherCAT 网卡、链路状态和当前任务状态。中间按“从站诊断”“CAN 测试”“性能测试”“DJI 电机”“高级命令”分为五个页面；窗口底部是可调整大小的执行输出面板。页面内容较多时可独立滚动，不会把日志挤到很远的位置。

| 页面 | 功能 |
| --- | --- |
| 从站诊断 | `info` 状态监视、可设拍数的 PDO `loop`、ESC 寄存器读取和从站复位 |
| CAN 测试 | 可设采样周期数的被动扫描与反馈读取；发送界面可设 CAN 频率（1–1000 Hz）和总帧数 |
| 性能测试 | 主机调度实时性、EtherCAT 实时性与吞吐基准、双 CAN 通道回环延迟 |
| DJI 电机 | 启动 M3508/C620 或 GM6020 调试，查看位置、转速和反馈/输出实时波形，并发送常用闭环命令 |
| 高级命令 | 选择 `ecat2can_master` 支持的任意命令并填写参数，覆盖暂未提供专用表单的功能 |

主机调度测试不需要 EtherCAT 硬件。每次只运行一个任务，可从底部输出面板停止任务、清空日志或拖动面板调整高度。高级命令的参数以空格分隔，直接作为进程参数传递，不经过 shell；命令完整用法见 `build/ecat2can_master --help`。`probe` 命令还需要在启动 GUI 前设置 `ECAT2CAN_PROBE_CSV` 环境变量。

## 权限和安全

图形程序会以当前用户身份启动主站，不会自动提权。给主站程序配置 raw socket 能力：

```bash
sudo setcap cap_net_raw,cap_net_admin+eip build/ecat2can_master
```

每次重新链接主站后都需要重新设置 capability。也可以从具备相应权限的终端启动 GUI，但不建议把 GUI 整体以 root 运行。

`bench` 和“CAN 报文发送”会在 CAN 总线上产生实际流量。发送功能在执行前要求确认；先确认所选通道、设备、报文 ID、数据、频率和帧数。不要在连接真实执行器且不了解报文含义时发送控制帧。

DJI 调试默认以 `--monitor` 只读模式启动，界面会绘制输出轴位置/目标、转速/目标转速或电流反馈与输出原始计数。选择“控制模式”会在启动前弹出确认；进程启动后仍为失能状态，点击“使能”还会再次确认。请先只读检查电机反馈、通道和 ID，并确认电机已固定、周围无人。停止任务会向 `motor_pos` 发送终止信号，由程序先发送零输出再关闭 EtherCAT。

图表采样周期为 20 ms（50 Hz），保留最近约 12 秒的数据；它用于调试趋势观察，不替代 1 kHz 控制周期，也不适合作为高频控制性能测量工具。GM6020 电流/电压模式及限幅单位需与电机固件配置一致。

从站诊断、PDO 校验、扫描与寄存器读取需要连接 ECAT2CAN EtherCAT 从站。使用顺序和故障定位步骤见[命令速查](COMMANDS.md)与[故障排查](TROUBLESHOOTING.md)。
