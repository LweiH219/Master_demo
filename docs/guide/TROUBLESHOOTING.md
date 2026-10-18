# 故障排查

> 文档类型：故障排查  | 适用范围：EtherCAT、CAN 和电机示例  | 状态：当前

## 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `打不开网卡` | 没 setcap（重新 `make` 后要重设） |
| `没扫到 EtherCAT 从站` | 网关没上电 / 网线没插 Port0 / 网卡没有 `LOWER_UP` |
| `期望 PDO 192/192，实际 x/y` | 从站固件和上位机的协议版本不匹配 |
| 一直 `(等反馈)` | ① **CAN_H/CAN_L 接反** ② 电机没供电 ③ `--id` / `--ch` 不对 ④ 网关通道和接线不一致 |
| 反馈的**错误码=3** | 电机三相线与电调没接好 |
| 使能后电机完全不动 | GM6020 可能是电流环开关没开 → 加 `--gm-voltage` 验证 |
| 使能后往一个方向冲 | 闭环符号反了，加 `--invert-spd` |
| **高频抖动 / 嗡嗡响** | 增益相对链路延迟过大。先 `d` 失能，然后 `skp` 减半、`kp` 减半；仍抖就加大 `--filter-ms` |
| 电流实测和指令长期对不上 | 驱动器电流环没工作，或电压模式下量纲不同 |
| 某一路 CAN 怎么接都不通 | 见 Tide_ECAN 移植记录：检查该路 `init_*_pins()` 有没有被重复配置 |
| 半小时后突然全断 | 网关 MCAN 进了 bus-off。固件已有自动恢复；旧版本需复位板子 |

**排错顺序**：先用 `ecat2can_master` 确认链路，再看电机。

```bash
./build/ecat2can_master enp3s0 info      # 1) 从站和 PDO 正常吗
./build/ecat2can_master enp3s0 loop      # 2) 192 字节 PDO 往返
./build/ecat2can_master enp3s0 fdb       # 3) 网关收到的每一帧（含 ID/DLC/数据）
./build/ecat2can_master enp3s0 scan      # 4) 设备挂在哪一路
```

`fdb` 特别有用：如果电机在发反馈，这里能看到；**如果这里什么都没有，
问题就在网关之前或 CAN 物理层**。

### 寄存器判读：CAN 物理层

| MCAN 状态 | 含义 |
| --- | --- |
| `ECR.REC` 持续上升 + `PSR.LEC` 报错 | **信号来了但解不开** → CANH/CANL 接反、波特率不对 |
| `ECR = 0` + `PSR.ACT = Synchronizing` | **总线被持续摁成显性** → 收发器故障 / 引脚复用冲突 |
| `ECR = 0` + `PSR.ACT = Idle` | **无信号** → 线没通、对端没发、收发器没工作 |
| `PSR.ACT = 2 (Receiver)` + `LEC = 0` | **正在正常收帧** ✅ |

> `PSR`：`ACT` = bits[4:3]（掩码 `0x18`），`LEC` = bits[2:0]（掩码 `0x07`）；
> `ECR`：`TEC` = bits[7:0]，`REC` = bits[14:8]。

---
