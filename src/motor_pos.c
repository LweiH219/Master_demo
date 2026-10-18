/*
 * motor_pos.c —— DJI 电机应用示例（用于验证 ECAT2CAN 的真实 CAN 转发）
 * ===========================================================================
 * 支持：M3508 + C620 电调 / GM6020（自带驱动器）
 *
 * 链路：
 *   Linux(本程序) ──EtherCAT 1kHz──► hpm5e3kit 网关 ──CAN 1Mbps──► 电机
 *        ▲                                                            │
 *        └──────────── EtherCAT 上行 PDO（电机反馈）◄──────────────────┘
 *
 * 协议依据（官方手册）：
 *   【C620 + M3508】
 *     控制 0x200(ID1~4) / 0x1FF(ID5~8)，标准帧 DLC 8，4×int16 大端
 *       电流给定 -16384~0~16384 ←→ 转矩电流 -20~0~20A
 *     反馈 0x200+ID，1kHz：[0:2]转子机械角度 0~8191、[2:4]转子转速 rpm、
 *          [4:6]实际转矩电流、[6]温度、[7]电机错误码
 *     减速比 P19（3591/187≈19.2），相对编码器
 *   【GM6020】
 *     控制 0x1FF(ID1~4) / 0x2FF(ID5~7)，标准帧 DLC 8，4×int16 大端
 *       电压给定 -25000~0~25000（默认电压模式）
 *       另有电流模式 0x1FE/0x2FE，-16384~0~16384 ↔ -3~3A，
 *       需固件 ≥1.0.11.2 且用 RoboMaster Assistant 打开电流环开关
 *     反馈 0x204+驱动器ID（0x205~0x20B），1kHz，布局与 C620 相同
 *     直驱无减速箱（减速比 1），绝对编码器
 *
 * 控制结构：位置环（轴角度 → 目标转速）串 速度环（rpm → 输出量）。
 * 本程序是应用层示例；ECAT2CAN 协议、从站状态机和实时性测试请使用
 * ecat2can_master，不要把本程序视为主站核心。
 *   速度环内部统一用**原始计数**运算，这样两种电机的增益数值可以直接套用；
 *   对外的 --limit / c / m 才换算成物理量（m3508 用 A，gm6020 用计数）。
 *
 * 终端界面：
 *   用 ANSI 滚动区把终端最后一行留给状态显示，命令与消息在上方滚动，
 *   两者不再互相覆盖。stdout 不是 tty 时自动退化为逐行打印。
 *
 * 安全设计：
 *   · 上电默认**失能**，必须手动按 e 才输出
 *   · 使能瞬间把目标位置设成当前位置（原地保持，不会突然窜出去）
 *   · 输出限幅 / 转速限幅 / 目标角度限幅
 *   · 反馈超时 100ms / 温度过高 / 驱动器报错误码 → 立刻切断输出
 *   · 发散保护：位置误差连续 0.5s 增大 → 判定闭环符号反了，切输出并提示
 *   · Ctrl+C 或退出时，先发 100 个周期的零输出再关网卡
 *   · --monitor 只读监视模式：硬禁止使能，输出恒为 0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <sys/ioctl.h>

#include "soem/soem.h"
#include "ecat2can_proto.h"
#include "motor_control.h"

/* 电机协议、反馈解码和 PID 实现在 motor_control.c；本文件只编排 UI、现场总线和周期调度。 */

/* ===================== 终端 UI =====================
 * 把终端最后一行留给状态显示：滚动区设为 1..rows-1，状态行画在第 rows 行。
 * 命令输入和消息都在滚动区里滚动，永远不会和状态行互相覆盖。
 * stdout 不是 tty（重定向到文件/管道）时自动退化为逐行打印。
 */
typedef struct {
    int  enabled;           /* 是否启用 ANSI 固定状态栏。 */
    int  rows;              /* 终端行数。 */
    int  width;             /* 用于截断状态文本的终端宽度。 */
    int  top;               /* 滚动区最后一行，也就是提示符所在行。 */
    char status1[256];      /* 固定状态栏第一行。 */
    char status2[256];      /* 固定状态栏第二行。 */
    char input[80];         /* 当前提示符显示的输入行副本。 */
    size_t input_len;       /* input 中的有效字节数。 */
} ui_t;

static ui_t g_ui;

static void ui_draw_status(void);
static void ui_draw_prompt(void);

static void ui_init(void)
{
    struct winsize ws;

    g_ui.enabled = 0;
    g_ui.rows = 0;
    g_ui.width = 80;
    g_ui.status1[0] = g_ui.status2[0] = '\0';
    g_ui.input_len = 0;

    if (!isatty(STDOUT_FILENO)) return;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return;
    if (ws.ws_row < 10) return;                /* 终端太矮就不折腾了 */

    g_ui.rows  = ws.ws_row;
    g_ui.width = ws.ws_col > 0 ? ws.ws_col : 80;
    g_ui.top   = g_ui.rows - 2;                /* 最后两行留给状态栏 */
    g_ui.enabled = 1;

    printf("\033[1;%dr", g_ui.top);            /* 滚动区 1..rows-2 */
    printf("\033[%d;1H", g_ui.top);            /* 光标放到滚动区底部 */
    fflush(stdout);
}

static void ui_restore(void)
{
    if (!g_ui.enabled) return;
    printf("\033[%d;1H\033[2K", g_ui.top);
    printf("\033[%d;1H\033[2K", g_ui.rows - 1);
    printf("\033[%d;1H\033[2K", g_ui.rows);
    printf("\033[r");                          /* 恢复整屏滚动区 */
    printf("\033[%d;1H\n", g_ui.rows);
    fflush(stdout);
    g_ui.enabled = 0;
}

static void ui_draw_status(void)
{
    if (!g_ui.enabled || g_ui.status1[0] == '\0') return;
    /* 超宽会折行、破坏固定状态栏，直接按终端宽度截断 */
    g_ui.status1[g_ui.width - 1] = '\0';
    g_ui.status2[g_ui.width - 1] = '\0';
    printf("\033[s");                                          /* 保存光标 */
    printf("\033[%d;1H\033[2K%s", g_ui.rows - 1, g_ui.status1);
    printf("\033[%d;1H\033[2K%s", g_ui.rows,     g_ui.status2);
    printf("\033[u");                                          /* 恢复光标 */
    fflush(stdout);
}

