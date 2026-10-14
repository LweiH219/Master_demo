/* CPU_ZERO / CPU_SETSIZE / sched_getaffinity 需要 _GNU_SOURCE，
 * 必须放在**所有** include 之前。 */
#define _GNU_SOURCE

/*
 * ecat2can_master.c —— Linux 主机侧 SOEM 测试主站
 * ===========================================================================
 * 用途：在没有 TwinCAT 的 Linux 上验证「EtherCAT -> 单路 CAN」从站模块，
 *       讲的是港科大 ECAT2CAN 的 192 字节 PDO 协议，逐字段对应。
 *
 * 从站侧对应文件：
 *     ecat2can/application/ecat2can.h     （结构与命令定义）
 *     ecat2can/application/ecat2can.c     （命令处理逻辑）
 *     ecat2can/SSC/ESI/ECAT_IO.xml        （PDO 映射：SM2=192B @0x1100 下行，
 *                                          SM3=192B @0x1400 上行）
 *
 * 依赖：
 *     SOEM 2.0   https://github.com/OpenEtherCATsociety/SOEM
 *     （本仓库已带：~/HPM/tools/SOEM，构建产物 build/libsoem.a）
 *
 * 用法：
 *     ecat2can_master <网卡名> <命令> [参数...]
 *
 *     info                 进 OP 后打印从站信息，不做数据交换
 *     loop [次数]          LOOP_TEST：192 字节往返一致性校验（默认 1000 次）
 *     work [id] [b0..b7]   WORK_MODE：按 1 kHz 周期发一帧 CAN，打印反馈帧
 *     fdb  [次数]          ONLY_FDB：只读 CAN 总线反馈，不发
 *     reset                HW_RESET：让从站看门狗复位
 *
 * 示例：
 *     ./ecat2can_master enp3s0 loop
 *     ./ecat2can_master enp3s0 work 0x123 11 22 33 44 55 66 77 88
 *
 * 注意：
 *   1. 需要 CAP_NET_RAW/CAP_NET_ADMIN：
 *          sudo setcap cap_net_raw,cap_net_admin+eip ./ecat2can_master
 *      或直接 sudo 运行。
 *   2. 建议先把网卡从 NetworkManager 摘出来，免得它插话：
 *          sudo nmcli dev set <网卡名> managed no
 *   3. 本板只引出一路 CAN，对应协议里的 can_channel = 3（见从站 bsp_can.h 的
 *      ECAT2CAN_CAN_CHANNEL）。填别的通道会被从站丢弃并置 BAD_CHANNEL 错误位。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sched.h>

#include "soem/soem.h"

#include "ecat2can_proto.h"
/* ===================== 运行环境 ===================== */
typedef struct {
    ecx_contextt ctx;
    const char  *iface;
    uint8        map[4096];
    int          roundtrip_us;
    int          slavecount;   /* 扫到的从站数 */
    int          slave_idx;    /* 当前操作的从站（1..slavecount） */
} fieldbus_t;

/*
 * 多从站：SOEM 把整个组的过程映像**按从站顺序紧凑排列** ——
 *   输出区：[从站1 的 PDO][从站2 的 PDO]...
 *   输入区：[从站1 的 PDO][从站2 的 PDO]...
 * 所以第 k 个从站的 PDO 起点 = 组首地址 + (k-1) × 192。
 *
 * 用环境变量 ECAT2CAN_SLAVE=N 选从站，默认 1。
 */
#define FB_OUT(fb, grp) ((ecat2can_tx_rx_message *) \
    ((uint8_t *)(grp)->outputs + (size_t)((fb)->slave_idx - 1) * ECAT2CAN_PDO_BYTES))
#define FB_IN(fb, grp)  ((const ecat2can_tx_rx_message *) \
    ((const uint8_t *)(grp)->inputs + (size_t)((fb)->slave_idx - 1) * ECAT2CAN_PDO_BYTES))
/* 按【从站号】取该从站的 192 字节 PDO 区域（多从站同时操作时用）*/
#define FB_OUT_N(fb, grp, n) ((ecat2can_tx_rx_message *) \
    ((uint8_t *)(grp)->outputs + (size_t)((n) - 1) * ECAT2CAN_PDO_BYTES))
#define FB_IN_N(fb, grp, n)  ((const ecat2can_tx_rx_message *) \
    ((const uint8_t *)(grp)->inputs + (size_t)((n) - 1) * ECAT2CAN_PDO_BYTES))

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ===================== 工具函数 ===================== */
static int fb_roundtrip(fieldbus_t *fb)
{
    ec_timet start, end, diff;
    int wkc;

    start = osal_current_time();
    ecx_send_processdata(&fb->ctx);
    wkc = ecx_receive_processdata(&fb->ctx, EC_TIMEOUTRET);
    end = osal_current_time();
    osal_time_diff(&start, &end, &diff);
    fb->roundtrip_us = (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000);

    return wkc;
}

static void dump_frame(const char *tag, const can_frame_struct *f)
{
    int i;
    printf("%s ch=%u id=0x%03X dlc=%u data=",
           tag, f->can_channel, (unsigned)f->std_id, (unsigned)f->dlc);
    if (f->dlc == 0) {
        printf("(空)");
    } else {
        for (i = 0; i < (int)f->dlc && i < 8; i++) {
            printf("%02X ", f->data[i]);
        }
    }
}


/* 打印输入 PDO 的解析结果（从站 -> 主站 的 192 字节） */
static void dump_input_pdo(const uint8_t *in)
{
    const ecat2can_tx_rx_message *m = (const ecat2can_tx_rx_message *)in;
    int i;

    char ebuf[96];
    printf("  [上行] command=0x%02X seq=%u status=%u frame_count=%u error_flags=0x%02X(%s)\n",
           m->command, m->seq, m->status, m->frame_count, m->error_flags,
           ecat2can_err_str(m->error_flags, ebuf, sizeof(ebuf)));

    for (i = 0; i < (int)m->frame_count && i < (int)ECAT2CAN_MAX_FRAME_COUNT; i++) {
        printf("    ");
        dump_frame("CAN反馈", &m->can_frames[i]);
        printf("\n");
    }
}

