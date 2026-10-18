#include "motor_control.h"

#include <stddef.h>

/* DJI 将四个有符号 int16 指令打包进一帧标准 CAN 报文。C620 使用
 * 0x200/0x1FF 分别承载 ID 1~4/5~8，选定电机的指令写入其中一个槽位。 */
#define C620_CTRL_ID_LO  0x200u
#define C620_CTRL_ID_HI  0x1FFu
#define C620_FB_ID_BASE  0x200u /* 反馈 ID = 0x200 + 电机 ID。 */
#define C620_CUR_RAW_MAX 16384.0f /* 电流指令协议满量程。 */
#define C620_CUR_AMP_MAX 20.0f    /* ±16384 对应 ±20 A。 */

/* GM6020 在电压模式和电流模式下使用不同的控制 ID。电压模式支持 ID 1~7；
 * 电流模式仍采用四槽打包，但原始电流范围为 ±3 A。 */
#define GM_CTRL_ID_LO_V  0x1FFu
#define GM_CTRL_ID_HI_V  0x2FFu
#define GM_CTRL_ID_LO_I  0x1FEu
#define GM_CTRL_ID_HI_I  0x2FEu
#define GM_FB_ID_BASE    0x204u /* 反馈 ID = 0x204 + 驱动器 ID。 */
#define GM_VOLT_RAW_MAX  25000.0f /* 电压指令原始计数满量程。 */
#define GM_CUR_RAW_MAX   16384.0f /* 电流指令原始计数满量程。 */
#define GM_CUR_AMP_MAX   3.0f     /* ±16384 对应 ±3 A。 */

float motor_lpf_alpha(float tau_s, float dt)
{
    /* tau=0 表示调用者明确要求不滤波。 */
    return tau_s <= 0.0f ? 1.0f : dt / (tau_s + dt);
}

void motor_pid_reset(pidc_t *p)
{
    p->integ = 0.0f;
    p->prev_meas = 0.0f;
    p->d_lpf = 0.0f;
    p->out = 0.0f;
}

float motor_pid_step(pidc_t *p, float error, float measurement, float dt)
{
    float d_raw, candidate, unclamped;
    if (dt <= 0.0f) return p->out;
    /* 先对积分候选值限幅；如果输出已经饱和且误差还会把输出推向同一
     * 方向，下面的条件更新会拒绝该候选值，从而实现抗积分饱和。 */
    candidate = p->integ + error * dt;
    if (candidate > p->i_limit) candidate = p->i_limit;
    if (candidate < -p->i_limit) candidate = -p->i_limit;
    /* 对测量值而不是误差微分，避免目标阶跃带来微分冲击；负号符合
     * 常用的“对测量值微分”PID 约定。 */
    d_raw = -(measurement - p->prev_meas) / dt;
    p->prev_meas = measurement;
    p->d_lpf += p->a_d * (d_raw - p->d_lpf);
    unclamped = p->kp * error + p->ki * candidate + p->kd * p->d_lpf;
    if (unclamped > p->out_limit && error > 0.0f) {
        /* 输出顶在正限幅且误差同向时保持原积分值。 */
    } else if (unclamped < -p->out_limit && error < 0.0f) {
        /* 输出顶在负限幅且误差同向时保持原积分值。 */
    } else {
        p->integ = candidate;
    }
    p->out = p->kp * error + p->ki * p->integ + p->kd * p->d_lpf;
    if (p->out > p->out_limit) p->out = p->out_limit;
    if (p->out < -p->out_limit) p->out = -p->out_limit;
    return p->out;
}