static void ui_draw_prompt(void)
{
    if (!g_ui.enabled) return;
    printf("\033[%d;1H\033[2K> %.*s", g_ui.top,
           (int)g_ui.input_len, g_ui.input);
    fflush(stdout);
}

/* 打印一条消息：先清提示行，打印后把状态行和提示行补回来 */
static void ui_msg(const char *fmt, ...)
{
    va_list ap;

    if (!g_ui.enabled) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
        fflush(stdout);
        return;
    }

    printf("\033[%d;1H\033[2K", g_ui.top);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    ui_draw_status();
    ui_draw_prompt();
}

static void ui_set_status_lines(const char *l1, const char *l2)
{
    snprintf(g_ui.status1, sizeof(g_ui.status1), "%s", l1);
    snprintf(g_ui.status2, sizeof(g_ui.status2), "%s", l2);
    ui_draw_status();
}

/* ===================== 现场总线 ===================== */
typedef struct {
    ecx_contextt ctx;       /* SOEM 上下文及已发现的从站状态。 */
    const char  *iface;     /* 传给 SOEM 的 Linux 网卡名。 */
    uint8_t      map[4096]; /* SOEM PDO 映射使用的工作区。 */
    int          roundtrip_us; /* 最近一次收发往返耗时。 */
    int          obytes, ibytes; /* 映射后的下行/上行 PDO 字节数。 */
    int          expected_wkc; /* 正常过程数据交换时的期望 WKC。 */
} fieldbus_t;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static int fb_roundtrip(fieldbus_t *fb)
{
    ec_timet t0, t1, diff;
    int wkc;

    t0 = osal_current_time();
    ecx_send_processdata(&fb->ctx);
    wkc = ecx_receive_processdata(&fb->ctx, EC_TIMEOUTRET);
    t1 = osal_current_time();
    osal_time_diff(&t0, &t1, &diff);
    fb->roundtrip_us = (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000);
    return wkc;
}

static uint8_t *fb_out(fieldbus_t *fb) { return fb->ctx.grouplist[0].outputs; }
static uint8_t *fb_in (fieldbus_t *fb) { return fb->ctx.grouplist[0].inputs;  }

static int fb_open(fieldbus_t *fb, const char *iface)
{
    ecx_contextt *ctx = &fb->ctx;

    memset(fb, 0, sizeof(*fb));
    fb->iface = iface;

    if (!ecx_init(ctx, iface)) {
        printf("!! 打不开网卡 %s（需要 CAP_NET_RAW：sudo setcap cap_net_raw,cap_net_admin+eip ./motor_pos）\n",
               iface);
        return -1;
    }
    if (ecx_config_init(ctx) <= 0) {
        printf("!! 没扫到 EtherCAT 从站。检查网关是否上电、网线是否插 Port0\n");
        return -2;
    }
    printf("扫到 %d 个 EtherCAT 从站\n", ctx->slavecount);

    ecx_config_map_group(ctx, fb->map, 0);
    fb->obytes = ctx->grouplist[0].Obytes;
    fb->ibytes = ctx->grouplist[0].Ibytes;
    fb->expected_wkc = ctx->grouplist[0].outputsWKC * 2 + ctx->grouplist[0].inputsWKC;

    if (fb->obytes != (int)ECAT2CAN_PDO_BYTES || fb->ibytes != (int)ECAT2CAN_PDO_BYTES) {
        printf("!! 期望 PDO 192/192，实际 %d/%d\n", fb->obytes, fb->ibytes);
        return -3;
    }
    printf("PDO 映射：下行 %d 字节 / 上行 %d 字节\n", fb->obytes, fb->ibytes);
    return 0;
}

