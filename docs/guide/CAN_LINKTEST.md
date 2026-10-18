# CAN 链路自测

> 文档类型：操作指南  | 适用范围：SocketCAN 链路自测  | 状态：当前

## 使用方法

用一个已知正常的 SocketCAN 设备（如 PCAN-USB）验证网关某一路的收发：

```bash
sudo ip link set can0 type can bitrate 1000000 sample-point 0.75
sudo ip link set can0 up
./scripts/can-linktest.sh can0 enp3s0 40
```

它会自检 `can0` 的网卡层 / CAN 层状态、统计 `cansend` 成败、并打印网关收到的帧。
**改线之后先跑这个。**

> ⚠️ 两个坑：
> - PCAN-USB **每次重新插拔都会掉回 DOWN**，必须先重配 bitrate 再 up
> - **`cansend` 返回 0 只代表帧进了发送队列，不代表总线上有人 ACK。**
>   判断链路通不通，唯一有效的判据是**网关是否真的收到了帧**

---
