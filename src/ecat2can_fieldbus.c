#include "ecat2can_fieldbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t g_stop = 0;

void fb_request_stop(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

/* 统一释放一次 SOEM 初始化周期的互斥锁和网卡资源。 */
static void fb_release(fieldbus_t *fb)
{
    ecx_contextt *ctx = &fb->ctx;

    if (!fb->context_ready) return;

    if (fb->operational) {
        for (int i = 1; i <= ctx->slavecount; i++) {
            ecx_dcsync0(ctx, i, FALSE, 0, 0);
        }
    }
    if (ctx->slavecount > 0) {
        ctx->slavelist->state = EC_STATE_INIT;
        ecx_writestate(ctx, 0);
    }
    ecx_close(ctx);
    fb->context_ready = 0;
    fb->operational = 0;
    memset(ctx, 0, sizeof(*ctx));
}

int fb_roundtrip(fieldbus_t *fb)
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

/* ===================== 从站上线 ===================== */
int fb_start(fieldbus_t *fb)
{
    ecx_contextt *ctx = &fb->ctx;
    ec_groupt *grp;
    ec_slavet *slave;
    int i;
    const char *iface = fb->iface;   /* memset 会把它清掉，先存下来 */

    memset(fb, 0, sizeof(*fb));
    fb->iface = iface;

    /* ecx_init 创建 mailbox pool 锁，即使网卡打开失败也要走 ecx_close。 */
    fb->context_ready = 1;
    if (!ecx_init(ctx, fb->iface)) {
        printf("!! 打不开网卡 %s（需要 CAP_NET_RAW，或是网卡名写错了）\n", fb->iface);
        printf("   修: sudo setcap cap_net_raw,cap_net_admin+eip ./ecat2can_master\n");
        fb_release(fb);
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
     * 所以这里在没有发现从站时重试。首次发现后保留该次完整上下文，
     * 不再用之前扫描的数量覆盖当前 slavelist。
     * 次数可用 ECAT2CAN_SCAN_RETRY 覆盖（默认 6，每次间隔 400 ms）。
     */
    {
        int attempt, last_count = 0, last_attempt = 0;
        int retry = 6;
        const char *e = getenv("ECAT2CAN_SCAN_RETRY");
        if (e != NULL) {
            int v = atoi(e);
            if (v >= 1 && v <= 50) retry = v;
        }

        for (attempt = 1; attempt <= retry; attempt++) {
            int n = ecx_config_init(ctx);
            /* ecx_config_init 会重建 SOEM 上下文；后续操作必须使用最后一次
             * 扫描的 slavelist，而不能把早先的最大数量写回 slavecount。 */
            last_count = n;
            last_attempt = attempt;
            if (n > 0) break;                 /* 保留这次完整初始化得到的上下文 */
            if (attempt < retry) {
                /* 每轮 config_init 都创建队列锁；关掉并重开 SOEM，避免覆盖锁指针。 */
                fb_release(fb);
                osal_usleep(400000);
                fb->context_ready = 1;
                if (!ecx_init(ctx, fb->iface)) {
                    printf("!! 扫描重试时重新打开网卡 %s 失败\n", fb->iface);
                    fb_release(fb);
                    break;
                }
            }
        }
        if (last_attempt > 1 && last_count > 0) {
            printf("扫描重试：第 %d 次才扫到 %d 个从站（共试 %d 次）\n",
                   last_attempt, last_count, retry);
        }
        if (last_count <= 0) {
            printf("!! 重试 %d 次都没扫到从站。\n", retry);
        }
    }

    if (ctx->slavecount <= 0) {
        printf("!! 没扫到从站。依次检查：\n"
               "   1) 网线插在板子的 EtherCAT 口上，且是 Port0 那个（本板 Port1 不支持）\n"
               "   2) 板子已上电且固件在跑（串口应打印 EEPROM loading successful）\n"
               "   3) 网卡有链路：ip -br link show %s 应显示 LOWER_UP\n",
               fb->iface);
        fb_release(fb);
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
        fb_release(fb);
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
    /*
     * ★ 重试若干次，而不是一次 ecx_statecheck 定生死。
     *
     * 实测：ecx_statecheck() 返回时 slavelist 里的 state 有时还没更新到最新
     * （从站其实已经到了 SAFE-OP，但本地结构体还是旧的 PRE-OP），紧接着
     * 调用 ecx_readstate() 才能看到真实的 0x04。原来只有一次判断，于是把
     * 「已经正常起来」的从站误判成"未进入 SAFE-OP"直接放弃。
     *
     * 现在：statecheck 之后补一次 readstate 确认；仍不满足就再等一小会儿
     * 重来，最多 10 轮（每轮约 EC_TIMEOUTSTATE/10）。
     */
    {
        int attempt;
        for (attempt = 0; attempt < 10; attempt++) {
            ecx_statecheck(ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE / 10);
            if (slave->state == EC_STATE_SAFE_OP) break;
            ecx_readstate(ctx);              /* 拉一次真实状态 */
            if (slave->state == EC_STATE_SAFE_OP) break;
            fb_roundtrip(fb);                /* 让从站再拿到一拍有效输出 */
            osal_usleep(1000);
        }
    }
    printf("完成（当前 0x%02X）\n", slave->state);
    if (slave->state != EC_STATE_SAFE_OP) {
        int k;
        printf("!! 从站未进入 SAFE-OP，停止启动并清理现场总线\n");
        /*
         * ★ 关键诊断：把每个从站的 AL_STATUS 和 AL_STATUS_CODE 打出来。
         *   PRE-OP → SAFE-OP 失败几乎都是 SM / FMMU 配置问题，具体是哪一条
         *   由 AL_STATUS_CODE 指明（例如 0x0016 邮箱配置、0x001D/0x001E 输入
         *   输出配置、0x0024 FMMU 配置）。原来这里只调了 ecx_readstate()
         *   却什么都不打印，等于把最有用的信息扔掉了。
         */
        ecx_readstate(ctx);
        for (k = 1; k <= ctx->slavecount; k++) {
            ec_slavet *s = ctx->slavelist + k;
            printf("   从站 %d: state=0x%04X  AL_CODE=0x%04X  (%s)\n",
                   k, s->state, (unsigned)s->ALstatuscode,
                   (char *)ec_ALstatuscode2string((uint16_t)s->ALstatuscode));
        }
        fb_release(fb);
        return 0;
    }

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
            fb->operational = 1;
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
    fb_release(fb);
    return 0;
}

void fb_stop(fieldbus_t *fb)
{
    if (!fb->context_ready) return;
    fb_release(fb);
    printf("已请求 INIT 状态并关闭网卡\n");
}