/* ===================== 从站上线 ===================== */
static int fb_start(fieldbus_t *fb)
{
    ecx_contextt *ctx = &fb->ctx;
    ec_groupt *grp;
    ec_slavet *slave;
    int i;
    const char *iface = fb->iface;   /* memset 会把它清掉，先存下来 */

    memset(fb, 0, sizeof(*fb));
    fb->iface = iface;

    if (!ecx_init(ctx, fb->iface)) {
        printf("!! 打不开网卡 %s（需要 CAP_NET_RAW，或是网卡名写错了）\n", fb->iface);
        printf("   修: sudo setcap cap_net_raw,cap_net_admin+eip ./ecat2can_master\n");
        return 0;
    }
    printf("网卡 %s 初始化完成\n", fb->iface);

    /*
     * ★ 扫描重试。
     *
     * EtherCAT 的 DL 端口状态里，「有链路」和「通信已建立」是两回事：
     *   bits = 0b01  端口打开，但没有帧通过
     *   bits = 0b10  端口打开且通信已建立   ← 枚举需要这个
     *
     * 下游从站上电/复位/首次进 INIT 的过程中，它的 ESC 端口会先到 0b01，
     * 过一会儿才到 0b10。一次性扫描很容易正好落在窗口外，只扫到前面的从站。
     * 实测遇到过多从站时 8 次扫描只成功 1 次的情况。
     *
     * 所以这里重试若干次，取「找到从站最多」的那一次结果。
     * 次数可用 ECAT2CAN_SCAN_RETRY 覆盖（默认 6，每次间隔 400 ms）。
     */
    {
        int attempt, best = 0, best_attempt = 0;
        int retry = 6;
        const char *e = getenv("ECAT2CAN_SCAN_RETRY");
        if (e != NULL) {
            int v = atoi(e);
            if (v >= 1 && v <= 50) retry = v;
        }

        for (attempt = 1; attempt <= retry; attempt++) {
            int n = ecx_config_init(ctx);
            if (n > best) { best = n; best_attempt = attempt; }
            if (n > 0 && attempt == 1 && retry == 1) break;
            if (best > 1) break;              /* 已经找到多个，不必再试 */
            if (attempt < retry) osal_usleep(400000);
        }
        if (best_attempt > 1) {
            printf("扫描重试：第 %d 次才扫到 %d 个从站（共试 %d 次）\n",
                   best_attempt, best, retry);
        }
        ctx->slavecount = best;
        if (best <= 0) {
            printf("!! 重试 %d 次都没扫到从站。\n", retry);
        }
    }

    if (ctx->slavecount <= 0) {
        printf("!! 没扫到从站。依次检查：\n"
               "   1) 网线插在板子的 EtherCAT 口上，且是 Port0 那个（本板 Port1 不支持）\n"
               "   2) 板子已上电且固件在跑（串口应打印 EEPROM loading successful）\n"
               "   3) 网卡有链路：ip -br link show %s 应显示 LOWER_UP\n",
               fb->iface);
        return 0;
    }
    fb->slavecount = ctx->slavecount;
    printf("扫到 %d 个从站\n", ctx->slavecount);
    for (i = 1; i <= ctx->slavecount; i++) {
        static const char *topo_name[] = { "无", "线端(1口)", "直通(2口)", "分支(3口)", "交叉(4口)" };
        uint8 ap;
        slave = ctx->slavelist + i;

        /* activeports：ESC 里「链路已建立且通信正常」的端口位图
         *   bit0=port0  bit1=port1  bit2=port2  bit3=port3
         * topology：活跃端口数（1=线端 2=直通 3=分支 4=交叉）
         * entryport：主站从哪个口进来 */
        ap = slave->activeports;
        printf("  [%d] %-14s Man=0x%08X ID=0x%08X Rev=0x%08X\n",
               i, slave->name, slave->eep_man, slave->eep_id, slave->eep_rev);
        printf("      端口: 活跃=0x%X", ap);
        if (ap) {
            int k;
            printf(" (");
            for (k = 0; k < 4; k++) {
                if (ap & (1u << k)) printf("%sport%d", (k && (ap & ((1u << k) - 1))) ? " " : "", k);
            }
            printf(")");
        }
        printf("  拓扑=%s  入口=port%d\n",
               topo_name[slave->topology <= 4 ? slave->topology : 0], slave->entryport);
    }

    /*
     * ★ 单从站时的「下游有链路却没枚举到」检测。
     *
     * activeports 是 ESC 自己报告的链路状态，**与主站枚举无关**。
     * 所以：只扫到 1 个从站，但它的端口位图显示有 2 个活跃口 ——
     * 说明下游物理上确实接了设备、链路也建立了，但对方**没有响应 EtherCAT 枚举**。
     * 这是排查多从站问题时最关键的一条线索。
     */
    if (ctx->slavecount == 1 && ctx->slavelist[1].topology > 1) {
        printf("\n"
               "⚠️  下游链路存在，但从站没有被枚举到：\n"
               "     从站1 报告 %d 个活跃端口，说明它的另一个口上确实接了设备且链路已建立，\n"
               "     但主站扫不到它 —— 对方没有响应 EtherCAT 寻址。\n"
               "\n"
               "   常见原因（按可能性）：\n"
               "     1) 下游从站的**固件没在跑** —— ESC 上电后端口未被 PDI 使能，\n"
               "        帧进了它的 PHY 却不出 ESC。PHY 灯会亮/闪，但协议层是哑的。\n"
               "     2) 下游从站的 EEPROM 没加载/内容不对 —— 厂家号产品号读不出来。\n"
               "     3) 下游从站供电不足，MCU 没启动。\n"
               "     4) 网线只通了接收对，没通发送对（半通）。\n"
               "\n"
               "   验证：给下游从站接串口，看有没有打印 \"EEPROM loading successful\"。\n",
               ctx->slavelist[1].topology);
    }

    ecx_config_map_group(ctx, fb->map, 0);
    grp = ctx->grouplist + 0;
    printf("过程数据映射：下行 %d 字节 / 上行 %d 字节（%d 从站 × %d）\n",
           grp->Obytes, grp->Ibytes, ctx->slavecount, ECAT2CAN_PDO_BYTES);

    /*
     * 每个从站的 PDO 应当是 192 字节；整组因此是 N×192。
     * ★ 早先这里写死 192，接两块板时会误报"从站固件不配套" —— 其实是槽位没算对。
     */
    {
    int want = (int)(ECAT2CAN_PDO_BYTES * (unsigned)ctx->slavecount);
    if ((int)grp->Obytes != want || (int)grp->Ibytes != want) {
        printf("!! 期望 %d/%d（%d 从站 × 192），实际 %d/%d。\n",
               ECAT2CAN_PDO_BYTES * ctx->slavecount,
               ECAT2CAN_PDO_BYTES * ctx->slavecount,
               ctx->slavecount, grp->Obytes, grp->Ibytes);
        if (grp->Obytes % ECAT2CAN_PDO_BYTES != 0) {
            printf("   总字节数不是 192 的整数倍 → 从站 PDO 尺寸和本工具不匹配，\n"
                   "   请核对从站固件是否为 ECAT2CAN 协议。\n");
        } else {
            printf("   是 192 的整数倍但个数对不上 → 有从站的 PDO 尺寸不同。\n");
        }
        return 0;
    }
    }

    /* 选从站：环境变量 ECAT2CAN_SLAVE=N，默认第 1 个 */
    fb->slave_idx = 1;
    {
        const char *e = getenv("ECAT2CAN_SLAVE");
        if (e != NULL) {
            int v = atoi(e);
            if (v >= 1 && v <= ctx->slavecount) {
                fb->slave_idx = v;
            } else {
                printf("!! ECAT2CAN_SLAVE=%s 超范围（1..%d），仍用 1\n", e, ctx->slavecount);
            }
        }
    }
    printf("本次操作从站：%d%s（换从站用 ECAT2CAN_SLAVE=N）\n",
           fb->slave_idx,
           (ctx->slavecount == 1) ? "  —— 只有一个从站" : "");

    slave = ctx->slavelist + 1;

    ecx_configdc(ctx);

    printf("等待 SAFE-OP ... ");
    ecx_statecheck(ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
    printf("完成（当前 0x%02X）\n", slave->state);

    /* 先跑一拍，让从站拿到一次有效输出 */
    fb_roundtrip(fb);

    printf("切 OPERATIONAL ... ");
    slave = ctx->slavelist;          /* 0 号是广播用的虚拟从站 */
    slave->state = EC_STATE_OPERATIONAL;
    ecx_writestate(ctx, 0);

    for (i = 0; i < 20 && !g_stop; i++) {
        fb_roundtrip(fb);
        ecx_statecheck(ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 20);
        if (slave->state == EC_STATE_OPERATIONAL) {
            printf("完成，从站已进 OP\n");
            /* 1 ms 分布式时钟同步，和港科大实装时的 1 kHz 控制周期一致。
             * ★ 多从站时每个都要设，否则未设的从站会各自自由跑。 */
            for (i = 1; i <= ctx->slavecount; i++) {
                ecx_dcsync0(ctx, i, TRUE, 1000000, 0);
            }
            if (ctx->slavecount > 1) {
                printf("  已对 %d 个从站都下发 DC 同步（1 ms）\n", ctx->slavecount);
            }
            return 1;
        }
    }

    printf("失败\n");
    ecx_readstate(ctx);
    for (i = 1; i <= ctx->slavecount; i++) {
        slave = ctx->slavelist + i;
        printf("  从站 %d 状态 0x%04X AL-status=0x%04X %s\n", i, slave->state,
               slave->ALstatuscode, ec_ALstatuscode2string(slave->ALstatuscode));
    }
    return 0;
}

static void fb_stop(fieldbus_t *fb)
{
    ecx_contextt *ctx = &fb->ctx;

    for (int i = 1; i <= ctx->slavecount; i++) {
        ecx_dcsync0(ctx, i, FALSE, 0, 0);
    }
    ctx->slavelist->state = EC_STATE_INIT;
    ecx_writestate(ctx, 0);
    ecx_close(ctx);
    printf("已请求 INIT 状态并关闭网卡\n");
}

/* ===================== 各命令 ===================== */
/*
 * 读**枚举到的从站**的 ESC 寄存器。
 *
 * 为什么要这个命令：ESC 的 DL_STAT / STATION_ADDR 是芯片自己维护的，
 * 与主站软件无关。把这里的读数和用调试器直接从板子上读到的对比，
 * 就能判断「主站连的到底是哪一块板」以及「每个端口的链路到底通没通」。
 *
 * STATION_ADDR 由主站在枚举时分配 —— 没被枚举到的板子，这个值恒为 0。
 */
static int cmd_regs(fieldbus_t *fb)
{
    ecx_contextt *ctx = &fb->ctx;
    ec_slavet *sl = &ctx->slavelist[fb->slave_idx];
    uint16 adp = sl->configadr;
    uint16 station, dlstat, alstat, alcode;
    uint16 pdesc16;
    uint8  pdesc;
    int k;

    printf("从站 %d 的 ESC 寄存器（configadr=0x%04X）\n", fb->slave_idx, adp);

    station = ecx_FPRDw(&ctx->port, adp, 0x0010, EC_TIMEOUTRET);
    pdesc16 = ecx_FPRDw(&ctx->port, adp, 0x0007, EC_TIMEOUTRET);
    pdesc   = (uint8)pdesc16;
    dlstat  = ecx_FPRDw(&ctx->port, adp, 0x0110, EC_TIMEOUTRET);
    alstat  = ecx_FPRDw(&ctx->port, adp, 0x0130, EC_TIMEOUTRET);
    alcode  = ecx_FPRDw(&ctx->port, adp, 0x0134, EC_TIMEOUTRET);

    printf("  STATION_ADDR = 0x%04X%s\n", station,
           (station == 0) ? "   ← 0 = 主站没分配过地址" : "   ← 主站枚举时分配的");
    printf("  PORT_DESC    = 0x%02X\n", pdesc);
    printf("  DL_STAT      = 0x%04X\n", dlstat);
    for (k = 0; k < 4; k++) {
        unsigned v = (dlstat >> (8 + 2 * k)) & 3u;
        printf("    port%d: bits[%d:%d] = 0b%02u  %s\n", k, 9 + 2 * k, 8 + 2 * k, v,
               (v == 2) ? "✅ 链路已建立且通信正常" :
               (v == 1) ? "❌ 未建立" : "   (无此端口/关闭)");
    }
    printf("  AL_STAT      = 0x%04X\n", alstat);
    printf("  AL_STAT_CODE = 0x%04X  %s\n", alcode, ec_ALstatuscode2string(alcode));
    printf("\n");
    printf("  对照用法：接上调试器直接读同一块板子的 0xF1700110(低16位) / 0xF1700010，\n"
           "  两处读数一致 → 主站连的就是这块板。\n");
    return 0;
}

static int cmd_info(fieldbus_t *fb)
{
    int disp_tick = 0;
    int tick = 0;

    while (!g_stop) {
        ec_groupt *grp = fb->ctx.grouplist + 0;
        int expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;
        int wkc = fb_roundtrip(fb);

        /* slavelist[1].state 只有 readstate/statecheck 才会刷新，定期读一次 */
        if (++tick % 20 == 0) {
            ecx_readstate(&fb->ctx);
        }
        /*
         * ★ 过程数据必须以 ~1 kHz 持续下发，不能几十毫秒才发一次。
         *
         * EtherCAT 从站的 SM 看门狗默认 100 ms：主站超过这个时间没写同步管理器，
         * 从站就会掉出 OP、报 AL=0x001B（Sync manager watchdog），
         * 表现为 WKC 由 3/3 掉到 1/3（SAFE-OP 下从站只读输入、不处理输出）。
         *
         * 早先这里是 usleep(100000) —— 刚好撞上看门狗超时，于是把
         * 「正常工作的从站」误判成了故障。数据要发得快，显示可以慢。
         */
        if (++disp_tick % 100 == 0) {
            ec_slavet *sl = &fb->ctx.slavelist[fb->slave_idx];
            printf("往返 %3d us  WKC %d/%d  状态 0x%02X%s  AL=0x%04X %-28s\r",
                   fb->roundtrip_us, wkc, expected_wkc,
                   sl->state,
                   (sl->state == EC_STATE_OPERATIONAL) ? " (OP)" : "",
                   sl->ALstatuscode,
                   ec_ALstatuscode2string(sl->ALstatuscode));
            fflush(stdout);
        }
        osal_usleep(1000);
    }
    printf("\n");
    return 0;
}

/* LOOP_TEST：从站把下行 192 字节原样搬到上行，用来验证 EtherCAT 链路本身
 *
 * 注意时序：EtherCAT 一帧里 ESC 是「先把 SM3 的上行数据塞进帧，再把帧里的
 * 下行数据写进 SM2」。所以主站在第 N 轮读到的输入，对应的是第 N-1 轮写下去
 * 的输出 —— 有且只有 1 拍延迟。因此这里拿"上一轮的下行"和"本轮的上行"比对。
 */
static int cmd_loop(fieldbus_t *fb, int cycles)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    uint8_t prev[ECAT2CAN_PDO_BYTES];
    int i, round, bad = 0, compared = 0;
    int expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;

    printf("LOOP_TEST：往从站写 192 字节，下一拍读回来逐字节比对（%d 次）\n",
           cycles);
    memset(prev, 0, sizeof(prev));

    for (round = 0; round < cycles && !g_stop; round++) {
        uint8_t *out = (uint8_t *)FB_OUT(fb, grp);
        const uint8_t *in = (const uint8_t *)FB_IN(fb, grp);
        ecat2can_tx_rx_message *m = (ecat2can_tx_rx_message *)out;
        int wkc;

        /* 造一个每轮都不同的确定性图案 */
        for (i = 0; i < (int)ECAT2CAN_PDO_BYTES; i++) {
            out[i] = (uint8_t)(i * 7 + round * 31 + 0x5A);
        }
        m->command = LOOP_TEST;
        m->frame_count = 0;
        m->seq = (uint8_t)round;
        m->status = 0;
        m->error_flags = 0;

        wkc = fb_roundtrip(fb);
        if (wkc < expected_wkc) {
            printf("  第 %d 轮 WKC=%d（期望 %d），从站可能掉出 OP\n",
                   round, wkc, expected_wkc);
            bad++;
            continue;
        }

        /*
         * 本轮的上行应该等于上一轮的下行（第 0 轮没有上一轮，跳过）。
         *
         * ★ 但要跳过 5 字节协议头里【从站自己拥有】的字段：
         *     offset 2 = seq          从站自己的发送序号，逐帧递增
         *     offset 3 = status       从站反馈状态
         *     offset 4 = error_flags  从站反馈错误标志
         *   command / frame_count 是主站下发的，从站原样保留 → 要参与比对。
         *
         * 早先这里 memcmp 整个 192 字节，把从站的 seq 和主站下发的 seq 对比，
         * 两者本来就不同源，于是偶发差 1 就误报「不一致」——
         * 实测每 150 轮左右误报一次。
         */
        if (round > 0) {
            int diff = 0, first = -1;
            compared++;
            for (i = 0; i < (int)ECAT2CAN_PDO_BYTES; i++) {
                if (i == 2 || i == 3 || i == 4) continue;   /* 从站拥有的字段 */
                if (prev[i] != in[i]) { diff = 1; first = i; break; }
            }
            if (diff) {
                printf("  第 %d 轮不一致：首个不同字节 offset=%d 上轮下行=0x%02X 本轮上行=0x%02X\n",
                       round, first, prev[first], in[first]);
                bad++;
            }
        }

        memcpy(prev, out, ECAT2CAN_PDO_BYTES);

        if (round < 3 || (round + 1) % 200 == 0) {
            printf("  第 %4d 轮 OK  往返 %d us  WKC %d\n", round,
                   fb->roundtrip_us, wkc);
        }
        osal_usleep(1000);
    }

    if (bad == 0) {
        printf("LOOP_TEST 全部通过：EtherCAT 链路与 192 字节 PDO 搬运正常（比对 %d 次）\n",
               compared);
        return 0;
    }
    printf("LOOP_TEST 有 %d 次失败（共比对 %d 次）\n", bad, compared);
    return 1;
}

/* 默认发送通道（命令行未显式指定时使用）。
 * 四路板子（ECAN-Lite / 港科广 5e3y）通道号 0~3 全可用；
 * 单路板子（hpm5e3ykit）只有 3。见 ecat2can_proto.h。 */
static uint8_t g_can_channel = ECAT2CAN_CAN_CHANNEL_DEFAULT;

/* 组装一帧标准 CAN 帧到下行 PDO 的第 index 个槽位 */
static void build_frame(ecat2can_tx_rx_message *m, int index, uint8_t ch,
                        uint32_t id, const uint8_t *data, int len)
{
    can_frame_struct *f = &m->can_frames[index];
    int i;

    memset(f, 0, sizeof(*f));
    f->can_channel = ch;
    f->std_id = id & ECAT2CAN_STD_ID_MAX;
    f->dlc = (uint16_t)len;
    for (i = 0; i < len && i < 8; i++) {
        f->data[i] = data[i];
    }
}

/* 在 nch 个通道上各发一帧（同一拍 PDO 里），nch=1 就是普通单通道发送。
 * chs[] 给出要用的通道号。 */
static int work_channels(fieldbus_t *fb, const uint8_t *chs, int nch,
                         uint32_t id, const uint8_t *data, int len, int cycles)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    const ecat2can_tx_rx_message *in = FB_IN(fb, grp);
    int i, k, round, last_count = -1;

    printf("WORK_MODE：周期 1 kHz 下发 CAN 帧（%d 次）\n", cycles);
    printf("  通道 = ");
    for (k = 0; k < nch; k++) printf("%s%u", k ? ", " : "", chs[k]);
    printf("\n  ID   = 0x%03X  DLC = %d  data =", id, len);
    for (i = 0; i < len && i < 8; i++) printf(" %02X", data[i]);
    printf("\n");

    printf("提示：从站每收到一帧 EtherCAT 输出就把这帧发到 CAN 总线上，\n"
           "      总线上的反馈被收集进上行 PDO；总线空闲时 frame_count = 0。\n"
           "      ⚠ 某一路总线上只有本模块一个节点时，该路 CAN 帧发出去收不到 ACK，\n"
           "        MCAN 会不断重发直到进入 bus-off（固件已带自动恢复，但那一拍会丢帧）。\n");

    for (round = 0; round < cycles && !g_stop; round++) {
        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = WORK_MODE;
        out->frame_count = (uint8_t)nch;
        out->seq = (uint8_t)round;
        for (k = 0; k < nch; k++) {
            build_frame(out, k, chs[k], id, data, len);
        }

        fb_roundtrip(fb);

        if (round < 5 || in->frame_count != last_count ||
            in->status != ECAT2CAN_STATUS_OK || in->error_flags != 0) {
            printf("第 %4d 轮  往返 %d us\n", round, fb->roundtrip_us);
            dump_input_pdo((const uint8_t *)in);
            last_count = in->frame_count;
        }
        osal_usleep(1000);
    }
    return 0;
}

static int cmd_work(fieldbus_t *fb, uint8_t ch, uint32_t id,
                    const uint8_t *data, int len, int cycles)
{
    return work_channels(fb, &ch, 1, id, data, len, cycles);
}

/* 在 4 个通道上各发一帧 —— 用来找"设备到底挂在哪一路"，
 * 也是四路并发控制（每路一帧）的最小示例。 */
static int cmd_workall(fieldbus_t *fb, uint32_t id, const uint8_t *data,
                       int len, int cycles)
{
    uint8_t chs[ECAT2CAN_CAN_CHANNEL_COUNT];
    unsigned k;

    for (k = 0; k < ECAT2CAN_CAN_CHANNEL_COUNT; k++) {
        chs[k] = (uint8_t)k;
    }
    return work_channels(fb, chs, (int)ECAT2CAN_CAN_CHANNEL_COUNT,
                         id, data, len, cycles);
}

/* ===================== scan：多通道流量扫描 ===================== */
#define SCAN_MAX_IDS 12

