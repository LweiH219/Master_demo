/* Included by ecat2can_master.c after its fieldbus helpers. */
#include <errno.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>

enum { PROBE_OK, PROBE_LATE, PROBE_NO_ECHO, PROBE_SKIP, PROBE_BAD_WKC };
typedef struct {
    int64_t release, begin, sent, matched, late_match, first_ecat;
    uint32_t load_mask, exchanges, bad_wkc;
    int cpu, status;
} probe_row;

static int64_t probe_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static uint32_t probe_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void probe_payload(uint8_t *p, uint32_t seq, uint32_t token)
{
    for (int k = 0; k < 4; ++k) {
        p[k] = (uint8_t)(seq >> (8*k));
        p[k+4] = (uint8_t)(token >> (8*k));
    }
}

/* Count only full-WKC PDOs and full 8-byte, per-run token/sequence matches.
 * An expired sample can subsequently acquire late_match, without being
 * relabelled as an on-time success. Background frames are deduplicated across
 * ALL later polls using the original sequence, not just within one cycle. */
static void probe_observe(const ecat2can_tx_rx_message *in, probe_row *rows,
                          int current, uint8_t rx, uint32_t token, int load,
                          int64_t now)
{
    int nf = in->frame_count;
    if (nf > (int)ECAT2CAN_MAX_FRAME_COUNT) nf = ECAT2CAN_MAX_FRAME_COUNT;
    for (int k = 0; k < nf; ++k) {
        const can_frame_struct *f = &in->can_frames[k];
        if (f->can_channel != rx || f->dlc != 8 ||
            probe_u32(f->data+4) != token) continue;
        uint32_t seq = probe_u32(f->data);
        if (seq == 0 || seq > (uint32_t)current+1) continue;
        probe_row *r = &rows[seq-1];
        if (!r->sent) continue;
        if (f->std_id == 0x600) {
            if ((int)seq-1 == current) {
                if (!r->matched) r->matched = now;
            } else if (!r->matched && !r->late_match) {
                r->late_match = now;
            }
        } else if (f->std_id > 0x600 && f->std_id <= 0x600+(uint32_t)load) {
            r->load_mask |= 1u << (f->std_id-0x601);
        }
    }
}

