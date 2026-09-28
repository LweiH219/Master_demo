/*
 * ecat2can_proto.h —— 港科大 ECAT2CAN 的 192 字节 PDO 协议（主机侧独立定义）
 *
 * 与从站 `application/ecat2can.h` + `bsp_can.h` 逐位对应。
 * 主机侧所有程序都 include 这个头，避免各处重复定义导致结构体布局漂移。
 *
 * ★ 本文件是主机侧**独立**定义，不依赖任何具体的板级 demo /
 *   固件工程目录。只要从站跑的是港科大 ECAT2CAN 协议即可通用：
 *      - hpm5e3ykit 单路版（通道 3）
 *      - ECAN-Lite / 港科广 5e3y 四路版（通道 0~3）
 */
#ifndef ECAT2CAN_PROTO_H
#define ECAT2CAN_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ===================== 协议常量 ===================== */
#define ECAT2CAN_MAX_FRAME_COUNT 17u
#define ECAT2CAN_PDO_BYTES       192u
#define ECAT2CAN_STD_ID_MAX      0x7FFu
#define ECAT2CAN_DLC_MAX         8u
#define ECAT2CAN_CAN_CHANNEL_MAX 3u

/* 默认发送通道。
 * 港科广原版 / ECAN-Lite 是四路全引出，通道号 0~3 全可用；
 * hpm5e3ykit 单路版固定走 3。命令行都支持显式指定，这里只作兜底默认。 */
#define ECAT2CAN_CAN_CHANNEL_DEFAULT 3u

/* 兼容旧名字（单路版代码里用得多），新代码建议用 _DEFAULT */
#define ECAT2CAN_CAN_CHANNEL ECAT2CAN_CAN_CHANNEL_DEFAULT

/* 通道总数 = MAX + 1 */
#define ECAT2CAN_CAN_CHANNEL_COUNT (ECAT2CAN_CAN_CHANNEL_MAX + 1u)

#define ECAT2CAN_STATUS_OK    0x00u
#define ECAT2CAN_STATUS_ERROR 0x01u

#define ECAT2CAN_ERR_BAD_COMMAND (1u << 0)
#define ECAT2CAN_ERR_BAD_CHANNEL (1u << 1)
#define ECAT2CAN_ERR_BAD_DLC     (1u << 2)
#define ECAT2CAN_ERR_BAD_STD_ID  (1u << 3)
#define ECAT2CAN_ERR_TOO_MANY    (1u << 4)
#define ECAT2CAN_ERR_RX_DROPPED  (1u << 5)

typedef enum {
    LOOP_TEST = 0x00,  /* 从站把下行 PDO 原样回填到上行 PDO（不碰 CAN） */
    WORK_MODE = 0x01,  /* 正常转发 CAN 消息 */
    HW_RESET  = 0x02,  /* 从站看门狗强制复位 */
    ONLY_FDB  = 0x03,  /* 只收集 CAN 总线反馈，不下发 */
} ecat2can_command_t;

/* ===================== PDO 数据结构 =====================
 * 必须和从站完全一致：GCC 在 pack(1) 下的位域分配规则决定了
 * sizeof(can_frame_struct) == 11，从而 5 + 17*11 == 192。
 * 下面的 _Static_assert 会在编译期把这件事钉死。
 */
#pragma pack(push, 1)

typedef struct {
    uint8_t  can_channel;  /* CAN 通道号 */
    uint32_t std_id : 11;  /* 标准帧 ID */
    uint16_t dlc : 4;      /* 数据长度 */
    uint8_t  data[8];      /* 数据 */
} can_frame_struct;

typedef struct {
    uint8_t command;      /* ecat2can_command_t */
    uint8_t frame_count;  /* 有效 CAN 帧数，最大 17 */
    uint8_t seq;          /* 主站下发序号 */
    uint8_t status;       /* 从站反馈状态，0 正常 */
    uint8_t error_flags;  /* 从站反馈错误标志 */
    can_frame_struct can_frames[ECAT2CAN_MAX_FRAME_COUNT];
} ecat2can_tx_rx_message;

#pragma pack(pop)

_Static_assert(sizeof(can_frame_struct) == 11, "can_frame_struct 必须是 11 字节");
_Static_assert(sizeof(ecat2can_tx_rx_message) == ECAT2CAN_PDO_BYTES,
               "PDO 必须是 192 字节");
_Static_assert(offsetof(ecat2can_tx_rx_message, can_frames) == 5,
               "can_frames 必须从偏移 5 开始");

/* 通道号 -> 简短可读名，例如 "0(MCAN0)"。buf 至少 16 字节 */
static inline const char *ecat2can_ch_str(unsigned ch, char *buf, size_t n)
{
    snprintf(buf, n, "%u(MCAN%u)", ch, ch);
    return buf;
}

/* 把错误标志拼成可读字符串（不含换行）。buf 至少 96 字节 */
static inline const char *ecat2can_err_str(uint8_t ef, char *buf, size_t n)
{
    size_t used = 0;
    buf[0] = '\0';
    if (ef == 0) {
        snprintf(buf, n, "无");
        return buf;
    }
#define APP(bit, name)                                                  \
    do {                                                                \
        if ((ef) & (bit)) {                                             \
            int w = snprintf(buf + used, n - used, "%s ", name);        \
            if (w > 0 && (size_t)w < n - used) { used += (size_t)w; }   \
        }                                                               \
    } while (0)
    APP(ECAT2CAN_ERR_BAD_COMMAND, "BAD_COMMAND");
    APP(ECAT2CAN_ERR_BAD_CHANNEL, "BAD_CHANNEL");
    APP(ECAT2CAN_ERR_BAD_DLC, "BAD_DLC");
    APP(ECAT2CAN_ERR_BAD_STD_ID, "BAD_STD_ID");
    APP(ECAT2CAN_ERR_TOO_MANY, "TOO_MANY");
    APP(ECAT2CAN_ERR_RX_DROPPED, "RX_DROPPED");
#undef APP
    return buf;
}

#endif /* ECAT2CAN_PROTO_H */