static int cmd_scan(fieldbus_t *fb, int cycles)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    const ecat2can_tx_rx_message *in = FB_IN(fb, grp);
    uint32_t cnt[ECAT2CAN_CAN_CHANNEL_COUNT];
    uint16_t ids[ECAT2CAN_CAN_CHANNEL_COUNT][SCAN_MAX_IDS];
    int nids[ECAT2CAN_CAN_CHANNEL_COUNT];
    int round, k, n;

    memset(cnt, 0, sizeof(cnt));
    memset(nids, 0, sizeof(nids));

    printf("SCAN：只看不发，统计 %d 拍内各通道的 CAN 流量\n", cycles);
    printf("      （从站四路全开，插在哪个座子上就统计到哪个通道）\n\n");

    for (round = 0; round < cycles && !g_stop; round++) {
        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = ONLY_FDB;
        out->seq = (uint8_t)round;

        fb_roundtrip(fb);

        n = (int)in->frame_count;
        if (n > (int)ECAT2CAN_MAX_FRAME_COUNT) n = (int)ECAT2CAN_MAX_FRAME_COUNT;
        for (k = 0; k < n; k++) {
            const can_frame_struct *f = &in->can_frames[k];
            unsigned ch = f->can_channel;
            int j, found = 0;

            if (ch >= ECAT2CAN_CAN_CHANNEL_COUNT) continue;
            cnt[ch]++;
            for (j = 0; j < nids[ch]; j++) {
                if (ids[ch][j] == f->std_id) { found = 1; break; }
            }
            if (!found && nids[ch] < SCAN_MAX_IDS) {
                ids[ch][nids[ch]++] = (uint16_t)f->std_id;
            }
        }
        osal_usleep(1000);
    }

    printf("  ┌────────────┬──────────┬─────────────────────────────────┐\n");
    printf("  │ 通道       │ 收到帧数 │ 出现过的 ID                     │\n");
    printf("  ├────────────┼──────────┼─────────────────────────────────┤\n");
    for (k = 0; k < (int)ECAT2CAN_CAN_CHANNEL_COUNT; k++) {
        char idbuf[64];
        int used = 0;
        idbuf[0] = '\0';
        if (nids[k] == 0) {
            snprintf(idbuf, sizeof(idbuf), "—");
        } else {
            int j;
            for (j = 0; j < nids[k]; j++) {
                int w = snprintf(idbuf + used, sizeof(idbuf) - (size_t)used,
                                 "%s0x%03X", j ? " " : "", ids[k][j]);
                if (w > 0 && (size_t)w < sizeof(idbuf) - (size_t)used) used += w;
            }
            if (nids[k] >= SCAN_MAX_IDS) {
                snprintf(idbuf + used, sizeof(idbuf) - (size_t)used, " …");
            }
        }
        printf("  │ %u (MCAN%u)  │ %8u │ %-31s │\n", k, k, cnt[k], idbuf);
    }
    printf("  └────────────┴──────────┴─────────────────────────────────┘\n");

    {
        uint32_t total = 0;
        for (k = 0; k < (int)ECAT2CAN_CAN_CHANNEL_COUNT; k++) total += cnt[k];
        if (total == 0) {
            printf("\n  四路都没有收到任何帧。检查：设备供电 / 接线通断 /\n"
                   "  波特率是否 1 Mbps / 终端电阻是否接上。\n");
        } else {
            printf("\n  合计 %u 帧。出现帧的通道说明该路收发链路正常。\n", total);
        }
    }
    return 0;
}

/* ===================== bench：性能与实时性基准 ===================== *
 * 测四件事：
 *   1) EtherCAT 往返延迟（主机 -> ESC -> 主机）的分布与抖动
 *   2) 主机 1 kHz 循环的**实际拍周期**与抖动（不装 PREEMPT_RT 的真实水平）
 *   3) CAN 上行吞吐（电机/从站反馈的到达率）
 *   4) 上行 PDO 里的帧数分布（看有没有突发/丢帧）
 */
/* 绝对时间节拍：把下一拍的绝对时刻算出来再 nanosleep，
 * 这样"处理耗时"会被自然吸收，不会像 usleep(1000) 那样每拍累积漂移。
 * （usleep 是"至少睡 1ms"，加上处理时间实际会跑到 ~1122us，见 bench 实测） */
static void tick_sleep(struct timespec *next, int period_us)
{
    next->tv_nsec += (long)period_us * 1000L;
    while (next->tv_nsec >= 1000000000L) {
        next->tv_nsec -= 1000000000L;
        next->tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void print_stats(const char *name, uint32_t *v, int n, const char *unit)
{
    uint64_t sum = 0;
    double mean, var = 0.0;
    int i;

    if (n <= 0) { printf("  %-18s (无数据)\n", name); return; }

    qsort(v, (size_t)n, sizeof(uint32_t), cmp_u32);
    for (i = 0; i < n; i++) sum += v[i];
    mean = (double)sum / n;
    for (i = 0; i < n; i++) {
        double d = (double)v[i] - mean;
        var += d * d;
    }
    var /= n;

    printf("  %-18s min %6u  p50 %6u  p99 %6u  max %6u  avg %8.1f  σ %6.1f  %s\n",
           name, v[0], v[n / 2], v[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1],
           v[n - 1], mean, sqrt(var), unit);
}

static int cmd_bench(fieldbus_t *fb, int cycles, uint8_t ch)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    const ecat2can_tx_rx_message *in = FB_IN(fb, grp);
    uint32_t *rtt, *period, *fcnt;
    int i, n = 0, nf = 0;
    uint64_t total_frames = 0, total_bytes = 0;
    struct timespec prev, cur, t_start, t_end;
    double real_s = 0.0;
    int first = 1;
    uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    if (cycles < 100) cycles = 100;
    rtt    = malloc(sizeof(uint32_t) * (size_t)cycles);
    period = malloc(sizeof(uint32_t) * (size_t)cycles);
    fcnt   = malloc(sizeof(uint32_t) * (size_t)cycles);
    if (!rtt || !period || !fcnt) { printf("内存不足\n"); free(rtt); free(period); free(fcnt); return 1; }

    printf("BENCH：%d 拍，通道 %u（MCAN%u），WORK_MODE 保持链路真实负载\n",
           cycles, ch, ch);
    printf("       目标周期 1000 us\n\n");

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    {
    struct timespec next = t_start;
    for (i = 0; i < cycles && !g_stop; i++) {
        int nfr;

        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = WORK_MODE;
        out->frame_count = 1;
        out->seq = (uint8_t)i;
        build_frame(out, 0, ch, 0x123, data, 8);

        fb_roundtrip(fb);

        clock_gettime(CLOCK_MONOTONIC, &cur);
        if (!first) {
            period[nf] = (uint32_t)((cur.tv_sec - prev.tv_sec) * 1000000
                                    + (cur.tv_nsec - prev.tv_nsec) / 1000);
            nf++;
        }
        prev = cur;
        first = 0;

        rtt[n++] = (uint32_t)fb->roundtrip_us;

        nfr = (int)in->frame_count;
        if (nfr > (int)ECAT2CAN_MAX_FRAME_COUNT) nfr = (int)ECAT2CAN_MAX_FRAME_COUNT;
        fcnt[i] = (uint32_t)nfr;
        total_frames += (uint64_t)nfr;
        total_bytes  += (uint64_t)nfr * 11u;

        tick_sleep(&next, 1000);   /* ★ 绝对时间，锁 1 kHz */
    }
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    real_s = (double)(t_end.tv_sec - t_start.tv_sec)
           + (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    printf("── EtherCAT 链路 ────────────────────────────────────────────────\n");
    print_stats("往返延迟", rtt, n, "us");
    printf("\n── 主机循环 ────────────────────────────────────────────────────\n");
    print_stats("实际拍周期", period, nf, "us");
    printf("\n── CAN 上行 ────────────────────────────────────────────────────\n");
    {
        int j, hist[ECAT2CAN_MAX_FRAME_COUNT + 1];

        memset(hist, 0, sizeof(hist));
        for (j = 0; j < cycles; j++) {
            unsigned f = fcnt[j];
            if (f > ECAT2CAN_MAX_FRAME_COUNT) f = ECAT2CAN_MAX_FRAME_COUNT;
            hist[f]++;
        }
        printf("  每拍帧数分布     ");
        for (j = 0; j <= (int)ECAT2CAN_MAX_FRAME_COUNT; j++) {
            if (hist[j]) printf("%d帧:%.1f%%  ", j, 100.0 * hist[j] / cycles);
        }
        printf("\n");

        /* ★ 用实测墙钟时长，不能用 cycles/1000 估算 */
        printf("  实测时长 %.2f s（%d 拍 → 实际 %.1f Hz，目标 1000 Hz）\n",
               real_s, cycles, cycles / real_s);
        printf("  合计 %llu 帧 = **%.1f 帧/秒**；PDO 上行 %.1f KB/s\n",
               (unsigned long long)total_frames,
               total_frames / real_s, total_bytes / real_s / 1024.0);
        printf("\n  判读：若对端是 1 kHz 上报（GM6020/C620），帧/秒应接近 1000。\n"
               "        明显低于 1000 说明有丢帧 —— 从站按 (通道,ID) 去重，\n"
               "        一拍内同 ID 的多帧只报最后一帧（见 Tide_ECAN 移植记录）。\n");
    }

    free(rtt); free(period); free(fcnt);
    return 0;
}

/* ===================== bench_lat：端到端延迟（主机 → CAN 总线）===================== *
 *
 * 需要一条已知的总线对端（如 PCAN-USB）挂在**同一个 CAN 座子**上。
 *
 * 测的是这一整条链：
 *     主机写 EtherCAT 帧 → ESC → 网关解析 → MCAN 发送 → 收发器 → 总线
 *       → 对端收发器 → USB → 内核打时间戳 → 本进程收到
 *
 * 对端的 RX 时间戳来自 SocketCAN 的 SO_TIMESTAMP，用的是**本机系统时钟**，
 * 所以能和发送前的 t0 直接相减，得到绝对延迟（不是估算）。
 *
 * 两个读数：
 *   "发出→收到"  —— 上面整条链
 *   "EtherCAT 往返" —— 同一次的 EtherCAT RTT，可对比看 CAN 段占多少
 */
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <poll.h>

static int can_open_rx(const char *ifname)
{
    int fd;
    struct sockaddr_can addr;
    struct ifreq ifr;
    int on = 1;

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) { perror("socket(PF_CAN)"); return -1; }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "找不到 CAN 接口 %s（PCAN 插好了吗 / 是不是叫别的名字？）\n", ifname);
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(PF_CAN)");
        close(fd);
        return -1;
    }
    /*
     * 让内核在收包时打时间戳。
     *
     * ★ 必须用 SO_TIMESTAMPNS：它给的是 struct timespec，
     *   而 SO_TIMESTAMP 给的是 struct timeval（秒+微秒）。
     *   早先这里写成 SO_TIMESTAMP 却按 timespec 去 memcpy，
     *   时间戳全是垃圾，导致"一帧都收不到"的假象 —— 踩过。
     */
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) < 0) {
        /* 内核不支持纳秒时间戳时退回微秒版（读取处按 timeval 处理） */
        fprintf(stderr, "SO_TIMESTAMPNS 不可用，退回 SO_TIMESTAMP\n");
        setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &on, sizeof(on));
    }
    return fd;
}

static uint64_t ts_to_us(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000ull + (uint64_t)ts->tv_nsec / 1000ull;
}

static int cmd_bench_lat(fieldbus_t *fb, const char *canif, uint8_t ch,
                         uint32_t id, int count)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    uint32_t *lat, *rtt;
    uint8_t data[8];
    int fd, n = 0, i, misses = 0;
    double real_s = 0.0;
    struct timespec t0, t1, t_start, t_end;

    if (count < 10) count = 10;

    fd = can_open_rx(canif);
    if (fd < 0) return 1;

    lat = malloc(sizeof(uint32_t) * (size_t)count);
    rtt = malloc(sizeof(uint32_t) * (size_t)count);
    if (!lat || !rtt) { free(lat); free(rtt); close(fd); return 1; }

    printf("BENCH_LAT：%d 次，通道 %u（MCAN%u），监听 %s\n", count, ch, ch, canif);
    printf("  测量链：主机 → EtherCAT → 网关 → MCAN → 收发器 → 总线 → %s → 内核时间戳\n\n", canif);

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    for (i = 0; i < count && !g_stop; i++) {
        struct pollfd pfd;
        struct msghdr msg;
        struct iovec iov;
        struct can_frame cf;
        char cbuf[256];
        int pr;
        uint64_t a, b;

        /* 数据里放递增序号，便于确认收到的就是这一拍发的那一帧 */
        data[0] = (uint8_t)(i >> 8);
        data[1] = (uint8_t)(i & 0xFF);
        data[2] = 0xA5; data[3] = 0x5A;
        data[4] = data[5] = data[6] = data[7] = 0;

        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = WORK_MODE;
        out->frame_count = 1;
        out->seq = (uint8_t)i;
        build_frame(out, 0, ch, id, data, 8);

        /*
         * ★ t0 必须用 CLOCK_REALTIME，不能用 CLOCK_MONOTONIC！
         *   内核给 SO_TIMESTAMPNS 打的是 **CLOCK_REALTIME**（墙上时间），
         *   与 MONOTONIC 是两个完全不同的时基；混用会让 b-a 恒为负数，
         *   表现为"一帧都没收到"的假象 —— 踩过两次了。
         */
        clock_gettime(CLOCK_REALTIME, &t0);
        fb_roundtrip(fb);           /* ★ 这一刻 EtherCAT 帧带着命令发出去 */
        clock_gettime(CLOCK_MONOTONIC, &t1);

        /* 到下一拍之前把 CAN 对端收到的帧收上来 */
        pfd.fd = fd; pfd.events = POLLIN;
        for (;;) {
            pr = poll(&pfd, 1, 5);
            if (pr <= 0) break;

            memset(&msg, 0, sizeof(msg));
            iov.iov_base = &cf; iov.iov_len = sizeof(cf);
            msg.msg_iov = &iov; msg.msg_iovlen = 1;
            msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);

            if (recvmsg(fd, &msg, MSG_DONTWAIT) < 0) break;

            {
                struct cmsghdr *cm;
                int got = 0;
                struct timespec rts;
                for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
                    if (cm->cmsg_level != SOL_SOCKET) continue;
                    if (cm->cmsg_type == SCM_TIMESTAMPNS) {
                        memcpy(&rts, CMSG_DATA(cm), sizeof(rts));   /* timespec */
                        got = 1;
                    } else if (cm->cmsg_type == SCM_TIMESTAMP) {
                        struct timeval tv;                          /* timeval！ */
                        memcpy(&tv, CMSG_DATA(cm), sizeof(tv));
                        rts.tv_sec = tv.tv_sec;
                        rts.tv_nsec = (long)tv.tv_usec * 1000L;
                        got = 1;
                    }
                }
                /* 只接受"这一拍发出去的那一帧"：两个字节的序号都要对上。
                 * ★ 早先这里把高字节写死成 0 比较，导致 i>=256 之后
                 *   data[0]=1 永远对不上，每帧都被当迟到帧丢掉 ——
                 *   表现为样本数恰好卡在 256（踩过）。 */
                if (got && cf.can_id == id &&
                    (cf.data[0] != (uint8_t)(i >> 8) || cf.data[1] != (uint8_t)(i & 0xFF))) {
                    continue;
                }
                if (got && cf.can_id == id) {
                    a = ts_to_us(&t0);
                    b = ts_to_us(&rts);
                    if (b >= a && b - a < 100000u) {   /* 只接受 100ms 内的，排除异常 */
                        lat[n] = (uint32_t)(b - a);
                        rtt[n] = (uint32_t)fb->roundtrip_us;
                        n++;
                    }
                    break;
                }
            }
        }
        if (n <= i) misses++;

        clock_gettime(CLOCK_MONOTONIC, &t_end);   /* 节拍基准用 MONOTONIC */
        {
            struct timespec nx = t_end;
            tick_sleep(&nx, 1000);
        }
    }
    real_s = 0.0;
    (void)real_s;

    printf("── 端到端延迟（主机发出 → %s 收到）──\n", canif);
    if (n > 0) print_stats("发出→收到", lat, n, "us");
    else printf("  (一帧都没收到 —— 检查 %s 是否和通道 %u 在同一条总线上)\n", canif, ch);
    if (n > 0) {
        printf("\n── 同批次 EtherCAT 往返（可对比）──\n");
        print_stats("EtherCAT 往返", rtt, n, "us");
    }
    printf("\n  样本 %d / %d 次尝试\n", n, count);
    if (n > 0 && n < count) {
        printf("  有 %d 次没在当拍收到 —— 多数是两台设备的 1 kHz 时钟不同步导致的拍频，\n"
               "  少量属于正常；若比例很高则说明有丢帧或对端不在同一总线。\n", count - n);
    }
    (void)misses;

    free(lat); free(rtt); close(fd);
    return 0;
}