static int cmd_probe(fieldbus_t *fb, int seconds, uint8_t tx, uint8_t rx,
                     int hz, int load)
{
    ec_groupt *grp = fb->ctx.grouplist;
    ecat2can_tx_rx_message *out = FB_OUT(fb, grp);
    const ecat2can_tx_rx_message *in = FB_IN(fb, grp);
    const int ecat_only = tx == 255;
    const int expected = grp->outputsWKC*2 + grp->inputsWKC;
    const int count = seconds*hz;
    const int64_t period = 1000000000LL/hz;
    const char *path = getenv("ECAT2CAN_PROBE_CSV");
    probe_row *rows = calloc((size_t)count, sizeof(*rows));
    FILE *csv = path ? fopen(path, "w") : NULL;
    if (!rows || !csv) {
        perror("probe allocation / ECAT2CAN_PROBE_CSV");
        free(rows); if (csv) fclose(csv); return 2;
    }
    uint32_t token;
    if (getrandom(&token, sizeof(token), 0) != sizeof(token)) {
        perror("getrandom"); fclose(csv); free(rows); return 2;
    }
    printf("PROBE slave=%d tx=%s rx=%u Hz=%d seconds=%d load=%d id=0x600 token=%08x expected_WKC=%d\n",
           fb->slave_idx, ecat_only ? "ecat-only" : (tx == 0 ? "0" : tx == 1 ? "1" : tx == 2 ? "2" : "3"),
           rx, hz, seconds, load, token, expected);
    printf("CAN mode measures physical loop RTT, not RTT/2. No-echo is NOT proof of physical CAN loss.\n");
    printf("policy=%d priority=", sched_getscheduler(0));
    struct sched_param param;
    sched_getparam(0,&param); printf("%d CSV=%s\n",param.sched_priority,path);
    fflush(stdout);
    int wkc = 0;
    memset(grp->outputs,0,grp->Obytes);
    for (int k=1;k<=fb->slavecount;++k) FB_OUT_N(fb,grp,k)->command=ONLY_FDB;
    for (int j=0;j<200 && !g_stop;++j) {
        wkc=fb_roundtrip(fb); osal_usleep(1000);
    }
    if (wkc != expected || g_stop) {
        fprintf(stderr,"probe warmup incomplete: WKC=%d/%d stop=%d\n",wkc,expected,g_stop);
        fclose(csv);free(rows);return 2;
    }
    /* Prefault recorded samples and stack before the timed interval. */
    for (size_t k=0;k<(size_t)count*sizeof(*rows);k+=4096)
        ((volatile unsigned char *)rows)[k]=0;
    volatile unsigned char stack_reserve[65536];
    for (size_t k=0;k<sizeof(stack_reserve);k+=4096) stack_reserve[k]=0;
    if (mlockall(MCL_CURRENT|MCL_FUTURE)) {
        perror("mlockall (run with sufficient locked-memory limit)");
        fclose(csv);free(rows);return 2;
    }
    struct rusage before,after;
    getrusage(RUSAGE_SELF,&before);
    int64_t base=probe_now()+10*period;
    int done=0;
    int error=0;
    for (int i=0;i<count && !g_stop;++i) {
        probe_row *r=&rows[i];
        r->release=base+(int64_t)i*period;
        struct timespec ts={r->release/1000000000LL,r->release%1000000000LL};
        int rc;
        do { rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,NULL); }
        while (rc==EINTR && !g_stop);
        if(g_stop)break;
        if(rc){errno=rc;perror("clock_nanosleep");error=1;break;}
        r->begin=probe_now(); r->cpu=sched_getcpu();
        int64_t deadline=r->release+period;
        done=i+1;
        if(r->begin>=deadline){r->status=PROBE_SKIP;continue;}
        memset(out,0,ECAT2CAN_PDO_BYTES);
        out->command=ecat_only?ONLY_FDB:WORK_MODE;
        out->seq=(uint8_t)(i+1);
        if(!ecat_only){
            uint8_t data[8];probe_payload(data,(uint32_t)i+1,token);
            out->frame_count=(uint8_t)(1+load);
            for(int k=0;k<=load;++k)build_frame(out,k,tx,0x600u+(unsigned)k,data,8);
        }
        r->status=PROBE_NO_ECHO;
        r->sent=probe_now();
        while(!g_stop){
            int64_t left=deadline-probe_now();
            if(left<=0)break;
            int timeout=(int)((left+999)/1000);
            if(timeout>EC_TIMEOUTRET)timeout=EC_TIMEOUTRET;
            int64_t exchange_start=probe_now();
            ecx_send_processdata(&fb->ctx);
            wkc=ecx_receive_processdata(&fb->ctx,timeout);
            int64_t now=probe_now();
            ++r->exchanges;
            if(r->exchanges==1)r->first_ecat=now-exchange_start;
            if(wkc!=expected){
                ++r->bad_wkc;
                if(ecat_only){r->status=PROBE_BAD_WKC;break;}
            }else if(ecat_only){
                r->matched=now;r->status=now<=deadline?PROBE_OK:PROBE_LATE;break;
            }else{
                probe_observe(in,rows,i,rx,token,load,now);
                if(r->matched){r->status=now<=deadline?PROBE_OK:PROBE_LATE;break;}
            }
            out->frame_count=0; /* Same seq, no retransmission. */
        }
    }
    int64_t measured_end=probe_now();
    getrusage(RUSAGE_SELF,&after);
    /* Drain old frames for 20ms, without sending any new CAN message. */
    if(!ecat_only && done>0 && !g_stop){
        out->frame_count=0;
        int64_t end=probe_now()+20000000;
        while(probe_now()<end){
            wkc=fb_roundtrip(fb);
            if(wkc==expected)probe_observe(in,rows,done-1,rx,token,load,probe_now());
        }
    }
    munlockall();
    fprintf(csv,"seq,release_us,wakeup_us,cpu,status,send_us,rtt_us,response_us,first_ecat_us,exchanges,bad_wkc,load_received,late_rtt_us\n");
    int good=0,late=0,noecho=0,skipped=0,badframes=0,late_seen=0;
    uint64_t bad_exchanges=0,total_exchanges=0,background=0;
    for(int i=0;i<done;++i){
        probe_row *r=&rows[i];
        good+=r->status==PROBE_OK;late+=r->status==PROBE_LATE;
        noecho+=r->status==PROBE_NO_ECHO;skipped+=r->status==PROBE_SKIP;
        badframes+=r->status==PROBE_BAD_WKC;
        late_seen+=r->late_match!=0;
        bad_exchanges+=r->bad_wkc;total_exchanges+=r->exchanges;
        unsigned received=(unsigned)__builtin_popcount(r->load_mask);
        background+=received;
        fprintf(csv,"%d,%.3f,%.3f,%d,%d,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%.3f\n",
            i+1,(r->release-base)/1000.0,(r->begin-r->release)/1000.0,r->cpu,r->status,
            r->sent?(r->sent-base)/1000.0:-1.,r->matched?(r->matched-r->sent)/1000.0:-1.,
            r->matched?(r->matched-r->release)/1000.0:-1.,r->first_ecat/1000.0,
            r->exchanges,r->bad_wkc,received,r->late_match?(r->late_match-r->sent)/1000.0:-1.);
    }
    int write_error=ferror(csv);
    if(fclose(csv))write_error=1;
    printf("RESULT scheduled=%d completed=%d on_time=%d matched_late=%d no_echo_in_window=%d release_skipped=%d bad_wkc_cycles=%d subsequently_seen_late=%d\n",
           count,done,good,late,noecho,skipped,badframes,late_seen);
    printf("exchanges=%"PRIu64" bad_wkc_exchanges=%"PRIu64" background_received=%"PRIu64" elapsed_s=%.6f minor_faults=%ld major_faults=%ld\n",
           total_exchanges,bad_exchanges,background,(measured_end-base)/1e9,
           after.ru_minflt-before.ru_minflt,after.ru_majflt-before.ru_majflt);
    free(rows);
    if(error||write_error||g_stop||done!=count)return 2;
    return good==done && bad_exchanges==0?0:3;
}
