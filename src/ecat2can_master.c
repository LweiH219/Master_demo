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
} fieldbus_t;

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

    if (ecx_config_init(ctx) <= 0) {
        printf("!! 没扫到从站。依次检查：\n"
               "   1) 网线插在板子的 EtherCAT 口上，且是 Port0 那个（本板 Port1 不支持）\n"
               "   2) 板子已上电且固件在跑（串口应打印 EEPROM loading successful）\n"
               "   3) 网卡有链路：ip -br link show %s 应显示 LOWER_UP\n",
               fb->iface);
        return 0;
    }
    printf("扫到 %d 个从站\n", ctx->slavecount);

    ecx_config_map_group(ctx, fb->map, 0);
    grp = ctx->grouplist + 0;
    printf("过程数据映射：下行 %d 字节 / 上行 %d 字节\n", grp->Obytes, grp->Ibytes);

    if (grp->Obytes != ECAT2CAN_PDO_BYTES || grp->Ibytes != ECAT2CAN_PDO_BYTES) {
        printf("!! 期望 192/192，实际 %d/%d。请核对从站 SSC/ESI 与从站固件是否配套。\n",
               grp->Obytes, grp->Ibytes);
        return 0;
    }

    slave = ctx->slavelist + 1;
    printf("从站：%s  Man=0x%08X ID=0x%08X Rev=0x%08X\n",
           slave->name, slave->eep_man, slave->eep_id, slave->eep_rev);

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
            /* 1 ms 分布式时钟同步，和港科大实装时的 1 kHz 控制周期一致 */
            ecx_dcsync0(ctx, 1, TRUE, 1000000, 0);
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

    ecx_dcsync0(ctx, 1, FALSE, 0, 0);
    ctx->slavelist->state = EC_STATE_INIT;
    ecx_writestate(ctx, 0);
    ecx_close(ctx);
    printf("已请求 INIT 状态并关闭网卡\n");
}