/* ===================== looplat：双 CAN 口回环实时性测试 ===================== *
 *
 * 用途：测出「主机发出 CAN 指令 → 到达 CAN 总线」所花的时间，作为控制环
 *       实时性的参考基线，并用于 Linux 优化前后的对比。
 *
 * 前提：把两个 CAN 座子用两根线短接（CANH↔CANH、CANL↔CANL），
 *       例如 ECAN-Lite 的 CN9(通道 2) ↔ CN1(通道 3)。
 *
 * 回路：
 *     主机 → EtherCAT → MCAN(tx) → 收发器 → 线 → 收发器 → MCAN(rx)
 *          → EtherCAT → 主机
 *
 * 为什么用这个回路：全程经过**两个真实收发器和一段真实 CAN 总线**，
 * 不含 USB、不含外部设备 —— 正是控制回路实际承受的那一段。
 * （用 PCAN 做接收端会把 USB 传输时间算进去，那是虚高的。）
 *
 * ── 测量方法 ─────────────────────────────────────────────────────
 *   连续 1 kHz 收发（与真实控制环同频，不是发一次歇一会儿）：
 *     1) 发：改 seq → 从站真的把帧发到 tx 通道
 *     2) 收：seq 保持不变、frame_count=0 → 从站不会重发，
 *            主机尽快轮询 EtherCAT，捕捉回声落在 rx 通道的那一刻
 *     3) 帧里带递增序号，防止上一拍的迟到帧混进来
 *
 * ── 精度边界（必须知道）─────────────────────────────────────────
 *   主机侧靠"尽快轮询"逼近到达时刻，轮询粒度 = 一次 EtherCAT 往返
 *   （约 60~70 us），所以延迟读数有 ±33 us 量级的量化。
 *
 * ── 单程估算 ────────────────────────────────────────────────────
 *   报告里给「往返 ÷ 2」作为单程参考值。回路两半不完全对称
 *   （CAN 线那 100 多 us 只占单向），绝对值可能有 ±20% 偏差。
 *   但**做优化前后对比时这个系统性偏差会抵消** —— 对比看的是变化量。
 *
 * 用法：looplat <tx通道> <rx通道> [秒数=30] [频率Hz=1000] [负载帧数=0]
 *
 * 负载帧数 > 0 时，每拍的 PDO 里除回环帧外再塞 N 帧（ID 0x201..），
 * 用来模拟「同一总线上还有其他电机在 1kHz 上报」的高负载场景。
 * 上限 16（PDO 共 17 槽，得留 1 个给回环帧）。
 *       频率默认 1000；写 2000 可测 2 kHz 控制频率（注意往返延迟已接近 500us
 *       的周期，很可能跨拍 —— 工具会如实报告）。
 */

/* 直方图最多这么多档 */
#define LP_MAX_BINS 20

