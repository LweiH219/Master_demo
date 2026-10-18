/*
 * EtherCAT 生命周期与 PDO 映射接口。
 *
 * 命令和测试代码通过此结构访问 SOEM 上下文；模块负责初始化、进入 OP、
 * 周期过程数据收发以及失败/正常退出时的资源清理。
 */
#ifndef ECAT2CAN_FIELDBUS_H
#define ECAT2CAN_FIELDBUS_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "soem/soem.h"
#include "ecat2can_proto.h"

typedef struct {
    ecx_contextt ctx;       /* SOEM 主站上下文，包含从站表、PDO 映射和网卡状态 */
    const char *iface;      /* EtherCAT 网卡名称，例如 enp3s0 */
    uint8 map[4096];        /* SOEM 配置 PDO 时使用的过程数据映射缓冲区 */
    int roundtrip_us;       /* 最近一次过程数据收发耗时，单位：微秒 */
    int slavecount;         /* 最近一次成功扫描到的从站数量 */
    int slave_idx;          /* 当前命令操作的从站编号，从 1 开始 */
    int context_ready;      /* SOEM 上下文已初始化，退出时需要调用 ecx_close */
    int operational;        /* 从站已进入 OP，释放前需要关闭 DC 同步 */
} fieldbus_t;

/*
 * 组内 PDO 按固定的 ECAT2CAN_PDO_BYTES 字节排列，每个从站占一段。
 * FB_OUT/FB_IN 使用 fieldbus_t 当前选中的从站；FB_OUT_N/FB_IN_N 显式指定从站。
 * 所有从站编号均从 1 开始，SOEM 的 0 号项是广播/主站项。
 */
#define FB_OUT(fb, grp) ((ecat2can_tx_rx_message *) \
    ((uint8_t *)(grp)->outputs + (size_t)((fb)->slave_idx - 1) * ECAT2CAN_PDO_BYTES))
#define FB_IN(fb, grp) ((const ecat2can_tx_rx_message *) \
    ((const uint8_t *)(grp)->inputs + (size_t)((fb)->slave_idx - 1) * ECAT2CAN_PDO_BYTES))
#define FB_OUT_N(fb, grp, n) ((ecat2can_tx_rx_message *) \
    ((uint8_t *)(grp)->outputs + (size_t)((n) - 1) * ECAT2CAN_PDO_BYTES))
#define FB_IN_N(fb, grp, n) ((const ecat2can_tx_rx_message *) \
    ((const uint8_t *)(grp)->inputs + (size_t)((n) - 1) * ECAT2CAN_PDO_BYTES))

extern volatile sig_atomic_t g_stop;

void fb_request_stop(int signal_number);
int fb_roundtrip(fieldbus_t *fb);
int fb_start(fieldbus_t *fb);
void fb_stop(fieldbus_t *fb);

#endif
