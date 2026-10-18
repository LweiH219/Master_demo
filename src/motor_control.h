/*
 * motor_control.h - 与硬件解耦的 DJI 电机控制定义层
 *
 * 本模块不依赖 EtherCAT、终端输入或 CAN 传输，只接收每周期解码后的
 * DJI 反馈帧，并产生电机协议使用的有符号原始指令。
 */
#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <stdint.h>

/* DJI 13 位编码器：一圈机械角度对应 0~8191，共 8192 个计数。 */
#define MOTOR_ROTOR_CPR        8192.0f
/* M3508/P19 减速比（转子转数 : 输出轴转数）。 */
#define MOTOR_M3508_RATIO_DEF  (3591.0f / 187.0f)
/* 连续超过该时间未收到反馈时切断输出。 */
#define MOTOR_FB_TIMEOUT_MS    100
/* 示例把达到该温度视为过温故障。 */
#define MOTOR_TEMP_LIMIT_C     100
/* 命令行允许的最大输出轴目标角度。 */
#define MOTOR_TARGET_LIMIT_DEG 7200.0f

typedef enum {
    MOTOR_M3508 = 0, /* M3508 电机，由 C620 以电流指令驱动。 */
    MOTOR_GM6020 = 1 /* GM6020 一体化驱动器，可选电压/电流模式。 */
} motor_kind_t;

typedef struct {
    float kp, ki, kd;             /* 当前控制环单位下的 P/I/D 增益。 */
    float i_limit, out_limit;     /* 积分状态限幅和输出限幅。 */
    float integ;                  /* 累积误差（乘以 ki 之前）。 */
    float prev_meas;              /* 上次测量值，用于对测量值微分。 */
    float d_lpf, a_d;             /* 滤波后的微分值和滤波系数。 */
    float out;                    /* 上次限幅后的输出，dt 无效时保持。 */
} pidc_t;

typedef struct {
    uint16_t ecd, last_ecd;       /* 当前/上次 13 位编码器计数。 */
    int32_t total_ecd;            /* 从首帧开始展开后的多圈累计计数。 */
    int16_t rpm;                  /* 驱动器报告的转子转速。 */
    int16_t torque_raw;            /* 转矩电流（或驱动器原始量）反馈。 */
    uint8_t temp;                 /* 驱动器报告的温度（℃）。 */
    uint8_t err;                  /* 非零表示驱动器错误码。 */
    int seen;                     /* 是否至少收到过一帧有效反馈。 */
    int lost_ms;                  /* 距离上次有效反馈经过的毫秒数。 */
} motor_state_t;

typedef struct {
    motor_kind_t kind;            /* 示例选择的协议类型。 */
    int id;                       /* DJI 电机/驱动器 ID，从 1 开始。 */
    int gm_current_mode;          /* GM6020：1 为电流指令，0 为电压指令。 */
    uint32_t ctrl_id, fb_id;      /* 控制帧和反馈帧的 CAN ID。 */
    int ctrl_slot, ctrl_slots;    /* 控制帧中的 0 基 int16 槽位及有效槽位数。 */
    float out_raw_max, out_fs;    /* 原始量满量程及对应的物理量满量程。 */
    const char *out_unit;         /* 物理指令单位（"A" 或 "计数"）。 */
    float cur_raw_max, cur_fs;    /* 反馈电流换算参数；不可换算时为 0。 */
} motor_cfg_t;

typedef enum {
    MOTOR_FAULT_NONE = 0,
    MOTOR_FAULT_FEEDBACK_TIMEOUT, /* 尚未收到反馈，或反馈超时。 */
    MOTOR_FAULT_OVERTEMP,         /* 温度达到 MOTOR_TEMP_LIMIT_C。 */
    MOTOR_FAULT_DRIVER,           /* 驱动器报告非零错误码。 */
} motor_fault_t;

/* 根据时间常数和周期计算一阶低通滤波系数。 */
float motor_lpf_alpha(float tau_s, float dt);
/* 清零动态状态，保留已配置的增益和限幅。 */
void motor_pid_reset(pidc_t *pid);
/* 执行一次 PID；微分使用测量值，避免目标阶跃造成微分冲击。 */
float motor_pid_step(pidc_t *pid, float error, float measurement, float dt);
/* 根据电机类型和 ID 填充协议 ID、控制槽位及单位换算参数。 */
int motor_cfg_init(motor_cfg_t *cfg);
/* 清除反馈历史和多圈累计计数。 */
void motor_state_reset(motor_state_t *state);
/* 解码 DJI 8 字节大端反馈帧，并展开编码器回绕计数。 */
void motor_state_decode(motor_state_t *state, const uint8_t data[8]);
/* 根据反馈状态给出安全联锁结果。 */
motor_fault_t motor_state_fault(const motor_state_t *state);
/* 把展开后的转子计数换算为转子角度和输出轴角度。 */
float motor_rotor_deg(const motor_state_t *state);
float motor_shaft_deg(const motor_state_t *state, float ratio);

/* 对物理指令限幅，并换算为电机协议使用的原始指令单位。 */
float motor_physical_to_raw(const motor_cfg_t *cfg, float physical, float limit);
/* 把原始指令或反馈值换算回当前配置的物理单位。 */
float motor_raw_to_physical(const motor_cfg_t *cfg, float raw);

#endif