static int cmd_looplat(fieldbus_t *fb, uint8_t tx_ch, uint8_t rx_ch,
                       int seconds, int freq_hz, int load_n)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    const ecat2can_tx_rx_message *in = FB_IN(fb, grp);
    uint32_t *lat;
    int cycles, i, n = 0, timeouts = 0;
    /*
     * 把超时拆成两类，否则无法定位问题出在 EtherCAT 还是 CAN 那一侧：
     *   ecat_to  —— fb_roundtrip() 返回负值：过程数据帧本身没回来（EtherCAT 侧）
     *   can_lost —— EtherCAT 正常，但轮询窗口内始终没等到 CAN 回声（CAN 侧丢帧）
     * 两者都会让控制环丢一次反馈更新，但修法完全不同。
     */
    int ecat_to = 0, can_lost = 0, to_this_cycle = 0;
    /*
     * 再细分"没收到"的原因：是回声【晚了一拍】还是【压根没来】。
     * 二者对 1 kHz 控制环都算丢一次反馈，但含义完全不同：
     *   late_echo —— CAN 通路是通的，只是这一圈走不完一个控制周期
     *   can_lost  —— 连晚到的回声都没有，是真正的丢帧
     * PDO 每拍刷新，上一拍的帧可能还留在里面，所以用 prev_late_seen
     * 保证同一拍的迟到回声只记一次。
     */
    int late_echo = 0, prev_late_seen = 0;
    uint32_t lmin = 0xFFFFFFFFu, lmax = 0, last_d = 0;
    uint64_t lsum = 0;
    double drift_first = 0.0, drift_last = 0.0;
    uint64_t load_rx = 0;
    /* 帧匹配校验 */
    int seq_ok = 0, seq_lost = 0, seq_anomaly = 0, content_bad = 0, cross_cycle = 0;
    int expect = 0;              /* 期望匹配到的下一拍序号 */
    struct timespec t_start, t_end, next, last_disp;
    double real_s;
    uint8_t data[8];
    /*
     * 诊断用：轮询过程中把从站【最近一次非空的上行 PDO】存下来。
     * ★ 不能等到测试结束再读 —— 从站上报的是"自上次以来收到的帧"，
     *   跑完 15 秒后那一片多半已经空了，什么都看不到。
     */
    uint8_t last_in_buf[ECAT2CAN_PDO_BYTES];
    int last_in_valid = 0;
    int period_us, show_progress;

    if (seconds < 1) seconds = 10;
    if (seconds > 3600) seconds = 3600;
    if (freq_hz < 100) freq_hz = 100;
    if (freq_hz > 10000) freq_hz = 10000;
    /* 负载帧数：0 = 静默基线；上限 16 —— 因为 PDO 只有 17 个槽位，
     * 留 1 个给回环帧，再多就会把回环帧挤出上行 PDO。 */
    if (load_n < 0)  load_n = 0;
    if (load_n > 16) load_n = 16;

    period_us = 1000000 / freq_hz;
    cycles = seconds * freq_hz;

    lat = malloc(sizeof(uint32_t) * (size_t)cycles);
    if (!lat) { printf("内存不足\n"); return 1; }

    printf("═══ 双 CAN 口回环实时性测试 ═══\n");
    if (tx_ch == rx_ch) {
        printf("  回路   : 主机 → EtherCAT → MCAN%u → 片内回环 → MCAN%u → EtherCAT → 主机\n",
               tx_ch, rx_ch);
        printf("           ★ 同通道回环：需要从站处于 MCAN 外部回环模式，\n"
               "             路径里没有收发器和线缆，测得的是「主机↔从站」这一段。\n");
    } else {
        printf("  回路   : 主机 → EtherCAT → MCAN%u → 收发器 → 线 → 收发器 → MCAN%u → EtherCAT → 主机\n",
               tx_ch, rx_ch);
    }
    printf("  参数   : %d s @ %d Hz（周期 %d us，连续收发）\n",
           seconds, freq_hz, period_us);
    if (period_us < 700) {
        printf("  ⚠ 周期 %d us 已接近往返延迟量级，可能跨拍 —— 若超时率高属正常\n",
               period_us);
    }
    if (load_n > 0) {
        double fps = (double)(1 + load_n) * freq_hz;
        printf("  总线负载: 每拍 1+%d 帧 → %.0f 帧/秒，估算占用 %.0f%%%s\n",
               load_n, fps, fps * 120.0 / 1e6 * 100.0,
               (fps * 120.0 / 1e6 > 1.0) ? "  ⚠️ 超过总线容量，会丢帧" : "");
        printf("            负载帧 ID 0x201~0x%03X（贴近电机反馈的优先级）\n",
               0x200 + load_n);
        printf("            回环帧 ID 0x200（贴近控制指令，优先级最高）\n");
    } else {
        printf("  总线负载: 无（静默基线）—— 加第 6 个参数可灌负载\n");
    }
    printf("  轮询粒度: 约 66 us（一次 EtherCAT 往返）→ 延迟读数有 ±33 us 量化\n\n");

    /*
     * 过程显示：单行原地刷新，用 \r 回到行首 + \x1b[K 清到行尾，
     * 所以**不会重叠**。只在 stdout 是终端时显示 —— 重定向到文件时
     * 输出保持干净，方便做前后对比。
     */
    show_progress = isatty(STDOUT_FILENO);

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    next = t_start;
    clock_gettime(CLOCK_MONOTONIC, &last_disp);

    for (i = 0; i < cycles && !g_stop; i++) {
        struct timespec t0, t1, tnow, deadline;
        int guard, got = 0;
        uint32_t seen_mask = 0;
        uint8_t mhi = (uint8_t)(i >> 8), mlo = (uint8_t)(i & 0xFF);

        /*
         * 本拍必须在「下一拍开始」之前收工。
         * ★ 注意：tick_sleep 是先推进 next 再睡，所以每拍开始时 next 正好
         *   等于**当前**时刻（不是下一拍）—— 截止期必须是 next + 一个周期。
         *   早先直接拿 next 当截止期，判断每拍立即为真，轮询第一次就退出，
         *   结果一帧都收不到（踩过）。
         */
        deadline = next;
        deadline.tv_nsec += (long)period_us * 1000L;
        while (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }

        data[0] = mhi; data[1] = mlo;
        data[2] = 0xC3; data[3] = 0x3C;
        data[4] = data[5] = data[6] = data[7] = 0;

        /* ---- 第 1 拍：回环帧 + 负载帧，同一个 PDO 里一起下发 ----
         * 槽位 0 = 回环帧（0x200，最高优先级，先发）
         * 槽位 1..N = 负载帧（0x201..，模拟其他电机的 1kHz 反馈） */
        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = WORK_MODE;
        out->frame_count = (uint8_t)(1 + load_n);
        out->seq = (uint8_t)i;              /* seq 变化 → 从站真的发这些帧 */
        build_frame(out, 0, tx_ch, 0x200, data, 8);
        {
            int L;
            for (L = 0; L < load_n; L++) {
                uint8_t ld[8];
                ld[0] = (uint8_t)(0xA0 + L);
                ld[1] = (uint8_t)(i & 0xFF);
                ld[2] = (uint8_t)(i >> 8);
                ld[3] = (uint8_t)L;
                ld[4] = ld[5] = ld[6] = ld[7] = 0;
                build_frame(out, 1 + L, tx_ch, (uint32_t)(0x201 + L), ld, 8);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &t0);
        to_this_cycle = 0;
        prev_late_seen = 0;
        if (fb_roundtrip(fb) < 0) {     /* EtherCAT 这一拍的过程数据没回来 */
            ecat_to++;
            timeouts++;
            continue;
        }

        /* ---- 后续：不下发、seq 不变，尽快轮询找回声 ----
         * 轮询以「下一拍截止时刻」为界，绝不侵占下一拍 —— 这样即使
         * 回环跨拍（例如 2 kHz 时周期只有 500us），也不会把时间表拖乱。*/
        for (guard = 0; guard < 200; guard++) {
            int k, nfr;

            memset(out, 0, ECAT2CAN_PDO_BYTES);
            out->command = WORK_MODE;
            out->frame_count = 0;           /* ★ 不再下发 */
            out->seq = (uint8_t)i;          /* ★ seq 不变 → 不会重发 */

            if (fb_roundtrip(fb) < 0) {
                /*
                 * 轮询窗口内可能连续几次都超时，但这是【同一拍】的失败，
                 * 只记一次 —— 否则 ecat_to 会成倍虚高，误导归因。
                 */
                if (!to_this_cycle) { ecat_to++; to_this_cycle = 1; }
                continue;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);

            nfr = (int)in->frame_count;
            if (nfr > (int)ECAT2CAN_MAX_FRAME_COUNT) nfr = (int)ECAT2CAN_MAX_FRAME_COUNT;
            if (nfr > 0) {      /* 抓到一帧就留底，供"完全没收到"时诊断 */
                memcpy(last_in_buf, in, ECAT2CAN_PDO_BYTES);
                last_in_valid = 1;
            }
            for (k = 0; k < nfr; k++) {
                const can_frame_struct *f = &in->can_frames[k];
                /*
                 * 负载帧用**位图**记录，每拍每 ID 只算一次。
                 * ★ 不能直接累加 —— 一拍要轮询好几次，同一批帧会被重复计数
                 *   （踩过：4 帧负载被算成 10723 帧/秒，比下发的 5000 还多）。
                 *   每拍每 ID 至多一帧，所以"出现过的 ID 数"正好等于
                 *   这一拍成功送到总线上的负载帧数。
                 */
                if (f->can_channel == rx_ch && f->std_id >= 0x201 &&
                    f->std_id <= 0x200 + (uint32_t)load_n) {
                    seen_mask |= 1u << (f->std_id - 0x201);
                }
                if (f->can_channel == rx_ch && f->std_id == 0x200 &&
                    f->data[0] != mhi && f->data[1] == (uint8_t)((i - 1) & 0xFF) &&
                    i > 0 && !prev_late_seen) {
                    prev_late_seen = 1;     /* 上一拍的回声这一拍才到 */
                }
                if (f->can_channel == rx_ch && f->std_id == 0x200 &&
                    f->data[0] == mhi && f->data[1] == mlo) {
                    got = 1;
                    /* 内容回读校验：这一帧的 8 个字节应当原样回来。
                     * 标记占 [0..1]，[2..3] 是固定魔数，[4..7] 恒 0 —— 全部可校验。 */
                    if (f->data[2] != 0xC3 || f->data[3] != 0x3C ||
                        f->data[4] || f->data[5] || f->data[6] || f->data[7]) {
                        content_bad++;
                    }
                    break;
                }
            }
            if (got) break;

            clock_gettime(CLOCK_MONOTONIC, &tnow);
            if (tnow.tv_sec > deadline.tv_sec ||
                (tnow.tv_sec == deadline.tv_sec && tnow.tv_nsec >= deadline.tv_nsec)) {
                break;              /* 到下一拍开始时刻了，本拍放弃 */
            }
        }

        {   /* 本拍成功送达的负载帧数 = 位图里置位的个数 */
            uint32_t m = seen_mask;
            while (m) { load_rx += (m & 1u); m >>= 1; }
        }

        if (got) {
            uint64_t a = (uint64_t)t0.tv_sec * 1000000ull + (uint64_t)t0.tv_nsec / 1000ull;
            uint64_t b = (uint64_t)t1.tv_sec * 1000000ull + (uint64_t)t1.tv_nsec / 1000ull;
            uint32_t d = (uint32_t)(b - a);
            lat[n++] = d;
            last_d = d;

            /* ── 帧匹配校验 ──
             * 正常情况：这一拍匹配到的就是这一拍发的帧（i == expect）。
             * 若 i > expect，说明中间有拍超时、序号断档 —— 这是**正常**的。
             * 若 i < expect，说明匹配到了更早的帧 —— 那才是**真的认错了**。 */
            if (i == expect)       seq_ok++;
            else if (i > expect)   seq_lost += (i - expect);
            else                   seq_anomaly++;
            expect = i + 1;

            /* 超过一个控制周期 = 回声晚了一整拍以上 */
            if (d > (uint32_t)period_us) cross_cycle++;
            lsum += d;
            if (d < lmin) lmin = d;
            if (d > lmax) lmax = d;
        } else {
            timeouts++;
            if (prev_late_seen) {
                late_echo++;            /* 回声晚了一拍才到，不是丢帧 */
            } else {
                can_lost++;             /* 连迟到回声都没有 —— 真丢帧 */
            }
        }

        /* ---- 过程显示：每 100ms 刷新一次，单行原地覆盖 ---- */
        if (show_progress) {
            clock_gettime(CLOCK_MONOTONIC, &tnow);
            {
                long ms = (tnow.tv_sec - last_disp.tv_sec) * 1000
                        + (tnow.tv_nsec - last_disp.tv_nsec) / 1000000;
                if (ms >= 100) {
                    double done = (double)(tnow.tv_sec - t_start.tv_sec)
                                + (double)(tnow.tv_nsec - t_start.tv_nsec) / 1e9;
                    double left = (double)seconds - done;
                    printf("\r  ⏱ %5.1f/%.0fs  剩 %5.1fs │ %6d 拍 │ 当前 %4u 最大 %4u "
                           "均值 %5.1f us │ 超时 %d   \x1b[K",
                           done, (double)seconds, left < 0 ? 0.0 : left,
                           i + 1, last_d, lmax, n ? (double)lsum / n : 0.0, timeouts);
                    fflush(stdout);
                    last_disp = tnow;
                }
            }
        }

        /* 绝对时间表锁目标频率 —— 保证是「连续」，不是「发一次歇一会」 */
        tick_sleep(&next, period_us);
        last_d = 0;
    }
    if (show_progress) printf("\r\x1b[K");    /* 清掉进度行，后面接完整报告 */

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    real_s = (double)(t_end.tv_sec - t_start.tv_sec)
           + (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* ================= 报告 ================= */
    if (n == 0) {
        /*
         * 一次回环帧都没收到 —— 但这不一定是"没接线"。
         * 可能是接错座子（帧出现在别的通道上）、或 CANH/CANL 接反。
         * 所以把轮询窗口里**实际看到的报文**原样列出来，一眼就能区分：
         *   · 什么都没列出来        → 真的没接线 / 接线断路
         *   · 列出了别的通道的帧    → 座子接错了（例如把 CAN2 接成了 CAN1）
         *   · 列出了别的 ID 的帧    → 总线上的不是我们的回环帧
         */
        const ecat2can_tx_rx_message *di = last_in_valid
            ? (const ecat2can_tx_rx_message *)last_in_buf
            : FB_IN(fb, grp);
        int dn = (int)di->frame_count, dj;
        if (dn > (int)ECAT2CAN_MAX_FRAME_COUNT) dn = (int)ECAT2CAN_MAX_FRAME_COUNT;

        printf("❌ 一次都没收到回环帧。请确认：\n");
        printf("   1) 通道 %u 和 %u 的座子确实用两根线短接了（CANH↔CANH、CANL↔CANL）\n",
               tx_ch, rx_ch);
        printf("   2) 接的是 CANH 对 CANH、CANL 对 CANL（接反不通）\n");
        printf("\n   【诊断】从站最后上报的 CAN 报文共 %d 条：\n", dn);
        if (dn == 0) {
            printf("     （空）—— 从站一个 CAN 帧都没收到。\n");
            printf("     → 多半是【根本没有接线】或【线断了/没插紧】；\n");
            printf("       也可能是收发器没使能、或两端共地不良。\n");
        } else {
            for (dj = 0; dj < dn && dj < 8; dj++) {
                const can_frame_struct *df = &di->can_frames[dj];
                printf("     [%d] 通道 CAN%u  ID=0x%03X  DLC=%u  数据:",
                       dj, df->can_channel, df->std_id, df->dlc);
                {
                    unsigned db;
                    for (db = 0; db < df->dlc && db < 8; db++) printf(" %02X", df->data[db]);
                }
                if (df->can_channel != rx_ch) {
                    printf("   ← ★ 通道不是 %u！座子可能接错了", rx_ch);
                } else if (df->std_id != 0x200) {
                    printf("   ← ★ ID 不是 0x200");
                }
                printf("\n");
            }
            printf("\n     → 上面若有「通道不是 %u」的帧，说明【线接到了别的座子】。\n", rx_ch);
        }
        free(lat);
        return 1;
    }

    /*
     * ★ 长时稳定性必须在 qsort **之前**算 —— 排序后数组就丢了时间顺序，
     *   拿到的会变成「最小 1/4 → 最大 1/4」，那是分位数差不是漂移。
     *   （踩过：两次跑都稳定显示 +15%，看着像热漂移，其实是这个 bug。）
     */
    {
        int q = n / 4, k;
        if (q >= 1) {
            double f = 0, b = 0;
            for (k = 0; k < q; k++) f += lat[k];
            for (k = n - q; k < n; k++) b += lat[k];
            drift_first = f / q;
            drift_last  = b / q;
        }
    }

    qsort(lat, (size_t)n, sizeof(uint32_t), cmp_u32);

    printf("── 往返延迟 ────────────────────────────────────────────────────\n");
    {
        double mean = (double)lsum / n, var = 0.0;
        int k;
        for (k = 0; k < n; k++) { double d = (double)lat[k] - mean; var += d * d; }
        var /= n;
        printf("  样本     : %d / %d 次（%.2f s）\n", n, cycles, real_s);
        printf("  最小     : %u us\n", lmin);
        printf("  p50      : %u us\n", lat[n / 2]);
        printf("  p90      : %u us\n", lat[(int)(n * 0.90) < n ? (int)(n * 0.90) : n - 1]);
        printf("  p99      : %u us\n", lat[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1]);
        printf("  p99.9    : %u us\n", lat[(int)(n * 0.999) < n ? (int)(n * 0.999) : n - 1]);
        printf("  最大     : %u us\n", lmax);
        printf("  平均     : %.1f us\n", mean);
        printf("  抖动 σ   : %.1f us\n", sqrt(var));
    }

    /* ---- 直方图（自适应分档，档边界随输出一起打印，便于复现）---- */
    printf("\n── 延迟直方图 ──────────────────────────────────────────────────\n");
    {
        uint32_t lo = (lmin / 20u) * 20u;              /* 20us 对齐 */
        uint32_t hi = ((lmax + 19u) / 20u) * 20u;
        int nb, k;
        int cnt[LP_MAX_BINS + 1];

        if (hi <= lo) hi = lo + 20u;
        nb = (int)((hi - lo) / 20u) + 1;
        if (nb > LP_MAX_BINS) nb = LP_MAX_BINS;
        memset(cnt, 0, sizeof(cnt));
        for (k = 0; k < n; k++) {
            int b = (int)((lat[k] - lo) / 20u);
            if (b < 0) b = 0;
            if (b >= nb) b = nb;                        /* 溢出档 */
            cnt[b]++;
        }
        for (k = 0; k <= nb; k++) {
            double pct = 100.0 * cnt[k] / n;
            int bars = (int)(pct / 2.0);
            int j;
            if (k < nb) printf("  %5u-%5u us %7.3f%% │", lo + (uint32_t)k * 20u,
                               lo + (uint32_t)k * 20u + 19u, pct);
            else        printf("  %5u+    us %7.3f%% │", hi, pct);
            for (j = 0; j < bars && j < 45; j++) putchar('#');
            putchar('\n');
        }
    }

    /* ---- 单程估算 ---- */
    printf("\n── 单程估算（往返 ÷ 2）────────────────────────────────────────\n");
    {
        uint32_t p50 = lat[n / 2];
        uint32_t p99 = lat[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1];
        printf("  p50      : %u us   =  %.2f 个控制周期（%d Hz，周期 %d us）\n",
               p50 / 2, (double)(p50 / 2) / (double)period_us, freq_hz, period_us);
        printf("  p99      : %u us   =  %.2f 个控制周期\n",
               p99 / 2, (double)(p99 / 2) / (double)period_us);
        printf("  ✅ 这就是「主机发出 CAN 指令 → 到达总线」的参考值\n");
        printf("  ⚠ 粗略估算：回路两半不完全对称（CAN 线只占单向），绝对值可能\n");
        printf("     有 ±20%% 偏差；但**做 Linux 优化前后对比时该偏差抵消**，\n");
        printf("     对比看的是变化量，不受影响。\n");
    }

    /* ---- 与目标控制周期的关系 ---- */
    printf("\n── 与 %d Hz 控制周期（%d us）的关系 ────────────────────────────\n",
           freq_hz, period_us);
    {
        uint32_t p50 = lat[n / 2];
        double rt = (double)lsum / n;
        if ((int)p50 < period_us) {
            printf("  往返 %.0f us < %d us   ✅ 单拍内可完成往返，不跨拍（余量 %d us）\n",
                   rt, period_us, period_us - (int)p50);
        } else {
            printf("  往返 %.0f us ≥ %d us   ⚠️ 往返跨拍 —— 控制环会看到 1 拍以上滞后，\n"
                   "                          这个频率下回环跟不上，需降频或优化\n",
                   rt, period_us);
        }
    }

    /* ---- 总线负载实况 ---- */
    if (load_n > 0) {
        double fps_total = (double)(1 + load_n) * freq_hz;   /* 含回环帧，算占用用 */
        double fps_load  = (double)load_n * freq_hz;         /* 只算负载帧，算达成率用 */
        double fps_rx    = (double)load_rx / real_s;
        printf("\n── 总线负载实况 ────────────────────────────────────────────────\n");
        printf("  总线占用 : 每拍 %d 帧 × %d Hz = %.0f 帧/秒 → 估算 %.0f%%\n",
               1 + load_n, freq_hz, fps_total, fps_total * 120.0 / 1e6 * 100.0);
        printf("  负载帧   : 下发 %.0f 帧/秒 → 实收 %.0f 帧/秒（达成率 %.1f%%）\n",
               fps_load, fps_rx, fps_load > 0 ? fps_rx / fps_load * 100.0 : 0.0);
        printf("             ★ 达成率要和「负载帧下发数」比，不能和含回环帧的总数比\n");
        /* 达成率判据：拿负载帧自己比 */
        if (fps_rx < fps_load * 0.5) {
            printf("  ⚠️ 实收远低于下发 —— 总线已饱和，MCAN 发送队列积压/丢帧。\n"
                   "     此时「延迟」只反映成功发出的那部分，实际更糟。\n");
        } else if (fps_rx < fps_load * 0.9) {
            printf("  ⚠️ 实收略低于下发 —— 总线接近饱和，开始丢帧。\n");
        } else {
            printf("  ✅ 实收≈下发，总线尚未饱和，延迟数据可信。\n");
        }
    }

    /* ---- 帧匹配校验 ---- */
    printf("\n── 帧匹配校验（每次往返是否都匹配到「各自那一帧」）─────────────\n");
    printf("  唯一匹配   : %d 次（序号严格递增，就是这一拍自己发的帧）%s\n",
           seq_ok, (seq_anomaly == 0) ? "  ✅" : "");
    printf("  序号断档   : %d 拍（中间有拍超时，序号跳过）\n", seq_lost);
    if (seq_lost == timeouts) {
        printf("               ✅ 与超时数（%d）完全一致 → 断档全部来自超时，不是认错帧\n",
               timeouts);
    } else {
        printf("               ⚠️ 与超时数（%d）不一致 → 有不属于超时的断档，需排查\n",
               timeouts);
    }
    printf("  序号异常   : %d 次%s\n", seq_anomaly,
           (seq_anomaly == 0) ? "  ✅ 没有把更早的帧当成这一拍" :
           "  ❌ 匹配到了更早的帧，本次测量不可信");
    printf("  内容回读   : %s（%d 次字节不一致）\n",
           (content_bad == 0) ? "✅ 8 字节全部原样回来，无位错" : "❌ 存在位错",
           content_bad);
    printf("  跨拍测量   : %d 次%s\n", cross_cycle,
           (cross_cycle == 0) ? "  ✅ 回声都在一个控制周期内回来" :
           "  ⚠️ 有回声晚了一整拍以上，这些样本含跨拍时间");

    /* ---- 丢帧 ---- */
    printf("\n── 丢帧 ────────────────────────────────────────────────────────\n");
    printf("\n── 超时归因（区分 EtherCAT 侧与 CAN 侧）───────────────────────\n");
    printf("  EtherCAT 过程数据超时 : %d 次  (fb_roundtrip 返回负值)\n", ecat_to);
    printf("  回声晚了一拍（跨拍）  : %d 次  (CAN 通路是通的，但一圈走不完一个周期)\n", late_echo);
    printf("  CAN 回声未到（真丢帧）: %d 次  (连迟到的回声都没有)\n", can_lost);
    if (timeouts > 0) {
        if (ecat_to == 0 && can_lost > 0)
            printf("  → 全部出在 CAN 侧：查总线终端电阻、线长、从站 CAN 发送队列\n");
        else if (can_lost == 0 && ecat_to > 0)
            printf("  → 全部出在 EtherCAT 侧：查主站实时性、网卡、从站 ESC 负载\n");
        else if (ecat_to > 0 && can_lost > 0)
            printf("  → 两侧都有：先压 EtherCAT 侧（它是 CAN 通路的前置条件）\n");
    }

    if (timeouts == 0) {
        printf("  超时 0 次  ✅ 每一拍都拿到了回环\n");
    } else {
        printf("  超时 %d / %d 次（%.3f%%）  ← 每次 = 控制环丢一次反馈更新\n",
               timeouts, cycles, 100.0 * timeouts / cycles);
    }

    /* ---- 长时稳定性：前后 1/4 对比 ---- */
    printf("\n── 长时稳定性（前后 1/4 对比）──────────────────────────────────\n");
    if (drift_first > 0 && drift_last > 0) {
        double f = drift_first, b = drift_last;
        printf("  前 1/4   : %.1f us\n", f);
        printf("  后 1/4   : %.1f us\n", b);
        printf("  漂移     : %+.2f%%%s\n", (b - f) / f * 100.0,
               (fabs(b - f) / f < 0.05) ? "  ✅ 稳定" : "  ⚠️ 有漂移，可能是热漂移或后台负载变化");
    }

    printf("\n═══ %d Hz 基线（%d s）—— 可用作 Linux 优化前后的对比 ═══\n",
           freq_hz, seconds);
    printf("    对比时重点看 p99 / p99.9 / 最大 / 抖动 σ —— linux 调优主要改善**尾部**，\n");
    printf("    只看平均往往看不出效果。\n");

    free(lat);
    return 0;
}


/* ===================== looplat2：多从站同时回环压测 ===================== *
 *
 * 用法：looplat2 <从站列表> <tx通道> <rx通道> [秒数=30] [Hz=1000] [负载帧数=0]
 *   从站列表支持 "1,2" 或 "1-3" 或 "1,2,4"
 *
 * 和 looplat 的区别：
 *   looplat  —— 一次只测一个从站（ECAT2CAN_SLAVE 选），做完整的分位数/直方图/
 *               漂移/帧匹配自证，输出信息量大；
 *   looplat2 —— 在**同一个 EtherCAT 周期**里同时驱动多个从站的 CAN 回环，
 *               反映真实的多轴并发场景。输出精简，重点是各从站的
 *               往返延迟和丢帧率对比。
 *
 * 可行性依据：每个从站的 192 字节 PDO 区域互相独立（主机映射里按
 * slave_idx 分片），所以一条 EtherCAT 帧可以同时给 N 个从站下发各自的
 * 回环帧，再从各自的输入区域里分别匹配回声，互不干扰。
 *
 * 注意：多个从站同时回环时，CAN 侧是各自独立的物理总线（每块板自己的
 * CAN0↔CAN2），所以总线负载不叠加；真正叠加的是 EtherCAT 帧长度和
 * 从站的 PDI 处理负担 —— 这正是要压的东西。
 */
#define LP2_MAX_SLAVES  8
#define LP2_MAX_SAMPLES 40000      /* 每从站最多留这么多延迟样本 */

typedef struct {
    int      idx;                  /* 从站号 1..slavecount */
    uint32_t lat[LP2_MAX_SAMPLES];
    int      n;                    /* 已记录样本数 */
    int      lost;                 /* 累计未收到拍数 */
    int      got;                  /* 本拍已收到 */
    uint64_t lsum;
    uint32_t lmin, lmax;
} lp2_slave_t;

static int cmd_looplat_multi(fieldbus_t *fb, const int *slave_list, int nslave,
                             uint8_t tx_ch, uint8_t rx_ch,
                             int seconds, int freq_hz, int load_n)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    lp2_slave_t *sv;
    int cycles, i, s, k, period_us;
    int ecat_to = 0;
    uint64_t load_sent = 0;
    struct timespec t_start, t_end, next, t0, t1, tnow, deadline;
    double real_s;

    if (nslave < 1) { printf("!! 没选任何从站\n"); return 1; }
    if (nslave > LP2_MAX_SLAVES) { printf("!! 最多同时测 %d 个从站\n", LP2_MAX_SLAVES); return 1; }
    if (load_n < 0) load_n = 0;
    if (load_n > 16) load_n = 16;          /* PDO 只有 17 槽，留 1 个给回环帧 */
    if (seconds < 1) seconds = 10;
    if (freq_hz < 100) freq_hz = 100;
    if (freq_hz > 10000) freq_hz = 10000;
    period_us = 1000000 / freq_hz;

    sv = calloc((size_t)nslave, sizeof(*sv));
    if (sv == NULL) { printf("!! 内存不足\n"); return 1; }
    for (s = 0; s < nslave; s++) {
        sv[s].idx  = slave_list[s];
        sv[s].lmin = 0xFFFFFFFFu;
    }

    printf("\n═══ 多从站同时回环压测（looplat2）═══\n");
    printf("  从站     :");
    for (s = 0; s < nslave; s++) printf(" #%d", sv[s].idx);
    printf("   （共 %d 个，同一条 EtherCAT 帧同时驱动）\n", nslave);
    printf("  回环     : CAN%u 发 → CAN%u 收\n", tx_ch, rx_ch);
    printf("  参数     : %d s @ %d Hz（周期 %d us）\n", seconds, freq_hz, period_us);
    printf("  负载     : 每从站每拍 %d 帧（ID 0x201..）%s\n",
           load_n, load_n ? "" : "  ← 静默基线");
    printf("  ⚠ 前提：每个从站的 CAN%u 与 CAN%u 座子都已用两根线短接\n", tx_ch, rx_ch);

    cycles = seconds * freq_hz;
    if (cycles > LP2_MAX_SAMPLES) cycles = LP2_MAX_SAMPLES;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    next = t_start;

    for (i = 0; i < cycles && !g_stop; i++) {
        uint8_t mhi = (uint8_t)(i >> 8), mlo = (uint8_t)(i & 0xFF);

        deadline = next;
        deadline.tv_nsec += (long)period_us * 1000L;
        while (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }

        /* ---- 下发：给每个从站各写一份自己的 PDO ---- */
        for (s = 0; s < nslave; s++) {
            ecat2can_tx_rx_message *o = FB_OUT_N(fb, grp, sv[s].idx);
            memset(o, 0, ECAT2CAN_PDO_BYTES);
            o->command     = WORK_MODE;
            o->frame_count = (uint8_t)(1 + load_n);
            o->seq         = (uint8_t)i;
            o->can_frames[0].can_channel = tx_ch;
            o->can_frames[0].std_id      = 0x200;
            o->can_frames[0].dlc         = 8;
            o->can_frames[0].data[0]     = mhi;
            o->can_frames[0].data[1]     = mlo;
            o->can_frames[0].data[2]     = 0xC3;
            o->can_frames[0].data[3]     = 0x3C;
            for (k = 0; k < load_n; k++) {
                o->can_frames[1 + k].can_channel = tx_ch;
                o->can_frames[1 + k].std_id      = (uint32_t)(0x201 + k);
                o->can_frames[1 + k].dlc         = 8;
                o->can_frames[1 + k].data[0]     = mhi;
                o->can_frames[1 + k].data[1]     = mlo;
            }
            sv[s].got = 0;
        }
        if (load_n > 0) load_sent += (uint64_t)nslave * (uint64_t)load_n;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (fb_roundtrip(fb) < 0) { ecat_to++; continue; }

        /* ---- 轮询：不下发，尽快从各自输入区域找回声 ---- */
        for (k = 0; k < 200; k++) {
            int all_got = 1;

            for (s = 0; s < nslave; s++) {
                ecat2can_tx_rx_message *o = FB_OUT_N(fb, grp, sv[s].idx);
                memset(o, 0, ECAT2CAN_PDO_BYTES);
                o->command     = WORK_MODE;
                o->frame_count = 0;      /* ★ 不再下发，避免重发 */
                o->seq         = (uint8_t)i;
            }

            if (fb_roundtrip(fb) < 0) { ecat_to++; continue; }
            clock_gettime(CLOCK_MONOTONIC, &t1);

            for (s = 0; s < nslave; s++) {
                const ecat2can_tx_rx_message *in;
                int nfr, j;

                if (sv[s].got) continue;
                in  = FB_IN_N(fb, grp, sv[s].idx);
                nfr = (int)in->frame_count;
                if (nfr > (int)ECAT2CAN_MAX_FRAME_COUNT) nfr = (int)ECAT2CAN_MAX_FRAME_COUNT;
                for (j = 0; j < nfr; j++) {
                    const can_frame_struct *f = &in->can_frames[j];
                    if (f->can_channel == rx_ch && f->std_id == 0x200 &&
                        f->data[0] == mhi && f->data[1] == mlo) {
                        uint32_t d;
                        sv[s].got = 1;
                        d = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000
                                     + (t1.tv_nsec - t0.tv_nsec) / 1000);
                        if (sv[s].n < LP2_MAX_SAMPLES) sv[s].lat[sv[s].n++] = d;
                        sv[s].lsum += d;
                        if (d < sv[s].lmin) sv[s].lmin = d;
                        if (d > sv[s].lmax) sv[s].lmax = d;
                        break;
                    }
                }
            }

            for (s = 0; s < nslave; s++) if (!sv[s].got) { all_got = 0; break; }
            if (all_got) break;

            clock_gettime(CLOCK_MONOTONIC, &tnow);
            if (tnow.tv_sec > deadline.tv_sec ||
                (tnow.tv_sec == deadline.tv_sec && tnow.tv_nsec >= deadline.tv_nsec)) {
                break;
            }
        }

        for (s = 0; s < nslave; s++) if (!sv[s].got) sv[s].lost++;

        tick_sleep(&next, period_us);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    real_s = (t_end.tv_sec - t_start.tv_sec) + (t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    /* ---- 报告 ---- */
    printf("\n── 各从站结果（同一 EtherCAT 帧同时驱动，%d 拍 / %.1f 秒）──────\n",
           cycles, real_s);
    printf("  %-6s %8s %8s %8s %8s %8s %10s\n",
           "从站", "p50(us)", "p90(us)", "p99(us)", "max(us)", "成功", "丢帧率");
    printf("  %s\n", "------------------------------------------------------------------");
    for (s = 0; s < nslave; s++) {
        uint32_t p50 = 0, p90 = 0, p99 = 0;
        if (sv[s].n > 0) {
            qsort(sv[s].lat, (size_t)sv[s].n, sizeof(uint32_t), cmp_u32);
            p50 = sv[s].lat[(int)(sv[s].n * 0.50)];
            p90 = sv[s].lat[(int)(sv[s].n * 0.90)];
            p99 = sv[s].lat[(int)((sv[s].n - 1) * 0.99)];
        }
        printf("  #%-5d %8u %8u %8u %8u %8d %9.2f%%\n",
               sv[s].idx, p50, p90, p99,
               sv[s].n ? sv[s].lmax : 0,
               sv[s].n, 100.0 * sv[s].lost / (cycles ? cycles : 1));
    }

    printf("\n── 汇总 ────────────────────────────────────────────────────────\n");
    {
        int total_lost = 0;
        for (s = 0; s < nslave; s++) total_lost += sv[s].lost;
        printf("  并发放大：%d 个从站 × %.2f%% 平均丢帧率\n",
               nslave, 100.0 * total_lost / ((double)nslave * (cycles ? cycles : 1)));
        printf("  EtherCAT 侧超时（fb_roundtrip 负值）: %d 次\n", ecat_to);
        if (load_n > 0) {
            printf("  负载帧下发量: %llu 帧（仅 EtherCAT 下发侧计数）\n",
                   (unsigned long long)load_sent);
        }
        printf("\n  判读：\n");
        printf("    · 各从站 p50 接近、丢帧率接近  → 从站之间没有互相拖累 ✓\n");
        printf("    · 某个从站明显更差             → 它的 CAN 接线/收发器/PDI 有单独问题\n");
        printf("    · 全部从站的延迟/丢帧都随从站数上升 → 瓶颈在 EtherCAT 帧长或主站\n");
    }
    printf("已请求 INIT 状态并关闭网卡\n");

    free(sv);
    return 0;
}


/* ===================== rt：实时性合格性测试 ===================== *
 *
 * 与 bench 的区别：
 *   bench = 一次性快照（看几个百分位）
 *   rt    = 长时间跑，回答「这台机器能不能稳定跑住这个周期」
 *
 * 输出三样 bench 没有的东西：
 *   1) 抖动**直方图** —— 只看 p99 看不出分布形状（双峰 / 长尾）
 *   2) **截止期错失计数** —— 实时性的真正判据，不是平均值
 *   3) **主机环境诊断** —— governor / RT 优先级 / CPU 亲和性 / isolcpus
 *
 * 用法：rt [秒数] [通道]
 */

#define RT_MAX_BUCKETS 9

static const char *read_first_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (!fgets(buf, (int)n, f)) { fclose(f); return NULL; }
    fclose(f);
    {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == ' ')) buf[--l] = '\0';
    }
    return buf;
}