int motor_cfg_init(motor_cfg_t *c)
{
    /* ctrl_id 选择共享的四槽控制帧。下面根据从 1 开始的电机 ID 计算
     * ctrl_slot：ID 1 占用字节 [0..1]，ID 2 占用 [2..3]，依此类推。
     * GM6020 的 ID 5~7 只有三个有效槽位，因为该驱动器没有 ID 8。 */
    if (c->kind == MOTOR_M3508) {
        if (c->id < 1 || c->id > 8) return -1;
        c->ctrl_id = c->id <= 4 ? C620_CTRL_ID_LO : C620_CTRL_ID_HI;
        c->ctrl_slots = 4; c->fb_id = C620_FB_ID_BASE + (uint32_t)c->id;
        c->out_raw_max = C620_CUR_RAW_MAX; c->out_fs = C620_CUR_AMP_MAX;
        c->out_unit = "A"; c->cur_raw_max = C620_CUR_RAW_MAX; c->cur_fs = C620_CUR_AMP_MAX;
    } else {
        if (c->id < 1 || c->id > 7) return -1;
        if (c->gm_current_mode) {
            c->ctrl_id = c->id <= 4 ? GM_CTRL_ID_LO_I : GM_CTRL_ID_HI_I;
            c->out_raw_max = GM_CUR_RAW_MAX; c->out_fs = GM_CUR_AMP_MAX;
            c->out_unit = "A"; c->cur_raw_max = GM_CUR_RAW_MAX; c->cur_fs = GM_CUR_AMP_MAX;
        } else {
            c->ctrl_id = c->id <= 4 ? GM_CTRL_ID_LO_V : GM_CTRL_ID_HI_V;
            c->out_raw_max = GM_VOLT_RAW_MAX; c->out_fs = GM_VOLT_RAW_MAX;
            c->out_unit = "计数"; c->cur_raw_max = 0.0f; c->cur_fs = 0.0f;
        }
        c->ctrl_slots = c->id <= 4 ? 4 : 3;
        c->fb_id = GM_FB_ID_BASE + (uint32_t)c->id;
    }
    c->ctrl_slot = (c->id - 1) % 4;
    return 0;
}

void motor_state_reset(motor_state_t *m) { *m = (motor_state_t){0}; }

void motor_state_decode(motor_state_t *m, const uint8_t data[8])
{
    int32_t delta;

    /* DJI 反馈采用大端排列：编码器、转速、转矩/电流，最后是温度和错误码。
     * 编码器每 8192 个计数回绕，因此先取 ±4096 范围内的最短有符号差值，
     * 再累加得到多圈位置。 */
    m->last_ecd = m->ecd;
    m->ecd = (uint16_t)((data[0] << 8) | data[1]);
    m->rpm = (int16_t)((data[2] << 8) | data[3]);
    m->torque_raw = (int16_t)((data[4] << 8) | data[5]);
    m->temp = data[6]; m->err = data[7];
    delta = (int32_t)m->ecd - (int32_t)m->last_ecd;
    if (delta > 4096) delta -= 8192;
    if (delta < -4096) delta += 8192;
    m->total_ecd += delta;
    /* 首帧只建立当前位置参考，不能把结构体初始的零值误认为实际运动。 */
    if (!m->seen) { m->seen = 1; m->total_ecd = 0; }
    m->lost_ms = 0;
}

motor_fault_t motor_state_fault(const motor_state_t *m)
{
    if (!m->seen || m->lost_ms > MOTOR_FB_TIMEOUT_MS) return MOTOR_FAULT_FEEDBACK_TIMEOUT;
    if (m->temp >= MOTOR_TEMP_LIMIT_C) return MOTOR_FAULT_OVERTEMP;
    if (m->err != 0) return MOTOR_FAULT_DRIVER;
    return MOTOR_FAULT_NONE;
}

float motor_rotor_deg(const motor_state_t *m) { return m->total_ecd * (360.0f / MOTOR_ROTOR_CPR); }
float motor_shaft_deg(const motor_state_t *m, float ratio) { return motor_rotor_deg(m) / ratio; }

float motor_physical_to_raw(const motor_cfg_t *c, float physical, float limit)
{
    /* 用户输入始终使用物理单位，最后再映射到协议的有符号原始范围；
     * 运行时通过 m 命令修改 --limit 时也由这里统一处理。 */
    if (limit < 0.0f) limit = 0.0f;
    if (limit > c->out_fs) limit = c->out_fs;
    if (physical > limit) physical = limit;
    if (physical < -limit) physical = -limit;
    return physical / c->out_fs * c->out_raw_max;
}

float motor_raw_to_physical(const motor_cfg_t *c, float raw)
{
    return raw / c->out_raw_max * c->out_fs;
}