/* ===================== 各命令 ===================== */
static int cmd_info(fieldbus_t *fb)
{
    int tick = 0;

    while (!g_stop) {
        ec_groupt *grp = fb->ctx.grouplist + 0;
        int expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;
        int wkc = fb_roundtrip(fb);

        /* slavelist[1].state 只有 readstate/statecheck 才会刷新，定期读一次 */
        if (++tick % 20 == 0) {
            ecx_readstate(&fb->ctx);
        }
        printf("往返 %3d us  WKC %d/%d  从站状态 0x%02X%s     \r",
               fb->roundtrip_us, wkc, expected_wkc,
               fb->ctx.slavelist[1].state,
               (fb->ctx.slavelist[1].state == EC_STATE_OPERATIONAL) ? " (OP)" : "");
        fflush(stdout);
        osal_usleep(100000);
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
        uint8_t *out = grp->outputs;
        uint8_t *in = grp->inputs;
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

        /* 本轮的上行应该等于上一轮的下行（第 0 轮没有上一轮，跳过） */
        if (round > 0) {
            compared++;
            if (memcmp(prev, in, ECAT2CAN_PDO_BYTES) != 0) {
                int first = -1;
                for (i = 0; i < (int)ECAT2CAN_PDO_BYTES; i++) {
                    if (prev[i] != in[i]) { first = i; break; }
                }
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
    ecat2can_tx_rx_message *out = (ecat2can_tx_rx_message *)grp->outputs;
    const ecat2can_tx_rx_message *in = (const ecat2can_tx_rx_message *)grp->inputs;
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
            dump_input_pdo(grp->inputs);
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
    ecat2can_tx_rx_message *out = (ecat2can_tx_rx_message *)grp->outputs;
    const ecat2can_tx_rx_message *in = (const ecat2can_tx_rx_message *)grp->inputs;
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
    ecat2can_tx_rx_message *out = (ecat2can_tx_rx_message *)grp->outputs;
    const ecat2can_tx_rx_message *in = (const ecat2can_tx_rx_message *)grp->inputs;
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
    ecat2can_tx_rx_message *out = (ecat2can_tx_rx_message *)grp->outputs;
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

/* ===================== looplat：双通道硬件回环延迟 ===================== *
 *
 * 前提：把两个 CAN 座子用两根线短接（CANH↔CANH、CANL↔CANL），
 *       例如 ECAN-Lite 的 CN9(通道 2) ↔ CN1(通道 3)。
 *
 * 测量的回路：
 *     主机 → EtherCAT → MCAN(tx) → 收发器 → 线 → 收发器 → MCAN(rx)
 *          → EtherCAT → 主机
 *
 * 全程经过**两个真实收发器和一段真实 CAN 总线**，不含 USB / 外部设备，
 * 是最接近实际控制回路的延迟。
 *
 * 精度说明：主机侧靠"尽快轮询 EtherCAT"来逼近到达时刻，所以分辨率
 * 受限于一次 EtherCAT 往返（约 60~70us）；而从站的收发是在 DC 同步
 * 中断里做的（1 kHz），所以延迟主要由**落在 DC 周期的哪个相位**决定。
 */
static int cmd_looplat(fieldbus_t *fb, uint8_t tx_ch, uint8_t rx_ch, int count)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = (ecat2can_tx_rx_message *)grp->outputs;
    const ecat2can_tx_rx_message *in = (const ecat2can_tx_rx_message *)grp->inputs;
    uint32_t *lat;
    uint8_t data[8];
    int i, n = 0, timeouts = 0;

    if (count < 10) count = 10;
    lat = malloc(sizeof(uint32_t) * (size_t)count);
    if (!lat) return 1;

    printf("LOOPLAT：通道 %u → 线 → 通道 %u，%d 次\n", tx_ch, rx_ch, count);
    printf("  前提：这两个座子已用 CANH/CANL 两根线短接\n\n");

    for (i = 0; i < count && !g_stop; i++) {
        struct timespec t0, t1, ts;
        uint64_t a, b;
        int guard, got = 0;
        uint8_t marker_hi = (uint8_t)(i >> 8);
        uint8_t marker_lo = (uint8_t)(i & 0xFF);

        data[0] = marker_hi; data[1] = marker_lo;
        data[2] = 0xC3; data[3] = 0x3C;
        data[4] = data[5] = data[6] = data[7] = 0;

        /* ---- 第 1 拍：把帧发到 tx 通道 ---- */
        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = WORK_MODE;
        out->frame_count = 1;
        out->seq = (uint8_t)i;            /* seq 变化 → 从站会真的发这一帧 */
        build_frame(out, 0, tx_ch, 0x321, data, 8);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        fb_roundtrip(fb);

        /* ---- 后续拍：seq 不变、frame_count=0，只读不回发，尽快轮询 ---- */
        for (guard = 0; guard < 200; guard++) {
            int k, nfr;

            memset(out, 0, ECAT2CAN_PDO_BYTES);
            out->command = WORK_MODE;
            out->frame_count = 0;         /* ★ 不再下发，避免重复发送 */
            out->seq = (uint8_t)i;        /* ★ seq 不变 → 从站不会重发 */

            fb_roundtrip(fb);
            clock_gettime(CLOCK_MONOTONIC, &ts);

            nfr = (int)in->frame_count;
            if (nfr > (int)ECAT2CAN_MAX_FRAME_COUNT) nfr = (int)ECAT2CAN_MAX_FRAME_COUNT;
            for (k = 0; k < nfr; k++) {
                const can_frame_struct *f = &in->can_frames[k];
                if (f->can_channel == rx_ch && f->std_id == 0x321 &&
                    f->data[0] == marker_hi && f->data[1] == marker_lo) {
                    got = 1;
                    break;
                }
            }
            if (got) break;
            osal_usleep(50);              /* 轻微让出，避免把 CPU 跑满 */
        }

        if (got) {
            a = (uint64_t)t0.tv_sec * 1000000ull + (uint64_t)t0.tv_nsec / 1000ull;
            b = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
            lat[n++] = (uint32_t)(b - a);
        } else {
            timeouts++;
        }

        /* 每拍之间留出一点间隔，让上一次的回环帧彻底散掉 */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        { struct timespec nx = t1; tick_sleep(&nx, 2000); }
    }

    printf("── 双通道硬件回环延迟（主机发出 → 回到主机）──\n");
    if (n > 0) {
        print_stats("回环往返", lat, n, "us");
        printf("\n  说明：这条回路含 2 次 EtherCAT 转发 + 2 个收发器 + 总线仲裁，\n"
               "        且从站转发发生在 1 kHz DC 同步中断里 —— 所以数值会明显\n"
               "        大于纯 EtherCAT 往返，差额就是 CAN 侧与 DC 相位的代价。\n");
    } else {
        printf("  (没收到回环帧 —— 确认 %u 和 %u 两个座子真的短接了，\n"
               "   且接的是 CANH↔CANH、CANL↔CANL)\n", tx_ch, rx_ch);
    }
    if (timeouts) printf("  超时 %d 次\n", timeouts);

    free(lat);
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
    ecat2can_tx_rx_message *out = (ecat2can_tx_rx_message *)grp->outputs;
    const ecat2can_tx_rx_message *in = (const ecat2can_tx_rx_message *)grp->inputs;
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
    ecat2can_tx_rx_message *out = (ecat2can_tx_rx_message *)grp->outputs;
    const ecat2can_tx_rx_message *in = (const ecat2can_tx_rx_message *)grp->inputs;
    int round, last_count = -1;

    printf("ONLY_FDB：只收 CAN 总线反馈，不下发（%d 次）\n", cycles);

    for (round = 0; round < cycles && !g_stop; round++) {
        memset(out, 0, ECAT2CAN_PDO_BYTES);
        out->command = ONLY_FDB;
        out->seq = (uint8_t)round;

        fb_roundtrip(fb);

        if (round < 5 || in->frame_count != last_count) {
            printf("第 %4d 轮  往返 %d us\n", round, fb->roundtrip_us);
            dump_input_pdo(grp->inputs);
            last_count = in->frame_count;
        }
        osal_usleep(1000);
    }
    return 0;
}

static int cmd_reset(fieldbus_t *fb)
{
    ec_groupt *grp = fb->ctx.grouplist + 0;
    ecat2can_tx_rx_message *out = (ecat2can_tx_rx_message *)grp->outputs;
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
           "  info                    进 OP，打印从站信息并周期刷新（Ctrl+C 退出）\n"
           "  loop [次数]             LOOP_TEST，192 字节往返一致性校验（默认 1000）\n"
           "  scan [次数]             ★多通道扫描：只看不发，统计各路收帧数与出现过的 ID\n"
           "  work <通道> <id> [b0..b7]  WORK_MODE，在指定通道以 1 kHz 下发一帧 CAN\n"
           "  workall <id> [b0..b7]   ★在 4 个通道各发一帧（同一拍 PDO）\n"
           "  bench [次数] [通道]     ★性能基准：EtherCAT 往返 / 循环抖动 / CAN 吞吐\n"
           "  rt [秒数] [通道|host]   ★实时性合格性测试：直方图 / 截止期错失 / 环境诊断\n"
           "                         第二参数写 host = 纯主机模式，不需要任何硬件\n"
           "  bench_lat <通道> <can口> [次数] ★端到端延迟：主机→EtherCAT→网关→CAN总线→对端\n"
           "  looplat <tx通道> <rx通道> [次数] ★双通道硬件回环延迟（两个座子短接后测）\n"
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
    } else if (strcmp(cmd, "looplat") == 0) {
        unsigned a2, b2;
        if (argc < 5) {
            printf("!! looplat 用法：looplat <发送通道> <接收通道> [次数]\n"
                   "   前提：这两个座子已用 CANH/CANL 短接\n");
            return 1;
        }
        a2 = (unsigned)strtoul(argv[3], NULL, 0);
        b2 = (unsigned)strtoul(argv[4], NULL, 0);
        if (a2 > ECAT2CAN_CAN_CHANNEL_MAX || b2 > ECAT2CAN_CAN_CHANNEL_MAX) {
            printf("!! 通道号必须在 0..%u\n", ECAT2CAN_CAN_CHANNEL_MAX);
            return 1;
        }
        if (a2 == b2) { printf("!! 收发通道不能相同\n"); return 1; }
        tx_ch = (uint8_t)a2;
        rx_ch = (uint8_t)b2;
        cycles = (argc >= 6) ? atoi(argv[5]) : 200;
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
        rc = cmd_looplat(&fb, tx_ch, rx_ch, cycles);
    } else if (strcmp(cmd, "work") == 0) {
        rc = cmd_work(&fb, g_can_channel, id, data, len, cycles);
    } else if (strcmp(cmd, "workall") == 0) {
        rc = cmd_workall(&fb, id, data, len, cycles);
    } else if (strcmp(cmd, "fdb") == 0) {
        rc = cmd_fdb(&fb, cycles);
    } else if (strcmp(cmd, "reset") == 0) {
        rc = cmd_reset(&fb);
    } else {
        rc = cmd_info(&fb);
    }

    fb_stop(&fb);
    return rc;
}