/* 打印主机的实时相关环境。这些是"跑不住 1kHz"的常见元凶。 */
static void rt_print_env(void)
{
    char buf[512];
    int pol = sched_getscheduler(0);
    struct sched_param sp;
    cpu_set_t set;

    printf("── 主机环境 ────────────────────────────────────────────────────\n");

    if (read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", buf, sizeof(buf))) {
        printf("  CPU 调频策略   : %-12s %s\n", buf,
               (strcmp(buf, "performance") == 0) ? "✅" :
               "⚠️ 建议 cpupower frequency-set -g performance");
    } else {
        printf("  CPU 调频策略   : (读不到，可能是虚拟机或没装 cpufreq)\n");
    }

    memset(&sp, 0, sizeof(sp));
    if (sched_getparam(0, &sp) == 0) {
        const char *pn;
        const char *hint;
        switch (pol) {
        case SCHED_FIFO:  pn = "SCHED_FIFO";  hint = "✅ 实时调度"; break;
        case SCHED_RR:    pn = "SCHED_RR";    hint = "✅ 实时调度（轮转）"; break;
        case SCHED_OTHER: pn = "SCHED_OTHER"; hint = "⚠️ 普通调度，建议 chrt -f 80（需 root）"; break;
#ifdef SCHED_BATCH
        case SCHED_BATCH: pn = "SCHED_BATCH"; hint = "❌ 批处理调度，会被主动降级 —— 实时性杀手，必须改"; break;
#endif
#ifdef SCHED_IDLE
        case SCHED_IDLE:  pn = "SCHED_IDLE";  hint = "❌ 空闲调度，只在 CPU 没事时才跑 —— 实时性杀手，必须改"; break;
#endif
        default: {
            /* 未知值直接显示原始数字，便于排查（踩过：SCHED_IDLE 被显示成"其他"） */
            static char ub[48];
            snprintf(ub, sizeof(ub), "未知(raw=%d)", pol);
            pn = ub;  hint = "❌ 非标准调度策略";
            break;
        }
        }
        printf("  调度策略       : %-14s 优先级 %-3d %s\n", pn, sp.sched_priority, hint);
        if (pol == SCHED_BATCH || pol == SCHED_IDLE) {
            printf("                   → 修：chrt -f 80 <程序>  （或 chrt -o 0 恢复普通）\n");
        }
    }

    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int i, first = -1, cnt = 0;
        for (i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, &set)) { if (first < 0) first = i; cnt++; }
        }
        printf("  CPU 亲和性     : %d 个核可用（首个 cpu%d）%s\n", cnt, first,
               (cnt < 4) ? "" : "  ⚠️ 用 taskset -c N 绑到隔离核可显著降抖动");
    }

    if (read_first_line("/proc/cmdline", buf, sizeof(buf))) {
        if (strstr(buf, "isolcpus=")) {
            const char *q = strstr(buf, "isolcpus=");
            char tmp[64]; int k = 0;
            q += 9;
            while (*q && *q != ' ' && k < 63) tmp[k++] = *q++;
            tmp[k] = '\0';
            printf("  isolcpus       : %-12s ✅ 已做 CPU 隔离\n", tmp);
        } else {
            printf("  isolcpus       : (未设置) ⚠️ 建议加 isolcpus=2,3 nohz_full=2,3\n");
        }
    }

    {
        double la[3] = {0, 0, 0};
        if (getloadavg(la, 3) == 3) {
            printf("  系统负载       : %.2f %.2f %.2f %s\n", la[0], la[1], la[2],
                   (la[0] < 0.5) ? "✅ 很闲" : "⚠️ 有负载，会推高抖动");
        }
    }
    printf("\n");
}