static int fb_to_op(fieldbus_t *fb)
{
    ecx_contextt *ctx = &fb->ctx;
    ec_slavet *slave;
    int i;

    ecx_configdc(ctx);
    ecx_statecheck(ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
    fb_roundtrip(fb);

    slave = ctx->slavelist;
    slave->state = EC_STATE_OPERATIONAL;
    ecx_writestate(ctx, 0);

    for (i = 0; i < 20 && !g_stop; i++) {
        fb_roundtrip(fb);
        ecx_statecheck(ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 20);
        if (slave->state == EC_STATE_OPERATIONAL) {
            ecx_dcsync0(ctx, 1, TRUE, 1000000, 0);   /* 1 ms 周期 */
            return 0;
        }
    }
    printf("!! 从站进不了 OP\n");
    return -4;
}

static void fb_close(fieldbus_t *fb)
{
    ecx_dcsync0(&fb->ctx, 1, FALSE, 0, 0);
    fb->ctx.slavelist->state = EC_STATE_INIT;
    ecx_writestate(&fb->ctx, 0);
    ecx_close(&fb->ctx);
}

/* ===================== 运行参数 ===================== */
typedef struct {
    motor_kind_t kind;      /* M3508/C620 或 GM6020 协议类型。 */
    int   id;               /* CAN 总线上的电机/驱动器 ID，从 1 开始。 */
    int   ch;               /* CAN 通道 0~3，ECAN-Lite 有四路通道。 */
    float ratio;            /* 转子转数与输出轴转数之比。 */
    int   ratio_given;      /* 是否显式指定 --ratio，否则使用机型默认值。 */
    float target_deg;       /* 输出轴目标角度，不是转子角度。 */
    float limit;            /* 当前物理单位下的输出限幅（A 或计数）。 */
    int   limit_given;      /* 是否显式指定 --limit，否则使用机型/模式默认值。 */
    float max_rpm;          /* 位置环输出限幅，单位为转子 rpm。 */
    int   log_ms;           /* 非交互输出的状态打印周期，单位毫秒。 */
    int   invert_spd;       /* 按需要反转反馈转速符号。 */
    int   monitor;          /* 只读模式，强制禁止输出。 */
    int   csv;              /* 输出 GUI 可读取的结构化遥测采样。 */
    int   gm_current_mode;  /* GM6020：1 为电流模式(0x1FE/0x2FE)，0 为电压模式。 */
    float filter_ms;        /* 反馈低通时间常数，不是采样周期；0 表示关闭。 */
    int   cur_loop;         /* 可选的主机侧电流环，默认关闭。 */
    unsigned gain_given;    /* 位掩码，记录命令行显式指定过的增益。 */
    float pos_kp, pos_ki, pos_kd; /* 位置环增益，输入为输出轴角度误差。 */
    float spd_kp, spd_ki, spd_kd; /* 速度环增益，输入单位为 rpm。 */
    float cur_kp, cur_ki;   /* 可选主机侧电流环增益。 */
} opts_t;

static void usage(const char *p)
{
    printf(
"用法: %s <网卡名> [选项]\n"
"\n"
"电机选择:\n"
"  --motor m3508|gm6020   电机类型（默认 m3508）\n"
"  --id N                 电机/驱动器 ID（m3508: 1~8, gm6020: 1~7，默认 1）\n"
"  --ch N                 CAN 通道号 0~%u（ECAN-Lite: CN4=0 CN5=1 CN9=2 CN1=3；\n"
"                         hpm5e3ykit 单路版只有 3。默认 %u）\n"
"  --ratio F              减速比（m3508 默认 %.4f = 3591/187，官方标称 19；\n"
"                         gm6020 默认 1，直驱无减速箱）\n"
"  --gm-voltage           GM6020 改用电压模式(0x1FF/0x2FF，-25000~25000)\n"
"                         【默认是电流模式】0x1FE/0x2FE，-16384~16384 ←→ -3~3A，\n"
"                         与 M3508 统一；但需要固件≥1.0.11.2，且用 RoboMaster\n"
"                         Assistant 打开电流环开关，否则电机不会响应\n"
"\n"
"控制:\n"
"  --target F             初始目标位置，单位=输出轴角度（默认 0）\n"
"  --limit F              输出限幅，物理单位随电机类型：\n"
"                           m3508  = 转矩电流 A（默认 3.0，额定 10A，电调上限 20A）\n"
"                           gm6020 电流模式 = A（默认 1.0，上限 3.0）\n"
"                           gm6020 电压模式 = 计数（默认 3000，上限 25000）\n"
"  --max-rpm F            位置环输出限幅，转子 rpm（默认 2000）\n"
"  --invert-spd           把反馈转速符号取反（默认不取；闭环发散时再试）\n"
"  --monitor              只读监视模式：硬禁止使能，输出恒为 0\n"
"  --csv                  额外输出 MOTOR_CSV 遥测行（供图形界面绘图）\n"
"  --log-ms N             非交互模式下的状态打印间隔 ms（默认 200）\n"
"\n"
"反馈滤波（重要）:\n"
"  --filter-ms N          反馈低通时间常数 ms（默认 5，0 = 不滤）\n"
"                         位置微分和转速反馈都用它。本链路往返延迟约 3~4ms，\n"
"                         而且电机反馈(1kHz)与 EtherCAT 周期(1kHz)不同步，\n"
"                         不滤波极易高频自激\n"
"\n"
"增益（也可运行时用 kp/ki/kd/skp/ski/skd 在线改）:\n"
"  位置环 --pos-kp/--pos-ki/--pos-kd : m3508 默认 8/0/0   gm6020 默认 4/0/0\n"
"                                      kd 默认 0（微分已由速度环提供阻尼）\n"
"  速度环 --spd-kp/--spd-ki/--spd-kd : m3508 默认 20/0.2/0  gm6020 默认 4/0.05/0\n"
"                                      单位 = 原始计数/rpm\n"
"\n"
"可选（一般不需要）:\n"
"  --cur-loop             host 侧再套一层电流环，默认关。\n"
"                         驱动器内部已有几 kHz 带宽的电流环，而我们往返延迟\n"
"                         3~4ms，主机侧电流环带宽上限只有约 80Hz —— 慢环套快环\n"
"                         只会增加滞后。留作对比实验用\n"
"  --cur-kp/--cur-ki      上面那个环的增益（默认 0.3 / 0.02）\n"
"  -h, --help             显示本帮助\n"
"\n"
"运行时命令（敲完按回车）:\n"
"  e               使能（目标先设为当前位置，原地保持）\n"
"  d               失能（输出 0）\n"
"  z / s           设零点 / 目标设为当前位置\n"
"  <数字>          设定目标位置，输出轴角度（如 90 或 -45.5）\n"
"  c <值>          开环输出测试，绕过 PID（c 0 退出）\n"
"  m <值>          输出限幅（单位同 --limit）\n"
"  kp/ki/kd <v>    位置环增益；skp/ski/skd <v> 速度环增益\n"
"  g               打印当前增益与限幅\n"
"  q / ?           退出 / 帮助\n"
"\n"
"状态行末段的「电流 实测/指令」可以直接看出驱动器的电流环有没有跟上。\n"
"\n"
"例:\n"
"  %s enp3s0 --motor m3508  --id 1 --limit 10\n"
"  %s enp3s0 --motor gm6020 --id 1 --limit 1\n"
"  %s enp3s0 --motor gm6020 --id 1 --gm-voltage --limit 3000\n",
    p, ECAT2CAN_CAN_CHANNEL_MAX, ECAT2CAN_CAN_CHANNEL_DEFAULT,
    MOTOR_M3508_RATIO_DEF, p, p, p);
}

/* ===================== 运行时控制台 ===================== */
typedef struct {
    int   enabled;        /* 用户按 e 后才允许输出。 */
    int   open_loop;      /* 旁路位置/速度 PID，直接测试指令输出。 */
    float ol_out;         /* 开环指令，单位与 --limit 相同。 */
} ctrl_t;

static void print_help(void)
{
    ui_msg(
"  e               使能（目标先设为当前位置，原地保持，不会窜出去）\n"
"  d               失能（输出 0，同时退出开环模式）\n"
"  z               把当前位置设为零点\n"
"  s               目标设为当前位置（软停）\n"
"  <数字>          设定目标位置，单位=输出轴角度（如 90 或 -45.5）\n"
"  c <值>          开环输出测试，绕过 PID（c 0 退出）\n"
"  m <值>          输出限幅（单位同 --limit）\n"
"  kp/ki/kd <v>    位置环增益；skp/ski/skd <v> 速度环增益\n"
"  g               打印当前增益与限幅\n"
"  q / ?           退出 / 本帮助");
}

static void print_gains(const opts_t *o, const motor_cfg_t *cfg,
                        const pidc_t *pp, const pidc_t *ps)
{
    ui_msg("位置环 kp=%.3f ki=%.3f kd=%.3f   输出限幅 %.0f rpm\n"
           "速度环 kp=%.3f ki=%.3f kd=%.3f   (原始计数/rpm)\n"
           "输出限幅 %.3f %s = 原始计数 %.0f / %.0f    目标 %.2f°",
           pp->kp, pp->ki, pp->kd, pp->out_limit,
           ps->kp, ps->ki, ps->kd,
           o->limit, cfg->out_unit,
           o->limit / cfg->out_fs * cfg->out_raw_max, cfg->out_raw_max,
           o->target_deg);
}

static void handle_cmd(const char *line, opts_t *o, const motor_cfg_t *cfg,
                       pidc_t *pp, pidc_t *ps, motor_state_t *m, ctrl_t *c)
{
    char cmd[16];
    float v = 0.0f;
    char c0 = line[0];

    /* --- 纯数字 = 目标位置 --- */
    if ((c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+' || c0 == '.') {
        float t = atof(line);
        if (t >  MOTOR_TARGET_LIMIT_DEG) t =  MOTOR_TARGET_LIMIT_DEG;
        if (t < -MOTOR_TARGET_LIMIT_DEG) t = -MOTOR_TARGET_LIMIT_DEG;
        o->target_deg = t;
        ui_msg("[目标] %.2f°(输出轴) = 转子 %.1f°", t, t * o->ratio);
        return;
    }

    if (sscanf(line, "%15s %f", cmd, &v) < 1) return;

    if (!strcmp(cmd, "e")) {
        if (o->monitor) {
            ui_msg("[监视模式] 已硬禁止使能（去掉 --monitor 才能控制）");
            return;
        }
        c->enabled = 1;
        motor_pid_reset(pp); motor_pid_reset(ps);
        o->target_deg = motor_shaft_deg(m, o->ratio);   /* 关键安全动作 */
        ui_msg("[使能] 目标先设为当前位置 %.2f°，积分已清零", o->target_deg);
    } else if (!strcmp(cmd, "d")) {
        c->enabled = 0;
        c->open_loop = 0;
        ui_msg("[失能] 输出 0");
    } else if (!strcmp(cmd, "z")) {
        m->total_ecd = 0;
        o->target_deg = 0.0f;
        ui_msg("[零点] 当前位置已设为零点");
    } else if (!strcmp(cmd, "s")) {
        o->target_deg = motor_shaft_deg(m, o->ratio);
        ui_msg("[软停] 目标 = 当前位置 %.2f°", o->target_deg);
    } else if (!strcmp(cmd, "q")) {
        g_stop = 1;
    } else if (!strcmp(cmd, "?")) {
        print_help();
    } else if (!strcmp(cmd, "g")) {
        print_gains(o, cfg, pp, ps);
    } else if (!strcmp(cmd, "c")) {
        if (v >  o->limit) v =  o->limit;
        if (v < -o->limit) v = -o->limit;
        if (v == 0.0f) {
            c->open_loop = 0;
            ui_msg("[开环] 退出，回到位置闭环");
        } else if (!c->enabled) {
            ui_msg("[开环] 请先按 e 使能");
        } else {
            c->open_loop = 1;
            c->ol_out = v;
            ui_msg("[开环] 直接给 %.3f %s（绕过 PID）。c 0 退出", v, cfg->out_unit);
        }
    } else if (!strcmp(cmd, "m")) {
        if (v < 0.0f) v = 0.0f;
        if (v > cfg->out_fs) v = cfg->out_fs;
        o->limit = v;
        ui_msg("[限幅] 输出限幅 = %.3f %s", v, cfg->out_unit);
    } else if (!strcmp(cmd, "kp") || !strcmp(cmd, "ki") || !strcmp(cmd, "kd")) {
        float *t = (cmd[1] == 'p') ? &pp->kp : (cmd[1] == 'i') ? &pp->ki : &pp->kd;
        *t = v;
        ui_msg("[位置环] kp=%.3f ki=%.3f kd=%.3f", pp->kp, pp->ki, pp->kd);
    } else if (!strcmp(cmd, "skp") || !strcmp(cmd, "ski") || !strcmp(cmd, "skd")) {
        float *t = (cmd[2] == 'p') ? &ps->kp : (cmd[2] == 'i') ? &ps->ki : &ps->kd;
        *t = v;
        ui_msg("[速度环] kp=%.3f ki=%.3f kd=%.3f", ps->kp, ps->ki, ps->kd);
    } else {
        ui_msg("未知命令: %s（按 ? 看帮助）", cmd);
    }
}

/* 电流字段："实测/指令"。
 * 电流量纲明确（C620 / GM6020 电流模式）时显示 A，否则显示原始计数。
 * 并排显示是为了能直接判断驱动器的电流环有没有跟上 —— 这也是排查"电流没闭环"的手段。 */
static const char *cur_str(int16_t meas_raw, float cmd_raw,
                           const motor_cfg_t *cfg, char *buf, size_t n)
{
    if (cfg->cur_fs > 0.0f) {
        snprintf(buf, n, "%6.2f/%6.2fA",
                 meas_raw / cfg->cur_raw_max * cfg->cur_fs,
                 cmd_raw  / cfg->cur_raw_max * cfg->cur_fs);
    } else {
        snprintf(buf, n, "%6d/%6.0f ", meas_raw, cmd_raw);
    }
    return buf;
}

/* ===================== main ===================== */
int main(int argc, char *argv[])
{
    fieldbus_t fb;
    opts_t o = {
        .kind = MOTOR_M3508,
        .id = 1,
        .ch = ECAT2CAN_CAN_CHANNEL_DEFAULT,
        .ratio = MOTOR_M3508_RATIO_DEF,
        .ratio_given = 0,
        .target_deg = 0.0f,
        .limit = 3.0f,
        .limit_given = 0,
        .max_rpm = 2000.0f,
        .log_ms = 200,
        .invert_spd = 0,
        .monitor = 0,
        .csv = 0,
        .gm_current_mode = 1,   /* 默认电流模式，与 M3508 统一 */
        .filter_ms = 5.0f,
        .cur_loop = 0,
        .gain_given = 0,
        /* 增益留 0，确定电机类型后再按机型填默认值（两种电机差别很大） */
    };
    pidc_t pid_pos, pid_spd;
    motor_state_t m;
    motor_cfg_t   cfg;
    ctrl_t   ctrl = { .enabled = 0, .open_loop = 0, .ol_out = 0.0f };
    char     linebuf[80];
    size_t   linelen = 0;
    int      i, quiet_cycles;
    float    prev_err = 0.0f;
    int      diverging = 0;
    float    dt = 0.001f;        /* 固定控制周期：1 ms（1 kHz）。 */
    long     cycle = 0;
    struct timespec next, now;
    int      fb_lost_warned = 0;
    int      last_fb_seen = 0;
    char     tbuf[32];
    /* 反馈低通滤波、自激检测和可选主机侧电流环。 */
    float    a_rpm, a_d_coef;
    float    rpm_filt = 0.0f;
    float    rpm_used = 0.0f;
    /* 若当前协议提供安培量程，下面三个电流环变量均使用安培。 */
    float    cur_ref_a = 0.0f, cur_meas_a = 0.0f, cur_cmd_a = 0.0f;
    float    rpm_tgt_last = 0.0f;
    int      osc_flips = 0, osc_prev_sign = 0, osc_warned = 0;
    pidc_t   pid_cur;

    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    /* ---- 解析参数 ---- */
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--motor") && i + 1 < argc) {
            const char *k = argv[++i];
            if (!strcmp(k, "m3508") || !strcmp(k, "c620")) o.kind = MOTOR_M3508;
            else if (!strcmp(k, "gm6020")) o.kind = MOTOR_GM6020;
            else { printf("--motor 只支持 m3508 / gm6020\n"); return 1; }
        }
        else if (!strcmp(a, "--id")          && i + 1 < argc) o.id          = atoi(argv[++i]);
        else if (!strcmp(a, "--ch")          && i + 1 < argc) o.ch          = atoi(argv[++i]);
        else if (!strcmp(a, "--ratio")       && i + 1 < argc) { o.ratio = atof(argv[++i]); o.ratio_given = 1; }
        else if (!strcmp(a, "--target")      && i + 1 < argc) o.target_deg  = atof(argv[++i]);
        else if (!strcmp(a, "--limit")       && i + 1 < argc) { o.limit = atof(argv[++i]); o.limit_given = 1; }
        else if (!strcmp(a, "--max-rpm")     && i + 1 < argc) o.max_rpm     = atof(argv[++i]);
        else if (!strcmp(a, "--log-ms")      && i + 1 < argc) o.log_ms      = atoi(argv[++i]);
        else if (!strcmp(a, "--invert-spd"))                  o.invert_spd  = 1;
        else if (!strcmp(a, "--monitor"))                     o.monitor     = 1;
        else if (!strcmp(a, "--csv"))                         o.csv         = 1;
        else if (!strcmp(a, "--gm-current"))                  o.gm_current_mode = 1;
        else if (!strcmp(a, "--gm-voltage"))                  o.gm_current_mode = 0;
        else if (!strcmp(a, "--filter-ms")   && i + 1 < argc) o.filter_ms   = atof(argv[++i]);
        else if (!strcmp(a, "--cur-loop"))                    o.cur_loop     = 1;
        /* 注意：下面每一条都必须用 argv[++i] 取走参数值。
         * 早期版本这里用了个 fp 指针却不推进 i，atof() 拿到的是选项名本身，
         * 解析出来恒为 0 —— 增益选项实际上一直没生效。 */
        else if (!strcmp(a, "--pos-kp")      && i + 1 < argc) { o.pos_kp = atof(argv[++i]); o.gain_given |=  1u; }
        else if (!strcmp(a, "--pos-ki")      && i + 1 < argc) { o.pos_ki = atof(argv[++i]); o.gain_given |=  2u; }
        else if (!strcmp(a, "--pos-kd")      && i + 1 < argc) { o.pos_kd = atof(argv[++i]); o.gain_given |=  4u; }
        else if (!strcmp(a, "--spd-kp")      && i + 1 < argc) { o.spd_kp = atof(argv[++i]); o.gain_given |=  8u; }
        else if (!strcmp(a, "--spd-ki")      && i + 1 < argc) { o.spd_ki = atof(argv[++i]); o.gain_given |= 16u; }
        else if (!strcmp(a, "--spd-kd")      && i + 1 < argc) { o.spd_kd = atof(argv[++i]); o.gain_given |= 32u; }
        else if (!strcmp(a, "--cur-kp")      && i + 1 < argc) { o.cur_kp = atof(argv[++i]); o.gain_given |= 64u; }
        else if (!strcmp(a, "--cur-ki")      && i + 1 < argc) { o.cur_ki = atof(argv[++i]); o.gain_given |=128u; }
        else { printf("未知参数: %s\n", a); usage(argv[0]); return 1; }
    }

    /* ---- 按电机类型补默认值 / 校验 ---- */
    if (!o.ratio_given) o.ratio = (o.kind == MOTOR_M3508) ? MOTOR_M3508_RATIO_DEF : 1.0f;
    if (!o.limit_given) {
        if (o.kind == MOTOR_M3508)            o.limit = 3.0f;
        else if (o.gm_current_mode)           o.limit = 1.0f;   /* GM6020 电流满量程只有 3A */
        else                                  o.limit = 3000.0f;
    }
    /* 两种电机的机电特性差很多，默认增益分开给：
     *   M3508+C620  —— 有减速箱的话折算惯量大、转矩常数小，可以给猛一些
     *   GM6020      —— 直驱、转矩常数大(1.2N·m/3A)、惯量小，且链路有 3~4ms 延迟，
     *                  增益必须保守，否则高频自激（实测踩过） */
    if (o.kind == MOTOR_M3508) {
        if (!(o.gain_given &  1u)) o.pos_kp = 8.0f;
        if (!(o.gain_given &  4u)) o.pos_kd = 0.0f;   /* 微分默认关，见 motor_pid_step 注释 */
        if (!(o.gain_given &  8u)) o.spd_kp = 20.0f;
        if (!(o.gain_given & 16u)) o.spd_ki = 0.2f;
    } else {
        if (!(o.gain_given &  1u)) o.pos_kp = 4.0f;
        if (!(o.gain_given &  4u)) o.pos_kd = 0.0f;
        if (!(o.gain_given &  8u)) o.spd_kp = 4.0f;
        if (!(o.gain_given & 16u)) o.spd_ki = 0.05f;
    }
    if (!(o.gain_given & 64u)) o.cur_kp = 0.3f;
    if (!(o.gain_given &128u)) o.cur_ki = 0.02f;
    if (o.filter_ms < 0.0f) o.filter_ms = 0.0f;

    if (o.ch < 0 || o.ch > (int)ECAT2CAN_CAN_CHANNEL_MAX) {
        printf("--ch 必须是 0~%u（ECAN-Lite: CN4=0 CN5=1 CN9=2 CN1=3）\n",
               ECAT2CAN_CAN_CHANNEL_MAX);
        return 1;
    }
    if (o.ratio <= 0.01f) { printf("--ratio 必须 > 0\n"); return 1; }
    if (o.limit < 0.0f)   { printf("--limit 不能为负\n"); return 1; }
    if (o.target_deg > MOTOR_TARGET_LIMIT_DEG || o.target_deg < -MOTOR_TARGET_LIMIT_DEG) {
        printf("--target 超出 ±%.0f°，已限幅\n", MOTOR_TARGET_LIMIT_DEG);
        o.target_deg = (o.target_deg > 0) ? MOTOR_TARGET_LIMIT_DEG : -MOTOR_TARGET_LIMIT_DEG;
    }

    cfg.kind = o.kind;
    cfg.id = o.id;
    cfg.gm_current_mode = o.gm_current_mode;
    if (motor_cfg_init(&cfg) != 0) {
        printf("电机 ID 超出该型号支持范围\n");
        return 1;
    }
    if (o.limit > cfg.out_fs) {
        printf("--limit %.3f 超出 %s 满量程 %.0f，已限幅\n", o.limit, cfg.out_unit, cfg.out_fs);
        o.limit = cfg.out_fs;
    }

    a_d_coef = motor_lpf_alpha(o.filter_ms / 1000.0f, dt);
    a_rpm    = motor_lpf_alpha(o.filter_ms / 1000.0f, dt);

    pid_pos = (pidc_t){ .kp = o.pos_kp, .ki = o.pos_ki, .kd = o.pos_kd,
                        .i_limit = 200.0f, .out_limit = o.max_rpm, .a_d = a_d_coef };
    pid_spd = (pidc_t){ .kp = o.spd_kp, .ki = o.spd_ki, .kd = o.spd_kd,
                        .i_limit = cfg.out_raw_max * 0.5f, .out_limit = cfg.out_raw_max,
                        .a_d = a_d_coef };
    pid_cur = (pidc_t){ .kp = o.cur_kp, .ki = o.cur_ki, .kd = 0.0f,
                        .i_limit = cfg.cur_fs > 0.0f ? cfg.cur_fs : 1.0f,
                        .out_limit = cfg.cur_fs > 0.0f ? cfg.cur_fs : 1.0f,
                        .a_d = 1.0f };

    motor_state_reset(&m);

    /* ---- 打印配置 ---- */
    printf("=== RoboMaster 电机位置闭环 ===\n");
    printf("网卡       : %s\n", argv[1]);
    printf("电机       : %s\n", o.kind == MOTOR_M3508 ? "M3508 + C620" : "GM6020");
    printf("ID         : %d\n", o.id);
    printf("CAN 通道   : %d (MCAN%d)%s\n", o.ch, o.ch,
           (o.ch == 3) ? "   [hpm5e3ykit 单路版只有这一路]" : "");
    printf("控制报文   : 0x%03X  (第 %d 个槽位，共 %d 个有效槽位)\n",
           cfg.ctrl_id, cfg.ctrl_slot + 1, cfg.ctrl_slots);
    printf("反馈报文   : 0x%03X\n", cfg.fb_id);
    if (o.kind == MOTOR_GM6020) {
        printf("控制方式   : %s\n", o.gm_current_mode
               ? "电流模式 0x1FE/0x2FE（-16384~16384 ←→ -3~3A）"
               : "电压模式 0x1FF/0x2FF（-25000~25000）");
    }
    printf("减速比     : %.4f\n", o.ratio);
    printf("目标位置   : %.2f° (输出轴)\n", o.target_deg);
    printf("输出限幅   : %.3f %s（原始计数 %.0f / %.0f）\n",
           o.limit, cfg.out_unit, o.limit / cfg.out_fs * cfg.out_raw_max, cfg.out_raw_max);
    printf("转速限幅   : %.0f rpm (转子)\n", o.max_rpm);
    printf("运行模式   : %s\n", o.monitor ? "只读监视（禁止使能）" : "可控制");
    printf("位置环 PID : kp=%.3f ki=%.3f kd=%.3f\n", pid_pos.kp, pid_pos.ki, pid_pos.kd);
    printf("速度环 PID : kp=%.3f ki=%.3f kd=%.3f (原始计数/rpm)\n",
           pid_spd.kp, pid_spd.ki, pid_spd.kd);
    printf("\n");

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (fb_open(&fb, argv[1]) != 0) return 2;
    if (fb_to_op(&fb) != 0) { fb_close(&fb); return 3; }
    printf("从站已进 OP，DC 1ms 同步已开启\n");

    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);

    ui_init();

    if (o.monitor) {
        ui_msg("【只读监视模式】不会输出任何驱动量，按 e 也无效。\n"
               "   现在可以放心手动转动电机，观察下面的位置/转速/温度/错误码。");
    } else if (o.kind == MOTOR_M3508) {
        ui_msg("默认【失能】。确认电机已固定、周围无人后：\n"
               "     e       使能（目标先设为当前位置，原地保持）\n"
               "     c 3     开环给 3A 转矩电流，先感受力气和方向（c 0 退出）\n"
               "     10      目标转到输出轴 10°\n"
               "     m 10    把电流限幅放到 10A（额定）\n"
               "     ?       完整帮助");
    } else {
        if (o.gm_current_mode) {
            ui_msg("默认【失能】。确认电机已固定、周围无人后：\n"
                   "     e       使能（目标先设为当前位置，原地保持）\n"
                   "     c 1     开环给 1A 转矩电流，先感受方向和力度（c 0 退出）\n"
                   "     10      目标转到轴上 10°\n"
                   "     ?       完整帮助\n"
                   "   ⚠ 电流模式需要 GM6020 固件≥1.0.11.2 且已在 RoboMaster Assistant\n"
                   "     里打开\"电流环开关\"。若使能后电机毫无反应，加 --gm-voltage 试电压模式。");
        } else {
            ui_msg("默认【失能】。确认电机已固定、周围无人后：\n"
                   "     e         使能（目标先设为当前位置，原地保持）\n"
                   "     c 3000    开环给 3000 电压给定值，先感受方向和力度（c 0 退出）\n"
                   "     10        目标转到轴上 10°\n"
                   "     ?         完整帮助");
        }
    }
    ui_draw_prompt();

    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop) {
        uint8_t *out = fb_out(&fb);
        uint8_t *in  = fb_in(&fb);
        ecat2can_tx_rx_message *otx = (ecat2can_tx_rx_message *)out;
        ecat2can_tx_rx_message *irx = (ecat2can_tx_rx_message *)in;
        int wkc, n;
        float out_raw_f = 0.0f;                       /* PID/开环输出，原始计数 */
        float raw_lim   = motor_physical_to_raw(&cfg, o.limit, o.limit);

        /* ---------- 1. 解析上行 PDO 里的反馈 ---------- */
        m.lost_ms += 1;
        for (n = 0; n < (int)irx->frame_count && n < (int)ECAT2CAN_MAX_FRAME_COUNT; n++) {
            const can_frame_struct *f = &irx->can_frames[n];
            if (f->std_id == cfg.fb_id && f->dlc == 8) {
                motor_state_decode(&m, f->data);
            }
        }

        /* ---------- 2. 交互命令 ---------- */
        for (;;) {
            char buf[128];
            ssize_t k = read(STDIN_FILENO, buf, sizeof(buf));
            if (k <= 0) break;
            for (ssize_t j = 0; j < k; j++) {
                char ch = buf[j];
                if (ch == '\n' || ch == '\r') {
                    if (linelen > 0) {
                        linebuf[linelen] = '\0';
                        memcpy(g_ui.input, linebuf, linelen + 1);
                        g_ui.input_len = linelen;
                        handle_cmd(linebuf, &o, &cfg, &pid_pos, &pid_spd, &m, &ctrl);
                        linelen = 0;
                        g_ui.input_len = 0;
                        ui_draw_prompt();
                    } else {
                        ui_draw_prompt();
                    }
                } else if (ch == 127 || ch == 8) {              /* 退格 */
                    if (linelen > 0) linelen--;
                    g_ui.input_len = linelen;
                    ui_draw_prompt();
                } else if (linelen < sizeof(linebuf) - 1) {
                    linebuf[linelen++] = ch;
                    g_ui.input_len = linelen;
                    memcpy(g_ui.input, linebuf, linelen);
                    ui_draw_prompt();
                }
            }
        }

        /* ---------- 3. 安全联锁 ---------- */
        if (o.monitor) ctrl.enabled = 0;

        if (ctrl.enabled) {
            motor_fault_t fault = motor_state_fault(&m);
            if (fault == MOTOR_FAULT_FEEDBACK_TIMEOUT) {
                if (!fb_lost_warned) {
                    ui_msg("!! 超过 %d ms 收不到反馈，已切断输出。检查电机供电 / CAN 接线 / --id",
                           MOTOR_FB_TIMEOUT_MS);
                    fb_lost_warned = 1;
                }
                ctrl.enabled = 0;
            } else if (fault == MOTOR_FAULT_OVERTEMP) {
                ui_msg("!! 电机温度 %u℃ 超过 %d℃，已切断输出", m.temp, MOTOR_TEMP_LIMIT_C);
                ctrl.enabled = 0;
            } else if (fault == MOTOR_FAULT_DRIVER) {
                ui_msg("!! 驱动器报错误码 %u，已切断输出", m.err);
                ctrl.enabled = 0;
            }
        }
        if (!m.seen || m.lost_ms == 0) fb_lost_warned = 0;

        /* ---------- 4. 闭环 ---------- */
        cur_ref_a = 0.0f;
        if (ctrl.enabled && ctrl.open_loop) {
            out_raw_f = motor_physical_to_raw(&cfg, ctrl.ol_out, o.limit);
            cur_ref_a = ctrl.ol_out;
            motor_pid_reset(&pid_pos); motor_pid_reset(&pid_spd); motor_pid_reset(&pid_cur);
            rpm_filt = o.invert_spd ? -(float)m.rpm : (float)m.rpm;
            rpm_used = rpm_filt;
            diverging = 0; prev_err = 0.0f;
        } else if (ctrl.enabled) {
            float pos_deg  = motor_shaft_deg(&m, o.ratio);
            float rpm_raw  = o.invert_spd ? -(float)m.rpm : (float)m.rpm;
            float rpm_tgt;
            float err      = o.target_deg - pos_deg;

            /* 速度反馈低通。电机反馈是 1kHz、我们的 EtherCAT 周期也是 1kHz 但两者
             * 不同步，网关偶尔会读到重复的旧帧 —— 不滤波的话转速测量会在"新值/旧值"
             * 之间跳，这是抖动的第二个来源。 */
            rpm_filt += a_rpm * (rpm_raw - rpm_filt);
            rpm_used  = rpm_filt;

            rpm_tgt = motor_pid_step(&pid_pos, err, pos_deg, dt);
            rpm_tgt_last = rpm_tgt;

            /* 发散保护：误差连续变大说明某个环的符号反了，别让电机一直顶 */
            if (fabsf(err) > fabsf(prev_err) + 1e-4f) {
                if (++diverging > 500) {
                    ui_msg("!! 位置误差连续 0.5s 增大（%.2f° → %.2f°），疑似闭环发散，已切断输出。\n"
                           "   若确认是转速符号约定相反，加 --invert-spd 重试。", prev_err, err);
                    ctrl.enabled = 0;
                    diverging = 0;
                }
            } else {
                diverging = 0;
            }
            prev_err = err;

            out_raw_f = motor_pid_step(&pid_spd, rpm_tgt - rpm_used, rpm_used, dt);
            cur_ref_a = motor_raw_to_physical(&cfg, out_raw_f);
        } else {
            out_raw_f = 0.0f;
            rpm_tgt_last = 0.0f;
            diverging = 0;
            motor_pid_reset(&pid_pos);
            motor_pid_reset(&pid_spd);
            motor_pid_reset(&pid_cur);
            rpm_filt = o.invert_spd ? -(float)m.rpm : (float)m.rpm;
            rpm_used = rpm_filt;
        }

        /* ---- 可选：host 侧电流环 ----
         * 默认关闭。驱动器内部已经有一个几 kHz 带宽的电流环，而我们这条链路
         * 往返延迟 3~4ms，主机侧电流环的带宽上限只有约 1/(4*3ms) ≈ 80Hz ——
         * 用慢环套快环只会增加滞后、让系统更不稳。留这个开关是为了做对比实验。 */
        if (ctrl.enabled && o.cur_loop && cfg.cur_fs > 0.0f) {
            cur_meas_a = m.torque_raw / cfg.cur_raw_max * cfg.cur_fs;
            cur_cmd_a  = motor_pid_step(&pid_cur, cur_ref_a - cur_meas_a, cur_meas_a, dt);
            out_raw_f  = cur_cmd_a / cfg.out_fs * cfg.out_raw_max;
        }

        /* 统一按物理限幅再转回原始计数 */
        if (out_raw_f >  raw_lim) out_raw_f =  raw_lim;
        if (out_raw_f < -raw_lim) out_raw_f = -raw_lim;

        /* ---- 自激检测：0.2s 内输出频繁换向 = 增益相对延迟过大 ---- */
        {
            int sg = (out_raw_f > 1.0f) - (out_raw_f < -1.0f);
            if (sg != 0) {
                if (osc_prev_sign != 0 && sg != osc_prev_sign) osc_flips++;
                osc_prev_sign = sg;
            }
            if ((cycle % 200) == 0) {
                if (osc_flips > 30 && !osc_warned) {
                    ui_msg("!! 疑似自激振荡：输出在 0.2s 内换向 %d 次。\n"
                           "   建议先把速度环降下来（skp 减半），再降位置环 kp。\n"
                           "   本链路往返延迟约 3~4ms，增益不能按本地闭环来给。",
                           osc_flips);
                    osc_warned = 1;
                }
                osc_flips = 0;
            }
        }

        /* ---------- 5. 组装下行 PDO ---------- */
        memset(out, 0, ECAT2CAN_PDO_BYTES);
        otx->command     = WORK_MODE;
        otx->frame_count = 1;
        otx->seq         = (uint8_t)cycle;
        otx->can_frames[0].can_channel = (uint8_t)o.ch;
        otx->can_frames[0].std_id      = cfg.ctrl_id;
        otx->can_frames[0].dlc         = 8;
        for (n = 0; n < 4; n++) {
            int16_t v = (n == cfg.ctrl_slot) ? (int16_t)out_raw_f : 0;
            otx->can_frames[0].data[n * 2]     = (uint8_t)((v >> 8) & 0xFF);
            otx->can_frames[0].data[n * 2 + 1] = (uint8_t)(v & 0xFF);
        }

        /* ---------- 6. 收发 ---------- */
        wkc = fb_roundtrip(&fb);
        if (wkc < fb.expected_wkc && (cycle % 1000) == 0) {
            ui_msg("!! WKC=%d < %d，从站可能掉出 OP", wkc, fb.expected_wkc);
        }

        /* ---------- 7. 状态显示 ---------- */
        {
            int log_ms = (o.log_ms > 0) ? o.log_ms : 200;
            if ((cycle % log_ms) == 0) {
                const char *st = ctrl.enabled
                                 ? (ctrl.open_loop ? "开环" : "使能") : "失能";
                if (!o.csv && g_ui.enabled) {
                    /* 两行状态栏：单行排下来接近 100 列，80 列终端上会被截断 */
                    char l1[192], l2[192];
                    snprintf(l1, sizeof(l1),
                             "[%s] 位置 %8.2f° → %8.2f°    电流 %s",
                             st, motor_shaft_deg(&m, o.ratio), o.target_deg,
                             cur_str(m.torque_raw, out_raw_f, &cfg,
                                     tbuf, sizeof(tbuf)));
                    snprintf(l2, sizeof(l2),
                             "       转速 %5d [%5.0f] rpm    %3u℃  E%u  %3dus%s",
                             m.rpm, rpm_tgt_last, m.temp, m.err, fb.roundtrip_us,
                             m.seen ? "" : "  (等反馈)");
                    ui_set_status_lines(l1, l2);
                } else if (!o.csv) {
                    printf("[%s] 位置 %8.2f°→%8.2f°  转速 %5d[%5.0f]rpm  电流 %s  "
                           "%3u℃ E%u  %3dus%s\n",
                           st, motor_shaft_deg(&m, o.ratio), o.target_deg,
                           m.rpm, rpm_tgt_last,
                           cur_str(m.torque_raw, out_raw_f, &cfg,
                                   tbuf, sizeof(tbuf)),
                           m.temp, m.err, fb.roundtrip_us,
                           m.seen ? "" : " (等反馈)");
                    fflush(stdout);
                }
                if (o.csv) {
                    printf("MOTOR_CSV,%.3f,%.4f,%.4f,%d,%.2f,%d,%.2f,%u,%d\n",
                           cycle * dt, motor_shaft_deg(&m, o.ratio), o.target_deg,
                           m.rpm, rpm_tgt_last, m.torque_raw, out_raw_f,
                           m.temp, ctrl.enabled);
                    fflush(stdout);
                }
            }
        }
        if (m.seen != last_fb_seen) {
            if (m.seen && !last_fb_seen) {
                ui_msg("已收到 0x%03X 的反馈：温度 %u℃ 错误码 %u  —— 链路正常",
                       cfg.fb_id, m.temp, m.err);
            }
            last_fb_seen = m.seen;
        }

        /* ---------- 8. 严格 1ms 节拍 ---------- */
        cycle++;
        next.tv_nsec += (long)(dt * 1e9f);
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1) next = now;   /* 落后太多就重新对齐 */
    }

    /* ---------- 退出：先发一段零输出，让驱动器确认停止 ---------- */
    ui_restore();
    printf("退出中：发送零输出 ...\n");
    {
        uint8_t *out = fb_out(&fb);
        ecat2can_tx_rx_message *otx = (ecat2can_tx_rx_message *)out;
        for (quiet_cycles = 0; quiet_cycles < 100; quiet_cycles++) {
            memset(out, 0, ECAT2CAN_PDO_BYTES);
            otx->command     = WORK_MODE;
            otx->frame_count = 1;
            otx->seq         = (uint8_t)quiet_cycles;
            otx->can_frames[0].can_channel = (uint8_t)o.ch;
            otx->can_frames[0].std_id      = cfg.ctrl_id;
            otx->can_frames[0].dlc         = 8;
            memset(otx->can_frames[0].data, 0, 8);
            fb_roundtrip(&fb);
        }
    }
    printf("已发送 100 个周期的零输出\n");

    fb_close(&fb);
    printf("已请求 INIT 状态并关闭网卡\n");
    return 0;
}
