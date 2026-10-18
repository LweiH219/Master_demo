#define _GNU_SOURCE
#include "ecat2can_rt.h"

#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static void rt_build_frame(ecat2can_tx_rx_message *m, uint8_t ch,
                           uint32_t id, const uint8_t *data, int len)
{
    can_frame_struct *f = &m->can_frames[0];
    f->can_channel = ch;
    f->std_id = id & ECAT2CAN_STD_ID_MAX;
    f->dlc = (uint16_t)len;
    memcpy(f->data, data, (size_t)len);
}

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
void rt_print_env(void)
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
int cmd_rt_host(int seconds)
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

int cmd_rt(fieldbus_t *fb, int seconds, uint8_t ch)
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
        rt_build_frame(out, ch, 0x123, data, 8);

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