/* rt 的统计与打印 —— 从站模式和纯主机模式共用 */
static void rt_report(uint32_t *per, int n, int *buck,
                      uint32_t pmin, uint32_t pmax, uint64_t sum,
                      int misses, int bad, int catchup, uint64_t can_rx,
                      double real_s, int cycles, uint8_t ch, int host_only)
{
    static const char *bname[RT_MAX_BUCKETS] = {
        "< T-100us", "T-100~-10", "T±10us  ✅", "T+10~+50", "T+50~+100",
        "T+100~+500", "T+500~1ms", "+1~2ms", "> +2ms"
    };
    double freq = (double)cycles / real_s;
    int i;

    printf("── 拍周期分布（目标 1000 us）───────────────────────────────────\n");
    for (i = 0; i < RT_MAX_BUCKETS; i++) {
        double pct = 100.0 * buck[i] / (n ? n : 1);
        int bars = (int)(pct / 2.0);            /* 1 格 = 2% */
        int j;
        printf("  %-11s %7.3f%% │", bname[i], pct);
        for (j = 0; j < bars && j < 40; j++) putchar('#');
        putchar('\n');
    }

    printf("\n── 统计 ────────────────────────────────────────────────────────\n");
    if (n > 0) {
        double mean = (double)sum / n;
        uint32_t *srt = malloc(sizeof(uint32_t) * (size_t)n);
        /* 剔除「追赶拍」后再算 σ：绝对时间表在某一拍超时后，下一拍截止时刻
         * 已过，clock_nanosleep 立即返回，于是出现极短周期。这是调度器的
         * 正常行为（保证长期平均频率精确），但一个 0 值距均值 -999，
         * 会把 σ 完全带偏 —— 所以单独统计。 */
        double v_eff = 0.0;
        int n_eff = 0, k;

        if (srt) {
            memcpy(srt, per, sizeof(uint32_t) * (size_t)n);
            qsort(srt, (size_t)n, sizeof(uint32_t), cmp_u32);
        }
        for (k = 0; k < n; k++) {
            if (per[k] >= 500u) {          /* 非追赶拍 */
                double dd = (double)per[k] - mean;
                v_eff += dd * dd;
                n_eff++;
            }
        }
        if (n_eff > 0) v_eff /= n_eff;

        printf("  拍数           : %d\n", n);
        printf("  最小 / p50     : %u / %u us\n", pmin, srt ? srt[n / 2] : 0);
        printf("  p99 / p99.9    : %u / %u us\n",
               srt ? srt[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1] : 0,
               srt ? srt[(int)(n * 0.999) < n ? (int)(n * 0.999) : n - 1] : 0);
        printf("  最大           : %u us\n", pmax);
        printf("  平均           : %.1f us\n", mean);
        printf("  追赶拍         : %d 次（周期 < 500us，绝对时间表超时后补拍）\n", catchup);
        if (n_eff > 0) {
            printf("  有效抖动 σ     : %.1f us  ← 排除追赶拍，这才是真实抖动\n",
                   sqrt(v_eff));
        }
        free(srt);
    }
    printf("  实测时长       : %.2f s\n", real_s);
    printf("  实测频率       : %.2f Hz（目标 1000，漂移 %+.3f%%）\n",
           freq, (freq - 1000.0) / 10.0);

    if (host_only) {
        printf("  EtherCAT       : 未使用（纯主机模式）\n");
    } else if (can_rx > 0) {
        printf("  CAN 上行       : %llu 帧（%.1f 帧/秒）✅ 测试期间有数据在流\n",
               (unsigned long long)can_rx, (double)can_rx / real_s);
    } else {
        printf("  CAN 上行       : 0 帧 ⚠️ 通道 %u 上无设备（不影响实时性结论，\n"
               "                   但也说明这次没带真实 CAN 负载）\n", ch);
    }

    printf("\n── 实时性判据 ──────────────────────────────────────────────────\n");
    printf("  截止期错失     : %d 次（> 1100us，即目标 +10%%）\n", misses);
    printf("  严重超标       : %d 次（> 5000us）\n", bad);

    printf("\n  判决：");
    if (n == 0) {
        printf("无数据\n");
    } else if (misses == 0) {
        printf("✅ **优秀** —— %d 拍零错失，可直接用于 1 kHz 硬实时控制\n", n);
    } else if (100.0 * misses / n < 0.01) {
        printf("✅ **良好** —— 错失率 %.4f%%，< 0.01%%，一般控制环可接受\n",
               100.0 * misses / n);
    } else if (100.0 * misses / n < 1.0) {
        printf("⚠️ **勉强** —— 错失率 %.3f%%，对增益敏感的控制环会有影响；\n"
               "     建议做 CPU 隔离 / 提高 RT 优先级 / 关掉后台负载\n",
               100.0 * misses / n);
    } else {
        printf("❌ **不合格** —— 错失率 %.2f%%，无法用于硬实时；\n"
               "     按上面的「主机环境」逐项排查（governor → 隔离核 → RT 优先级）\n",
               100.0 * misses / n);
    }
}

static volatile uint32_t s_spin_sink;   /* 只为阻止编译器优化掉模拟负载 */

/* 纯主机实时性测试：不碰 EtherCAT、不需要任何硬件。
 * 用途：单独评估主机能不能跑住 1 kHz，把「主机抖动」和「链路延迟」分开。 */
static int cmd_rt_host(int seconds)
{
    uint32_t *per;
    int cycles, i, n = 0;
    int buck[RT_MAX_BUCKETS];
    uint32_t pmin = 0xFFFFFFFFu, pmax = 0;
    uint64_t sum = 0;
    int misses = 0, bad = 0, catchup = 0;
    struct timespec t_start, t_end, next, prev, cur;
    double real_s;

    if (seconds < 1) seconds = 10;
    if (seconds > 3600) seconds = 3600;
    cycles = seconds * 1000;

    per = malloc(sizeof(uint32_t) * (size_t)cycles);
    if (!per) { printf("内存不足\n"); return 1; }

    printf("RT（纯主机模式）：%d 秒 @ 1000 Hz（%d 拍）\n", seconds, cycles);
    printf("    不连 EtherCAT、不需要任何硬件 —— 只测主机本身的定时与调度能力\n\n");

    memset(buck, 0, sizeof(buck));
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    next = t_start;
    prev = t_start;

    for (i = 0; i < cycles && !g_stop; i++) {
        uint32_t d;

        /* 模拟一拍里除 EtherCAT 之外的开销（协议组装 + 少量计算） */
        volatile uint32_t spin = 0;
        int q;
        for (q = 0; q < 200; q++) spin += (uint32_t)q;
        s_spin_sink = spin;                    /* 防止被优化掉 */

        clock_gettime(CLOCK_MONOTONIC, &cur);
        d = (uint32_t)((cur.tv_sec - prev.tv_sec) * 1000000
                       + (cur.tv_nsec - prev.tv_nsec) / 1000);
        prev = cur;
        if (i > 0) {
            per[n++] = d;
            sum += d;
            if (d < pmin) pmin = d;
            if (d > pmax) pmax = d;
            if (d > 1100u) misses++;
            if (d > 5000u) bad++;
            if (d < 500u)  catchup++;
        }
        if (d < 900u)        buck[0]++;
        else if (d < 990u)   buck[1]++;
        else if (d <= 1010u) buck[2]++;
        else if (d <= 1050u) buck[3]++;
        else if (d <= 1100u) buck[4]++;
        else if (d <= 1500u) buck[5]++;
        else if (d <= 2000u) buck[6]++;
        else if (d <= 3000u) buck[7]++;
        else                 buck[8]++;

        tick_sleep(&next, 1000);
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    real_s = (double)(t_end.tv_sec - t_start.tv_sec)
           + (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    rt_report(per, n, buck, pmin, pmax, sum, misses, bad, catchup, 0,
              real_s, cycles, 0, 1);
    free(per);
    return 0;
}

static int cmd_rt(fieldbus_t *fb, int seconds, uint8_t ch)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    const ecat2can_tx_rx_message *in = FB_IN(fb, grp);
    uint32_t *per;
    int cycles, i, n = 0;
    int buck[RT_MAX_BUCKETS];
    uint32_t pmin = 0xFFFFFFFFu, pmax = 0;
    uint64_t sum = 0, can_rx = 0;
    int misses = 0, bad = 0, catchup = 0;
    struct timespec t_start, t_end, next, prev, cur;
    double real_s;
    uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    if (seconds < 1) seconds = 10;
    if (seconds > 3600) seconds = 3600;
    cycles = seconds * 1000;

    per = malloc(sizeof(uint32_t) * (size_t)cycles);
    if (!per) { printf("内存不足\n"); return 1; }

    printf("RT：实时性合格性测试 —— %d 秒 @ 1000 Hz（%d 拍）\n", seconds, cycles);
    printf("    通道 %u（MCAN%u），WORK_MODE 保持链路真实负载\n\n", ch, ch);

    memset(buck, 0, sizeof(buck));
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    next = t_start;
    prev = t_start;

    for (i = 0; i < cycles && !g_stop; i++) {
        uint32_t d;

        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = WORK_MODE;
        out->frame_count = 1;
        out->seq = (uint8_t)i;
        build_frame(out, 0, ch, 0x123, data, 8);

        fb_roundtrip(fb);

        clock_gettime(CLOCK_MONOTONIC, &cur);
        d = (uint32_t)((cur.tv_sec - prev.tv_sec) * 1000000
                       + (cur.tv_nsec - prev.tv_nsec) / 1000);
        prev = cur;
        if (i > 0) {
            per[n++] = d;
            sum += d;
            if (d < pmin) pmin = d;
            if (d > pmax) pmax = d;
            if (d > 1100u) misses++;
            if (d > 5000u) bad++;
            if (d < 500u)  catchup++;
        }

        {
            int q, nfr = (int)in->frame_count;
            if (nfr > (int)ECAT2CAN_MAX_FRAME_COUNT) nfr = (int)ECAT2CAN_MAX_FRAME_COUNT;
            for (q = 0; q < nfr; q++) { if (in->can_frames[q].std_id) can_rx++; }
        }

        if (d < 900u)        buck[0]++;
        else if (d < 990u)   buck[1]++;
        else if (d <= 1010u) buck[2]++;
        else if (d <= 1050u) buck[3]++;
        else if (d <= 1100u) buck[4]++;
        else if (d <= 1500u) buck[5]++;
        else if (d <= 2000u) buck[6]++;
        else if (d <= 3000u) buck[7]++;
        else                 buck[8]++;

        tick_sleep(&next, 1000);
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    real_s = (double)(t_end.tv_sec - t_start.tv_sec)
           + (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    rt_report(per, n, buck, pmin, pmax, sum, misses, bad, catchup, can_rx,
              real_s, cycles, ch, 0);
    free(per);
    return 0;
}

static int cmd_fdb(fieldbus_t *fb, int cycles)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    const ecat2can_tx_rx_message *in = FB_IN(fb, grp);
    int round, last_count = -1;

    printf("ONLY_FDB：只收 CAN 总线反馈，不下发（%d 次）\n", cycles);

    for (round = 0; round < cycles && !g_stop; round++) {
        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = ONLY_FDB;
        out->seq = (uint8_t)round;

        fb_roundtrip(fb);

        if (round < 5 || in->frame_count != last_count) {
            printf("第 %4d 轮  往返 %d us\n", round, fb->roundtrip_us);
            dump_input_pdo((const uint8_t *)in);
            last_count = in->frame_count;
        }
        osal_usleep(1000);
    }
    return 0;
}

static int cmd_reset(fieldbus_t *fb)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    int i;

    printf("HW_RESET：从站会调用看门狗强制复位（掉出 OP，约 100 ms 后重新启动）\n");

    memset(out, 0, ECAT2CAN_PDO_BYTES);
    out->command = HW_RESET;
    out->seq = 0x5A;

    for (i = 0; i < 10 && !g_stop; i++) {
        fb_roundtrip(fb);
        osal_usleep(1000);
    }
    printf("已下发。若从站已复位，控制台会重新打印 HPM 启动 LOGO 和 EEPROM loading successful。\n");
    return 0;
}

/* ===================== main ===================== */
static void usage(const char *prog)
{
    printf("用法: %s <网卡名> <命令> [参数...]\n"
           "\n"
           "  regs                    ★读 ESC 寄存器（DL_STAT / STATION_ADDR）判断链路与身份\n"
           "  info                    进 OP，打印从站信息并周期刷新（Ctrl+C 退出）\n"
           "  loop [次数]             LOOP_TEST，192 字节往返一致性校验（默认 1000）\n"
           "  scan [次数]             ★多通道扫描：只看不发，统计各路收帧数与出现过的 ID\n"
           "  work <通道> <id> [b0..b7]  WORK_MODE，在指定通道以 1 kHz 下发一帧 CAN\n"
           "  workall <id> [b0..b7]   ★在 4 个通道各发一帧（同一拍 PDO）\n"
           "  bench [次数] [通道]     ★性能基准：EtherCAT 往返 / 循环抖动 / CAN 吞吐\n"
           "  rt [秒数] [通道|host]   ★实时性合格性测试：直方图 / 截止期错失 / 环境诊断\n"
           "                         第二参数写 host = 纯主机模式，不需要任何硬件\n"
           "  bench_lat <通道> <can口> [次数] ★端到端延迟：主机→EtherCAT→网关→CAN总线→对端\n"
           "  looplat <tx> <rx> [秒数] [Hz] [负载帧数]  ★双 CAN 口回环实时性测试（两座子短接）\n"
           "                         默认 30 秒 @ 1000 Hz、无负载\n"
           "                         第 6 参数灌总线负载（模拟同总线还有其他电机）\n"
           "                         例：looplat 2 3              静默基线\n"
           "                             looplat 2 3 30 1000 4    4 帧/拍 ≈ 60%% 负载\n"
           "                             looplat 2 3 30 1000 8    8 帧/拍 ≈ 108%% 饱和\n"
           "  fdb  [次数]             ONLY_FDB，只读 CAN 反馈（默认 20）\n"
           "  reset                   HW_RESET，让从站复位\n"
           "\n"
           "  通道号 0~%u，对应从站的 MCAN0~MCAN%u。ECAN-Lite 四个座子分别是\n"
           "  CN4=0 / CN5=1 / CN9=2 / CN1=3；hpm5e3ykit 单路版只有通道 3。\n"
           "  也可用环境变量 ECAT2CAN_CH 设默认通道。\n"
           "\n"
           "示例:\n"
           "  %s enp3s0 scan 3000                        # 先扫一遍，看设备挂在哪一路\n"
           "  %s enp3s0 work 0 0x123 11 22 33 44 55 66 77 88\n"
           "  %s enp3s0 workall 0x1FF 03 E8 00 00 00 00 00 00\n"
           "  %s enp3s0 loop\n"
           "\n"
           "网卡准备:\n"
           "  sudo nmcli dev set <网卡名> managed no\n"
           "  sudo setcap cap_net_raw,cap_net_admin+eip %s\n",
           prog, ECAT2CAN_CAN_CHANNEL_MAX, ECAT2CAN_CAN_CHANNEL_MAX,
           prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    fieldbus_t fb;
    const char *cmd;
    uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint32_t id = 0x123;
    const char *can_iface = "can0";
    uint8_t tx_ch = 2, rx_ch = 3;
    int rt_host_only = 0;
    int lp_freq = 1000;
    /* looplat2：多从站同时回环，从站列表形如 "1,2" 或 "1-3" */
    int lp2_slaves[8];
    int lp2_nslave = 0;
    int lp_load = 0;
    int len = 8, cycles = -1, rc = 0;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    fb.iface = argv[1];
    cmd = argv[2];

    /* 可选：用 ECAT2CAN_CH=0..3 指定发送通道（ECAN-Lite 四路全引出时才需要） */
    {
        const char *ch = getenv("ECAT2CAN_CH");
        if (ch != NULL) {
            unsigned v = (unsigned)strtoul(ch, NULL, 0);
            if (v <= ECAT2CAN_CAN_CHANNEL_MAX) {
                g_can_channel = (uint8_t)v;
                printf("默认通道被 ECAT2CAN_CH 设为 %u（MCAN%u）\n", g_can_channel, g_can_channel);
            } else {
                printf("ECAT2CAN_CH=%s 超出 0..%u，忽略\n", ch, ECAT2CAN_CAN_CHANNEL_MAX);
            }
        }
    }

    /* 编译期已经断言过 sizeof，这里再把关键偏移打印一次，便于和从站对表 */
    if (getenv("ECAT2CAN_DUMP_LAYOUT") != NULL) {
        printf("sizeof(can_frame_struct)    = %zu (期望 11)\n", sizeof(can_frame_struct));
        printf("sizeof(ecat2can_tx_rx_message) = %zu (期望 192)\n",
               sizeof(ecat2can_tx_rx_message));
        printf("offsetof(can_frames)        = %zu (期望 5)\n",
               offsetof(ecat2can_tx_rx_message, can_frames));
        return 0;
    }

    if (strcmp(cmd, "work") == 0) {
        int i;
        /* 通道是必填的第一个参数：work <ch> <id> [b0..b7] */
        if (argc < 4) {
            printf("!! work 需要指定通道：work <通道 0..%u> <id> [b0..b7]\n"
                   "   例：work 0 0x123 11 22 33 44 55 66 77 88\n",
                   ECAT2CAN_CAN_CHANNEL_MAX);
            return 1;
        }
        {
            unsigned v = (unsigned)strtoul(argv[3], NULL, 0);
            if (v > ECAT2CAN_CAN_CHANNEL_MAX) {
                printf("!! 通道号 %u 超出范围 0..%u\n", v, ECAT2CAN_CAN_CHANNEL_MAX);
                return 1;
            }
            g_can_channel = (uint8_t)v;
        }
        if (argc >= 5) {
            id = (uint32_t)strtoul(argv[4], NULL, 0) & ECAT2CAN_STD_ID_MAX;
        }
        len = 0;
        for (i = 5; i < argc && len < 8; i++, len++) {
            data[len] = (uint8_t)strtoul(argv[i], NULL, 16);
        }
        if (len == 0) len = 8;
        cycles = 2000;
    } else if (strcmp(cmd, "workall") == 0) {
        int i;
        if (argc >= 4) {
            id = (uint32_t)strtoul(argv[3], NULL, 0) & ECAT2CAN_STD_ID_MAX;
        }
        len = 0;
        for (i = 4; i < argc && len < 8; i++, len++) {
            data[len] = (uint8_t)strtoul(argv[i], NULL, 16);
        }
        if (len == 0) len = 8;
        cycles = 2000;
    } else if (strcmp(cmd, "scan") == 0) {
        cycles = (argc >= 4) ? atoi(argv[3]) : 3000;
    } else if (strcmp(cmd, "looplat2") == 0) {
        unsigned a2, b2;
        char *p;
        if (argc < 5) {
            printf("!! looplat2 用法：looplat2 <从站列表> <发送通道> <接收通道> [秒数=30] [Hz=1000] [负载帧数=0]\n"
                   "   从站列表形如 1,2 或 1-3\n"
                   "   例：looplat2 1,2 0 2 30 1000   （1、2 号从站同时做 CAN0↔CAN2 回环）\n"
                   "   ⚠ 每个从站的 CAN 通道对都要各自短接好\n");
            return 1;
        }
        /* 解析从站列表："1,2,4" 或 "1-3" 或混合 */
        p = argv[3];
        while (*p != '\0' && lp2_nslave < 8) {
            int lo, hi, v;
            char *endp;
            lo = (int)strtol(p, &endp, 0);
            if (endp == p) break;                 /* 解析不动了，退出 */
            hi = lo;
            p = endp;
            if (*p == '-') {                      /* 区间 a-b */
                p++;
                hi = (int)strtol(p, &endp, 0);
                if (endp == p) break;
                p = endp;
            }
            if (lo > hi) { v = lo; lo = hi; hi = v; }
            for (v = lo; v <= hi && lp2_nslave < 8; v++) {
                if (v >= 1) lp2_slaves[lp2_nslave++] = v;
            }
            if (*p == ',' || *p == ' ') p++;
        }
        if (lp2_nslave == 0) {
            printf("!! 从站列表解析失败：%s（应形如 1,2 或 1-3）\n", argv[3]);
            return 1;
        }
        a2 = (unsigned)strtoul(argv[4], NULL, 0);
        b2 = (unsigned)strtoul(argv[5], NULL, 0);
        if (a2 > ECAT2CAN_CAN_CHANNEL_MAX || b2 > ECAT2CAN_CAN_CHANNEL_MAX) {
            printf("!! 通道号必须在 0..%u\n", ECAT2CAN_CAN_CHANNEL_MAX);
            return 1;
        }
        tx_ch = (uint8_t)a2;
        rx_ch = (uint8_t)b2;
        cycles   = (argc >= 7) ? atoi(argv[6]) : 30;
        lp_freq  = (argc >= 8) ? atoi(argv[7]) : 1000;
        lp_load  = (argc >= 9) ? atoi(argv[8]) : 0;
        if (lp_freq < 100 || lp_freq > 10000) {
            printf("!! 频率必须在 100 ~ 10000 Hz\n");
            return 1;
        }
    } else if (strcmp(cmd, "looplat") == 0) {
        unsigned a2, b2;
        if (argc < 5) {
            printf("!! looplat 用法：looplat <发送通道> <接收通道> [秒数=30] [Hz=1000] [负载帧数=0]\n"
                   "   前提：这两个座子已用 CANH/CANL 短接\n"
                   "   例：looplat 2 3 60   （通道 2 ↔ 3 回环，跑 60 秒）\n");
            return 1;
        }
        a2 = (unsigned)strtoul(argv[3], NULL, 0);
        b2 = (unsigned)strtoul(argv[4], NULL, 0);
        if (a2 > ECAT2CAN_CAN_CHANNEL_MAX || b2 > ECAT2CAN_CAN_CHANNEL_MAX) {
            printf("!! 通道号必须在 0..%u\n", ECAT2CAN_CAN_CHANNEL_MAX);
            return 1;
        }
        /*
         * 允许收发同通道 —— 从站若配成 MCAN 外部回环模式
         * （can_config.mode = mcan_mode_loopback_external），
         * 每一路会自己发、自己收，此时「同通道回环」是唯一能通的方式，
         * 可以用来在**没有收发器、没有接线**的情况下测延迟。
         *
         * 正常模式下同通道当然收不到（节点不收自己发的帧），
         * 那种情况会在跑完后报「一次都没收到」，提示里已说明。
         */
        tx_ch = (uint8_t)a2;
        rx_ch = (uint8_t)b2;
        cycles = (argc >= 6) ? atoi(argv[5]) : 30;          /* 秒数，默认 30 */
        lp_freq = (argc >= 7) ? atoi(argv[6]) : 1000;       /* 频率 Hz */
        lp_load = (argc >= 8) ? atoi(argv[7]) : 0;          /* 负载帧数 */
        if (lp_freq < 100 || lp_freq > 10000) {
            printf("!! 频率必须在 100 ~ 10000 Hz\n");
            return 1;
        }
    } else if (strcmp(cmd, "bench_lat") == 0) {
        /* bench_lat <通道> <can口> [次数] */
        unsigned v;
        if (argc < 5) {
            printf("!! bench_lat 用法：bench_lat <通道 0..%u> <can口，如 can0> [次数]\n",
                   ECAT2CAN_CAN_CHANNEL_MAX);
            return 1;
        }
        v = (unsigned)strtoul(argv[3], NULL, 0);
        if (v > ECAT2CAN_CAN_CHANNEL_MAX) {
            printf("!! 通道号 %u 超出范围 0..%u\n", v, ECAT2CAN_CAN_CHANNEL_MAX);
            return 1;
        }
        g_can_channel = (uint8_t)v;
        can_iface = argv[4];
        cycles = (argc >= 6) ? atoi(argv[5]) : 3000;
    } else if (strcmp(cmd, "rt") == 0) {
        cycles = (argc >= 4) ? atoi(argv[3]) : 10;
        if (argc >= 5) {
            if (strcmp(argv[4], "host") == 0) {
                rt_host_only = 1;          /* 纯主机模式，不需要 EtherCAT */
            } else {
                unsigned v = (unsigned)strtoul(argv[4], NULL, 0);
                if (v > ECAT2CAN_CAN_CHANNEL_MAX) {
                    printf("!! 通道号 %u 超出范围 0..%u（或写 host 用纯主机模式）\n",
                           v, ECAT2CAN_CAN_CHANNEL_MAX);
                    return 1;
                }
                g_can_channel = (uint8_t)v;
            }
        }
    } else if (strcmp(cmd, "bench") == 0) {
        cycles = (argc >= 4) ? atoi(argv[3]) : 5000;
        if (argc >= 5) {
            unsigned v = (unsigned)strtoul(argv[4], NULL, 0);
            if (v > ECAT2CAN_CAN_CHANNEL_MAX) {
                printf("!! 通道号 %u 超出范围 0..%u\n", v, ECAT2CAN_CAN_CHANNEL_MAX);
                return 1;
            }
            g_can_channel = (uint8_t)v;
        }
    } else if (strcmp(cmd, "loop") == 0) {
        cycles = (argc >= 4) ? atoi(argv[3]) : 1000;
    } else if (strcmp(cmd, "fdb") == 0) {
        cycles = (argc >= 4) ? atoi(argv[3]) : 20;
    } else if (strcmp(cmd, "regs") == 0) {
        cycles = 0;
    } else if (strcmp(cmd, "info") == 0) {
        cycles = 0;
    } else if (strcmp(cmd, "reset") == 0) {
        cycles = 0;
    } else {
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /* 实时性测试：环境诊断不需要 EtherCAT，先打印 —— 连不上从站时更需要它 */
    if (strcmp(cmd, "rt") == 0) {
        rt_print_env();
        if (rt_host_only) {
            rc = cmd_rt_host(cycles);
            return rc;                     /* 纯主机模式，不进 EtherCAT */
        }
    }

    if (!fb_start(&fb)) {
        return 2;
    }

    if (strcmp(cmd, "loop") == 0) {
        rc = cmd_loop(&fb, cycles);
    } else if (strcmp(cmd, "scan") == 0) {
        rc = cmd_scan(&fb, cycles);
    } else if (strcmp(cmd, "rt") == 0) {
        cycles = (argc >= 4) ? atoi(argv[3]) : 10;
        if (argc >= 5) {
            unsigned v = (unsigned)strtoul(argv[4], NULL, 0);
            if (v > ECAT2CAN_CAN_CHANNEL_MAX) {
                printf("!! 通道号 %u 超出范围 0..%u\n", v, ECAT2CAN_CAN_CHANNEL_MAX);
                return 1;
            }
            g_can_channel = (uint8_t)v;
        }
    } else if (strcmp(cmd, "rt") == 0) {
        rc = cmd_rt(&fb, cycles, g_can_channel);
    } else if (strcmp(cmd, "bench") == 0) {
        rc = cmd_bench(&fb, cycles, g_can_channel);
    } else if (strcmp(cmd, "bench_lat") == 0) {
        rc = cmd_bench_lat(&fb, can_iface, g_can_channel, id, cycles);
    } else if (strcmp(cmd, "looplat") == 0) {
        rc = cmd_looplat(&fb, tx_ch, rx_ch, cycles, lp_freq, lp_load);
    } else if (strcmp(cmd, "looplat2") == 0) {
        int q, bad = 0;
        for (q = 0; q < lp2_nslave; q++) {
            if (lp2_slaves[q] > fb.ctx.slavecount) {
                printf("!! 从站 #%d 不存在（当前只扫到 %d 个）\n",
                       lp2_slaves[q], fb.ctx.slavecount);
                bad = 1;
            }
        }
        if (bad) { rc = 1; }
        else {
            rc = cmd_looplat_multi(&fb, lp2_slaves, lp2_nslave, tx_ch, rx_ch,
                                   cycles, lp_freq, lp_load);
        }
    } else if (strcmp(cmd, "work") == 0) {
        rc = cmd_work(&fb, g_can_channel, id, data, len, cycles);
    } else if (strcmp(cmd, "workall") == 0) {
        rc = cmd_workall(&fb, id, data, len, cycles);
    } else if (strcmp(cmd, "fdb") == 0) {
        rc = cmd_fdb(&fb, cycles);
    } else if (strcmp(cmd, "reset") == 0) {
        rc = cmd_reset(&fb);
    } else if (strcmp(cmd, "regs") == 0) {
        rc = cmd_regs(&fb);
    } else if (strcmp(cmd, "info") == 0) {
        rc = cmd_info(&fb);
    }

    fb_stop(&fb);
    return rc;
}
