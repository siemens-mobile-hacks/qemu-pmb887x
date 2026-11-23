/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gsm-relay.c - SMS, USSD, network name and calls for gsm-bts
 * (option relay=<chardev>).
 *
 * The cell keeps only the radio side; everything behind it (routing calls and
 * SMS between phones, an SMS centre, USSD answers, call services) is an
 * external relay server the chardev connects to (natively a TCP socket,
 * port 4725 by default; doc/gsm-relay.md). Several emulators (and phones
 * with the same protocol) on one relay call and text each other. The link is
 * a byte stream of frames [type][len u8][payload]:
 *
 *   up:   'H' hello (IMSI LV), 'S' SMS-SUBMIT TPDU, 'R' delivery result,
 *         'C' call event [ev][cause][digits], 'Q' USSD request text,
 *         'U' the phone's speech (33-byte libgsm/RTP GSM-FR), 'T' trace, 'P'
 *   down: 'N' number, 'D' [oa LV][SMS-DELIVER, or an SMS-SUBMIT to convert],
 *         'C', 'q' USSD answer (UCS2),
 *         'V' far-end speech (33 bytes, TCH/FS d-bit order), 'P'
 *
 * Names: VNet / vn_* = the network side (layer 2-3 of the cell), Bridge /
 * br_* = the relay link (frames in and out of the chardev), VnBox = the
 * queues between them (box.bridge = the relay is connected).
 *
 * What gsm-bts does without the option is not changed: its LAPDm and
 * location update are replaced by this file's only while the option is set.
 *
 * Network side: LAPDm SAPI 0 and 3 on SDCCH/4 sub 0, LOCATION
 * UPDATING ACCEPT + MM INFORMATION ("Virtual Network"), CM SERVICE (SMS,
 * SS, MO call), SMS CP/RP both ways (paging by IMSI for MT), USSD to the
 * relay, call control MO/MT with ASSIGNMENT to TCH/F on timeslot
 * GSM_RELAY_TCH_TS of the BCCH carrier, FACCH/F for the signalling there and
 * TCH/FS speech both ways.
 *
 * Timing: gsm-bts codes a block when the phone first asks for one of its
 * bursts; the network's own clock (timers, paging, the relay link) is a
 * QEMU_CLOCK_VIRTUAL timer at VN_HZ (13 ticks a second, about one per CCCH
 * block). Everything runs under the BQL, like the rest of gsm-bts.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "hw/gsm/gsm-bts.h"

#define GSM48_PD_CC 0x03
#define GSM48_PD_MM 0x05
#define GSM48_PD_RR 0x06
#define GSM48_PD_SMS 0x09
#define GSM48_PD_SS 0x0B
#define GSM48_MT_RR_CHANNEL_RELEASE 0x0D
#define GSM48_MT_RR_PAGING_REQUEST_1 0x21
#define GSM48_MT_RR_ASSIGNMENT_COMMAND 0x2E
#define GSM48_RR_CAUSE_NORMAL 0x00

#define LAPDM_CONTROL_PF 0x10
#define LAPDM_CONTROL_RR 0x01
#define LAPDM_CONTROL_SABM 0x2F
#define LAPDM_CONTROL_UA 0x63
#define LAPDM_CONTROL_DISC 0x43
#define LAPDM_CONTROL_UI 0x03
#define LAPDM_SDCCH_INFO_BYTES 20
#define LAPDM_N200 8

#define VN_L3_MAX 251
#define VN_QUEUE 4
#define VN_PAGE_BLOCKS (30 * VN_HZ)   /* ticks of paging for an SMS */
#define VN_SMS_MAX 176
#define VN_HZ 13            /* network clock: ticks a second */
#define VN_CALL_PAGE (20 * VN_HZ)     /* ticks of paging for an MT call */
#define VN_SILENT_TICKS 13  /* a dedicated channel the phone stopped listening to for 1 s is gone */

/* ---------------- queues between the network side and the relay link ---------------- */

#define VNB_USSD_Q 32
#define VNB_USSD_A 160
enum { VNB_MT_NONE, VNB_MT_PAGING, VNB_MT_DELIVERED, VNB_MT_FAILED };
#define VNB_CC_N 4
#define VNB_TR_N 16
#define VNB_SP_N 64
#define SP_HIGH 32                 /* downlink speech frames kept at most (pacing guard) */
#define VNB_SP_BYTES 33
#define VNB_SU_N 32
enum { VNB_CC_SETUP = 1, VNB_CC_ALERT, VNB_CC_CONNECT, VNB_CC_DISC };

typedef struct {
    uint8_t ev, cause, len;
    char num[13];
} VnbCc;

typedef struct {
    uint32_t bridge;
    uint8_t mo_seq, mo_ack, mo_len;
    uint8_t mo[VN_SMS_MAX];
    uint8_t mt_seq, mt_ack, mt_len;
    uint8_t mt_oa[12];
    uint8_t mt[VN_SMS_MAX];
    uint8_t mt_state;
    uint8_t imsi[9];
    uint16_t stats[4];
    uint8_t up_w, up_r, dn_w, dn_r;
    VnbCc up[VNB_CC_N];
    VnbCc dn[VNB_CC_N];
    uint8_t tr_w;
    struct { uint8_t kind, len, d[14]; } tr[VNB_TR_N];
    uint8_t sp_w, sp_r, sp_drop;
    uint8_t sp[VNB_SP_N][VNB_SP_BYTES];
    uint8_t su_w, su_r;
    uint8_t su[VNB_SU_N][VNB_SP_BYTES];
    uint8_t uq_seq, uq_ack, ua_seq, ua_ack;
    uint8_t uq_len, ua_len;
    uint8_t uq[VNB_USSD_Q];
    uint8_t ua[VNB_USSD_A];
} VnBox;

/* ---------------- the network side ---------------- */

typedef struct {
    uint8_t data[VN_L3_MAX];
    uint16_t len;
} VnMsg;

typedef struct {
    uint8_t sapi;
    bool est, send_ua, disc, send_sabm, send_rr, unack;
    uint8_t vs, vr, retx;
    uint16_t sent_off, sent_len;
    VnMsg ua_info;
    VnMsg q[VN_QUEUE];
    uint8_t qh, qn;
    VnMsg rx;
} VnLink;

enum { VN_IDLE, VN_LU, VN_MO, VN_MT, VN_CALL, VN_SS };
enum { CC_NULL, CC_MO_PROC, CC_MO_DELIV, CC_MO_CONN, CC_ACTIVE, CC_MT_PAGE, CC_MT_PRES, CC_MT_CONF, CC_MT_RING,
       CC_DISC, CC_REL };
enum { CT_NONE, CT_ALERT, CT_CONNECT, CT_HANGUP, CT_GUARD };

typedef struct {
    GSMBts *bts;
    bool active, release;
    uint8_t mode, idle;
    VnLink l[2];               /* SAPI 0, SAPI 3 */
    uint8_t mi[9];
    uint8_t sms[VN_SMS_MAX];
    uint8_t sms_len;
    bool sms_pending, rp_acked, release_req;
    uint16_t page_left;
    bool mt_bridge, paging;
    uint32_t clk;              /* ticks */
    uint32_t heard;            /* clk when the phone last asked for a block of its dedicated channel */
    uint8_t cc, cc_ti;
    bool cc_bridge, cc_up_disc;
    uint8_t cc_tev;
    uint32_t cc_t, cb_t;
    uint8_t cc_nlen;
    char cc_num[13];
    bool tch, tch_go;
    uint32_t tch_go_fn;        /* the frame after the SDCCH block with the ASSIGNMENT COMMAND */
    uint8_t facch_n;
    bool sp_on;
    uint32_t tch_ul;
    bool ss_wait;
    uint8_t ss_ti, ss_inv;
    uint32_t ss_t;
    VnBox box;
} VNet;

/* ---------------- TCH/F ---------------- */

typedef struct {
    uint8_t pos[GSM_XCCH_CODED_BITS];         /* coded bit k -> burst bit (stealing flags skipped) */
    /* downlink */
    bool dl_valid;
    uint32_t dl_fn;                           /* first frame of the group in burst[] */
    uint8_t prev_steal;
    uint8_t burst[4][GSM_BURST_DATA_BITS];
    uint8_t odd[4][GSM_BURST_DATA_BITS];
    bool sacch_valid;
    uint32_t sacch_fn;
    uint8_t sacch[GSM_XCCH_BLOCK_BURSTS][GSM_BURST_DATA_BITS];
    /* uplink: the group being received and the one before */
    uint32_t ul_g[2];                         /* group numbers (frame / 26 * 6 + group), [0] current */
    bool ul_ok[2];
    uint8_t ul[2][4][GSM_BURST_DATA_BITS];
    uint32_t ul_facch, ul_speech, ul_bad;
} VTch;

/* ---------------- the relay link: frames <-> queues ---------------- */

#define BR_DQ 4
#define BR_CQ 8
#define BR_KEEPALIVE_MS 30000u

typedef struct {
    uint8_t oa[12];
    uint8_t len;
    uint8_t tpdu[VN_SMS_MAX];
} BrSms;

typedef struct {
    int up, hello, waiting;
    uint32_t last_tx_ms;
    uint8_t rx[300];
    int rxn;
    uint8_t tx[2048];
    int txn;
    BrSms dq[BR_DQ];
    int dh, dn;
    VnbCc cq[BR_CQ];
    int ch, cn;
    uint8_t tr_r;
    char number[16];
    uint8_t ua[VNB_USSD_A], ua_len, ua_pending;
} Bridge;

struct GSMRelay {
    VNet net;
    VTch tch;
    Bridge br;
    QEMUTimer *timer;
};

static void vn_link_reset(VnLink *l, uint8_t sapi) {
    memset(l, 0, sizeof(*l));
    l->sapi = sapi;
}

static void vn_link_move(VnLink *l) {
    VnMsg q[VN_QUEUE];
    uint8_t qh = l->qh, qn = l->qn, sapi = l->sapi, i;
    memcpy(q, l->q, sizeof(q));
    if (qn && q[qh].len >= 2 && q[qh].data[0] == GSM48_PD_RR && q[qh].data[1] == GSM48_MT_RR_ASSIGNMENT_COMMAND) {
        qh = (qh + 1) % VN_QUEUE;
        qn--;
    }
    vn_link_reset(l, sapi);
    for (i = 0; i < qn; i++)
        l->q[i] = q[(qh + i) % VN_QUEUE];
    l->qn = qn;
}

static void vn_trace(VNet *n, uint8_t kind, const uint8_t *d, size_t len) {
    VnBox *b = &n->box;
    uint8_t w = b->tr_w;
    if (len > sizeof(b->tr[0].d))
        len = sizeof(b->tr[0].d);
    b->tr[w % VNB_TR_N].kind = kind;
    b->tr[w % VNB_TR_N].len = (uint8_t) len;
    if (len)
        memcpy(b->tr[w % VNB_TR_N].d, d, len);
    b->tr_w = (uint8_t) (w + 1);
}

static void vn_queue(VNet *n, int s, const uint8_t *m, size_t len) {
    VnLink *l = &n->l[s];
    if (l->qn == VN_QUEUE || len > VN_L3_MAX) {
        warn_report("gsm-bts relay: SAPI %d queue full, dropping a message", s ? 3 : 0);
        return;
    }
    vn_trace(n, 'D', m, len);
    VnMsg *q = &l->q[(l->qh + l->qn) % VN_QUEUE];
    memcpy(q->data, m, len);
    q->len = (uint16_t) len;
    l->qn++;
}

static void vn_release(VNet *n) {
    n->release_req = true;
}

static void vn_release_now(VNet *n) {
    static const uint8_t rel[] = { GSM48_PD_RR, GSM48_MT_RR_CHANNEL_RELEASE, GSM48_RR_CAUSE_NORMAL };
    n->release_req = false;
    if (n->release)
        return;
    vn_queue(n, 0, rel, sizeof(rel));
    n->release = true;
    n->idle = 0;
}

static void vn_cc_lost(VNet *n);

static void vn_channel_end_why(VNet *n, uint8_t why) {
    vn_trace(n, 'E', &why, 1);
    n->sp_on = false;
    n->box.sp_r = n->box.sp_w;
    vn_cc_lost(n);
    n->active = false;
    n->tch = false;
    n->tch_go = false;
    n->release = false;
    n->release_req = false;
    vn_link_reset(&n->l[0], 0);
    vn_link_reset(&n->l[1], 3);
    if (n->sms_pending && n->mode == VN_MO && n->mi[0]) {
        n->page_left = VN_PAGE_BLOCKS;
        n->paging = true;
    }
    n->mode = VN_IDLE;
}

static void vn_channel_end(VNet *n) {
    vn_channel_end_why(n, 0);
}

static void vn_channel_start(VNet *n) {
    if (n->active) {
        uint8_t why = 2;
        vn_trace(n, 'E', &why, 1);
    }
    vn_cc_lost(n);
    n->active = true;
    n->release = false;
    n->release_req = false;
    n->idle = 0;
    n->mode = VN_IDLE;
    n->rp_acked = false;
    n->page_left = 0;
    n->paging = false;
    n->tch = false;
    n->tch_go = false;
    n->heard = n->clk;
    vn_link_reset(&n->l[0], 0);
    vn_link_reset(&n->l[1], 3);
}

static void vn_tch_start(VNet *n) {
    vn_trace(n, 'A', 0, 0);
    n->tch = true;
    n->tch_go = false;
    n->facch_n = 0;
    n->tch_ul = n->clk;
    n->heard = n->clk;
    vn_link_move(&n->l[0]);
    vn_link_reset(&n->l[1], 3);
}

static void vn_save_mi(VNet *n, const uint8_t *lv, const uint8_t *end) {
    if (lv >= end || lv[0] == 0 || lv[0] > 8 || lv + 1 + lv[0] > end)
        return;
    if ((lv[1] & 7) != 1)      /* IMSI only (no TMSI is ever given out) */
        return;
    memcpy(n->mi, lv, 1 + lv[0]);
    memcpy(n->box.imsi, lv, 1 + lv[0]);
}

/* SMS-SUBMIT -> SMS-DELIVER (sender = oa, or the destination address when oa is 0); an SMS-DELIVER (the
 * relay's own, with the PC's time) is taken as is; 0 if neither. A SUBMIT (older relays) gets the host's local
 * time as the service centre time stamp. */
static int vn_submit_to_deliver(VNet *n, const uint8_t *t, size_t len, const uint8_t *oa) {
    uint8_t scts[7];
    size_t i = 2, alen, vp = 0, o = 0;
    uint8_t *d = n->sms;
    time_t now = time(NULL);
    struct tm tm;
    int f[6], tz;

    localtime_r(&now, &tm);
    f[0] = tm.tm_year % 100; f[1] = tm.tm_mon + 1; f[2] = tm.tm_mday;
    f[3] = tm.tm_hour; f[4] = tm.tm_min; f[5] = tm.tm_sec;
    for (int k = 0; k < 6; k++)
        scts[k] = (uint8_t) ((f[k] % 10) << 4 | f[k] / 10);
    tz = (int) (tm.tm_gmtoff / 900);           /* quarter hours, sign in bit 3 of the first digit */
    scts[6] = (uint8_t) (((abs(tz) % 10) << 4) | (abs(tz) / 10) | (tz < 0 ? 0x08 : 0));

    if (len >= 7 && (t[0] & 3) == 0 && len <= VN_SMS_MAX) {   /* SMS-DELIVER from the relay */
        memcpy(d, t, len);
        n->sms_len = (uint8_t) len;
        return 1;
    }
    if (len < 7 || (t[0] & 3) != 1)
        return 0;
    switch ((t[0] >> 3) & 3) {
        case 2: vp = 1; break;
        case 1: case 3: vp = 7; break;
    }
    alen = 2 + (t[i] + 1) / 2;
    if (i + alen + 2 + vp + 1 > len)
        return 0;
    d[o++] = 0x04 | (t[0] & 0x40);
    if (oa && oa[0] <= 20) {
        size_t olen = 2 + (oa[0] + 1) / 2;
        memcpy(&d[o], oa, olen);
        o += olen;
    } else {
        memcpy(&d[o], &t[i], alen);
        o += alen;
    }
    i += alen;
    d[o++] = t[i++];                    /* PID */
    d[o++] = t[i++];                    /* DCS */
    i += vp;
    memcpy(&d[o], scts, 7);
    o += 7;
    if (o + (len - i) > VN_SMS_MAX)
        return 0;
    memcpy(&d[o], &t[i], len - i);
    o += len - i;
    n->sms_len = (uint8_t) o;
    return 1;
}

static void vn_cp(VNet *n, uint8_t ti_pd, uint8_t type, const uint8_t *rp, size_t rplen) {
    uint8_t m[3 + VN_L3_MAX];
    m[0] = ti_pd;
    m[1] = type;
    if (type == 0x01) {
        m[2] = (uint8_t) rplen;
        memcpy(&m[3], rp, rplen);
        vn_queue(n, 1, m, 3 + rplen);
    } else {
        vn_queue(n, 1, m, 2);
    }
}

static void vn_deliver(VNet *n) {
    uint8_t rp[6 + VN_SMS_MAX];
    size_t o = 0;
    rp[o++] = 0x01;                     /* RP-DATA network -> MS */
    rp[o++] = 0x42;                     /* RP-MR */
    rp[o++] = 2; rp[o++] = 0x91; rp[o++] = 0x10;   /* RP-OA = service centre "01" */
    rp[o++] = 0;                        /* RP-DA */
    rp[o++] = n->sms_len;
    memcpy(&rp[o], n->sms, n->sms_len);
    o += n->sms_len;
    vn_cp(n, 0x09, 0x01, rp, o);
}

static void vn_cc_mt_setup(VNet *n);

/* MM INFORMATION: full name "Virtual Network", short "Virtual" (GSM 7-bit packed) */
static const uint8_t VN_MM_INFO[] = { 0x05, 0x32, 0x43, 0x0F, 0x87, 0xD6, 0xB4, 0x9C, 0x5E, 0x0F, 0xB3, 0x41, 0xCE,
    0x32, 0xFD, 0xFE, 0x96, 0xAF, 0x01, 0x45, 0x08, 0x87, 0xD6, 0xB4, 0x9C, 0x5E, 0x0F, 0xB3, 0x01 };

static void vn_l3_mm_rr(VNet *n, const uint8_t *m, size_t len) {
    const uint8_t *end = m + len;
    uint8_t pd = m[0] & 0x0F, mt = m[1] & 0x3F;

    if (pd == GSM48_PD_MM && mt == 0x08) {                  /* LOCATION UPDATING REQUEST */
        uint8_t acc[2 + GSM48_LAI_BYTES] = { GSM48_PD_MM, 0x02 };
        if (len > 9)
            vn_save_mi(n, &m[9], end);
        gsm_bts_encode_lai(&acc[2]);
        n->mode = VN_LU;
        vn_queue(n, 0, acc, sizeof(acc));
        vn_queue(n, 0, VN_MM_INFO, sizeof(VN_MM_INFO));
        vn_release(n);
        n->bts->ms_state |= GSM_BTS_MS_REGISTERED; /* WASM PORT (removable) */
    } else if (pd == GSM48_PD_MM && mt == 0x24) {           /* CM SERVICE REQUEST */
        static const uint8_t acc[] = { GSM48_PD_MM, 0x21 };
        static const uint8_t rej[] = { GSM48_PD_MM, 0x22, 0x20 };
        if (len > 4)
            vn_save_mi(n, &m[4 + m[3]], end);
        if ((m[2] & 0x0F) == 4) {                           /* SMS */
            n->mode = VN_MO;
            vn_queue(n, 0, acc, sizeof(acc));
        } else if ((m[2] & 0x0F) == 8) {                    /* supplementary service (USSD) */
            n->mode = VN_SS;
            vn_queue(n, 0, acc, sizeof(acc));
        } else if ((m[2] & 0x0F) == 1 && n->cc == CC_NULL) {  /* MO call (not emergency) */
            n->mode = VN_CALL;
            vn_queue(n, 0, acc, sizeof(acc));
        } else {
            vn_queue(n, 0, rej, sizeof(rej));
            vn_release(n);
        }
    } else if (pd == GSM48_PD_MM && mt == 0x01) {           /* IMSI DETACH INDICATION */
        n->bts->ms_state &= ~GSM_BTS_MS_REGISTERED; /* WASM PORT (removable) */
        vn_release(n);
    } else if (pd == GSM48_PD_RR && mt == 0x27) {           /* PAGING RESPONSE */
        n->page_left = 0;
        if (n->cc == CC_MT_PAGE) {
            n->mode = VN_CALL;
            vn_cc_mt_setup(n);
        } else if (n->sms_pending) {
            n->mode = VN_MT;
            n->l[1].send_sabm = true;
            n->l[1].retx = 0;
        } else {
            vn_release(n);
        }
    }
}

static void vn_l3_sms(VNet *n, const uint8_t *m, size_t len) {
    uint8_t ti = m[0] & 0xF0, mt = m[1], back = (uint8_t) ((ti ^ 0x80) | 0x09);

    if (mt == 0x01 && len >= 4 && m[2] + 3u <= len) {       /* CP-DATA */
        const uint8_t *rp = &m[3];
        size_t rl = m[2];
        vn_cp(n, back, 0x04, 0, 0);                         /* CP-ACK */
        if (rp[0] == 0x00 && rl >= 4) {                     /* RP-DATA MS -> network: SMS-SUBMIT */
            size_t i = 2;
            uint8_t ack[2] = { 0x03, rp[1] };
            i += 1 + rp[i];                                 /* RP-OA */
            if (i < rl)
                i += 1 + rp[i];                             /* RP-DA */
            if (n->box.bridge) {
                if (n->box.mo_seq != n->box.mo_ack || i >= rl || i + 1 + rp[i] > rl || rp[i] > VN_SMS_MAX) {
                    uint8_t err[4] = { 0x05, rp[1], 1, 42 };   /* RP-ERROR, congestion */
                    n->box.stats[1]++;
                    vn_cp(n, back, 0x01, err, sizeof(err));
                    n->rp_acked = true;
                    return;
                }
                memcpy(n->box.mo, &rp[i + 1], rp[i]);
                n->box.mo_len = rp[i];
                n->box.mo_seq++;
                n->box.stats[0]++;
            } else if (i < rl && i + 1 + rp[i] <= rl && vn_submit_to_deliver(n, &rp[i + 1], rp[i], 0)) {
                n->sms_pending = true;                      /* no relay: back to the sender */
                n->mt_bridge = false;
            }
            vn_cp(n, back, 0x01, ack, sizeof(ack));         /* CP-DATA (RP-ACK) */
            n->rp_acked = true;
        } else if (rp[0] == 0x02) {                         /* RP-ACK MS -> network: delivered */
            if (n->sms_pending && n->mt_bridge) {
                n->box.mt_state = VNB_MT_DELIVERED;
                n->box.stats[3]++;
            }
            n->sms_pending = false;
            vn_release(n);
        } else {
            if (n->sms_pending && n->mt_bridge)
                n->box.mt_state = VNB_MT_FAILED;
            n->sms_pending = false;
            vn_release(n);
        }
    } else if (mt == 0x04) {                                /* CP-ACK */
        if (n->mode == VN_MO && n->rp_acked)
            vn_release(n);
    } else if (mt == 0x10) {                                /* CP-ERROR */
        vn_release(n);
    }
}

/* ---------------- call control ---------------- */

static void vn_cc_send(VNet *n, uint8_t mt, const uint8_t *ie, size_t len) {
    uint8_t m[2 + 32];
    if (len > 32)
        return;
    m[0] = (uint8_t) (n->cc_ti | GSM48_PD_CC);
    m[1] = mt;
    if (len)
        memcpy(&m[2], ie, len);
    vn_queue(n, 0, m, 2 + len);
}

static void vn_cc_up_raw(VNet *n, uint8_t ev, uint8_t cause) {
    VnBox *b = &n->box;
    VnbCc *e;
    if ((uint8_t) (b->up_w - b->up_r) >= VNB_CC_N)
        return;
    e = &b->up[b->up_w % VNB_CC_N];
    e->ev = ev;
    e->cause = cause;
    e->len = 0;
    if (ev == VNB_CC_SETUP) {
        memcpy(e->num, n->cc_num, n->cc_nlen);
        e->len = n->cc_nlen;
    }
    b->up_w = (uint8_t) (b->up_w + 1);
}

static void vn_cc_up(VNet *n, uint8_t ev, uint8_t cause) {
    if (!n->cc_bridge)
        return;
    if (ev == VNB_CC_DISC) {
        if (n->cc_up_disc)
            return;
        n->cc_up_disc = true;
    }
    vn_cc_up_raw(n, ev, cause);
}

static void vn_cc_timer(VNet *n, uint8_t ev, unsigned sec) {
    n->cc_tev = ev;
    n->cc_t = n->clk + sec * VN_HZ;
}

static void vn_cc_clear(VNet *n) {
    n->cc = CC_NULL;
    n->cc_bridge = false;
    n->cc_tev = CT_NONE;
    if (n->active)
        vn_release(n);
}

static void vn_cc_net_disc(VNet *n, uint8_t cause) {
    uint8_t ie[3] = { 2, 0xE0, (uint8_t) (0x80 | cause) };
    switch (n->cc) {
        case CC_NULL: case CC_DISC: case CC_REL:
            return;
        case CC_MT_PAGE:
            n->page_left = 0;
            n->cc = CC_NULL;
            n->cc_bridge = false;
            return;
    }
    vn_cc_send(n, 0x25, ie, sizeof(ie));                    /* DISCONNECT */
    n->cc = CC_DISC;
    vn_cc_timer(n, CT_GUARD, 10);
}

static void vn_cc_lost(VNet *n) {
    if (n->cc == CC_NULL || n->cc == CC_MT_PAGE)
        return;
    vn_cc_up(n, VNB_CC_DISC, 41);
    n->cc = CC_NULL;
    n->cc_bridge = false;
    n->cc_tev = CT_NONE;
}

/* ASSIGNMENT COMMAND: TCH/F on GSM_RELAY_TCH_TS of the BCCH carrier, TSC 0, power 0, speech full rate v1 */
static void vn_cc_assign(VNet *n) {
    uint16_t arfcn = n->bts->arfcn;
    uint8_t ac[] = { GSM48_PD_RR, GSM48_MT_RR_ASSIGNMENT_COMMAND, 0x08 | GSM_RELAY_TCH_TS,
        (uint8_t) ((GSM_BTS_TSC << 5) | (arfcn >> 8)), (uint8_t) arfcn, 0x00, 0x63, 0x01 };
    if (!n->tch && !n->tch_go)
        vn_queue(n, 0, ac, sizeof(ac));
}

static void vn_cc_mt_setup(VNet *n) {
    uint8_t ie[4 + 3 + 7], o = 0, i;
    n->cc_ti = 0x00;
    ie[o++] = 0x04; ie[o++] = 1; ie[o++] = 0xA0;            /* bearer capability: speech, full rate only */
    if (n->cc_nlen) {
        uint8_t nb = (uint8_t) ((n->cc_nlen + 1) / 2);
        ie[o++] = 0x5C; ie[o++] = (uint8_t) (1 + nb); ie[o++] = 0x81;   /* calling party: unknown / ISDN */
        for (i = 0; i < nb; i++) {
            uint8_t lo = (uint8_t) (n->cc_num[2 * i] - '0');
            uint8_t hi = 2 * i + 1 < n->cc_nlen ? (uint8_t) (n->cc_num[2 * i + 1] - '0') : 15;
            ie[o++] = (uint8_t) ((lo & 15) | hi << 4);
        }
    }
    vn_cc_send(n, 0x05, ie, o);
    n->cc = CC_MT_PRES;
    n->cc_up_disc = false;
    vn_cc_timer(n, CT_NONE, 0);
}

static void vn_cc_mt(VNet *n, const char *num, uint8_t len, bool bridge) {
    if (n->active || n->cc != CC_NULL || !n->mi[0]) {
        if (bridge)
            vn_cc_up_raw(n, VNB_CC_DISC, 17);               /* user busy */
        return;
    }
    if (len > sizeof(n->cc_num))
        len = sizeof(n->cc_num);
    memcpy(n->cc_num, num, len);
    n->cc_nlen = len;
    n->cc = CC_MT_PAGE;
    n->cc_bridge = bridge;
    n->cc_up_disc = false;
    n->page_left = VN_CALL_PAGE;
}

static uint8_t vn_cc_state(const VNet *n) {
    static const uint8_t st[] = { 0, 3, 4, 8, 10, 6, 6, 9, 7, 12, 19 };
    return (uint8_t) (0xC0 | st[n->cc]);
}

static void vn_l3_cc(VNet *n, const uint8_t *m, size_t len) {
    uint8_t mt = m[1] & 0x3F, cause = 16;
    size_t i;

    if (mt == 0x05) {                                       /* SETUP (MO) */
        if (n->cc != CC_NULL) {
            static const uint8_t ie[] = { 0x08, 2, 0xE0, 0x80 | 17 };
            uint8_t ti = n->cc_ti;
            n->cc_ti = (uint8_t) ((m[0] & 0xF0) ^ 0x80);
            vn_cc_send(n, 0x2A, ie, sizeof(ie));            /* RELEASE COMPLETE, busy */
            n->cc_ti = ti;
            return;
        }
        n->cc_ti = (uint8_t) ((m[0] & 0xF0) ^ 0x80);
        n->cc_nlen = 0;
        for (i = 2; i < len;) {
            uint8_t iei = m[i];
            if (iei & 0x80) {
                i++;
                continue;
            }
            if (i + 1 >= len || i + 2 + m[i + 1] > len)
                break;
            if (iei == 0x5E && m[i + 1] >= 2) {             /* called party BCD number */
                size_t k;
                for (k = i + 3; k < i + 2 + m[i + 1] && n->cc_nlen < sizeof(n->cc_num); k++) {
                    uint8_t d[2] = { (uint8_t) (m[k] & 15), (uint8_t) (m[k] >> 4) };
                    for (int j = 0; j < 2 && n->cc_nlen < sizeof(n->cc_num); j++)
                        if (d[j] < 15)
                            n->cc_num[n->cc_nlen++] = "0123456789*#abc"[d[j]];
                }
            }
            i += 2 + m[i + 1];
        }
        vn_cc_send(n, 0x02, 0, 0);                          /* CALL PROCEEDING */
        vn_cc_assign(n);
        n->cc = CC_MO_PROC;
        n->cc_up_disc = false;
        if (n->cc_nlen == 3 && !memcmp(n->cc_num, "100", 3)) {   /* the cell's own test number */
            n->cc_bridge = false;
            vn_cc_timer(n, CT_HANGUP, 1);
        } else if (n->box.bridge) {
            n->cc_bridge = true;
            vn_cc_up(n, VNB_CC_SETUP, 0);
        } else {
            n->cc_bridge = false;                           /* no relay: the cell answers itself */
            vn_cc_timer(n, CT_ALERT, 2);
        }
        return;
    }
    if (len >= 5 && (mt == 0x25 || mt == 0x2D || mt == 0x2A) && m[2] >= 2)
        cause = m[4] & 0x7F;
    switch (mt) {
        case 0x0F:                                          /* CONNECT ACKNOWLEDGE */
            if (n->cc == CC_MO_CONN)
                n->cc = CC_ACTIVE;
            break;
        case 0x08:                                          /* CALL CONFIRMED */
            if (n->cc == CC_MT_PRES) {
                n->cc = CC_MT_CONF;
                vn_cc_assign(n);
            }
            break;
        case 0x01:                                          /* ALERTING */
            if (n->cc == CC_MT_PRES || n->cc == CC_MT_CONF) {
                n->cc = CC_MT_RING;
                vn_cc_up(n, VNB_CC_ALERT, 0);
            }
            break;
        case 0x07:                                          /* CONNECT */
            if (n->cc == CC_MT_PRES || n->cc == CC_MT_CONF || n->cc == CC_MT_RING) {
                vn_cc_send(n, 0x0F, 0, 0);                  /* CONNECT ACKNOWLEDGE */
                n->cc = CC_ACTIVE;
                vn_cc_up(n, VNB_CC_CONNECT, 0);
            }
            break;
        case 0x25:                                          /* DISCONNECT (the phone hangs up) */
            if (n->cc == CC_NULL)
                break;
            vn_cc_up(n, VNB_CC_DISC, cause);
            vn_cc_send(n, 0x2D, 0, 0);                      /* RELEASE */
            n->cc = CC_REL;
            vn_cc_timer(n, CT_GUARD, 10);
            break;
        case 0x2D:                                          /* RELEASE */
            vn_cc_send(n, 0x2A, 0, 0);                      /* RELEASE COMPLETE */
            if (n->cc != CC_NULL) {
                vn_cc_up(n, VNB_CC_DISC, cause);
                vn_cc_clear(n);
            }
            break;
        case 0x2A:                                          /* RELEASE COMPLETE */
            if (n->cc != CC_NULL) {
                vn_cc_up(n, VNB_CC_DISC, cause);
                vn_cc_clear(n);
            }
            break;
        case 0x34: {                                        /* STATUS ENQUIRY -> STATUS */
            uint8_t ie[4] = { 2, 0xE0, 0x80 | 30, vn_cc_state(n) };
            vn_cc_send(n, 0x3D, ie, sizeof(ie));
            break;
        }
        case 0x35:                                          /* START DTMF -> START DTMF ACKNOWLEDGE */
            if (len >= 4 && m[2] == 0x2C) {
                uint8_t ie[2] = { 0x2C, m[3] };
                vn_cc_send(n, 0x36, ie, sizeof(ie));
            }
            break;
        case 0x31:                                          /* STOP DTMF -> STOP DTMF ACKNOWLEDGE */
            vn_cc_send(n, 0x32, 0, 0);
            break;
    }
}

static void vn_cc_far(VNet *n, uint8_t ev, uint8_t cause, const char *num, uint8_t nlen, bool bridge) {
    switch (ev) {
        case VNB_CC_SETUP:
            vn_cc_mt(n, num, nlen, bridge);
            break;
        case VNB_CC_ALERT:
            if (n->cc == CC_MO_PROC) {
                vn_cc_send(n, 0x01, 0, 0);                  /* ALERTING */
                n->cc = CC_MO_DELIV;
            }
            break;
        case VNB_CC_CONNECT:
            if (n->cc == CC_MO_PROC || n->cc == CC_MO_DELIV) {
                vn_cc_send(n, 0x07, 0, 0);                  /* CONNECT */
                n->cc = CC_MO_CONN;
            }
            break;
        case VNB_CC_DISC:
            n->cc_up_disc = true;
            vn_cc_net_disc(n, cause ? cause : 16);
            break;
    }
}

static void vn_cc_poll(VNet *n) {
    VnBox *b = &n->box;
    while (b->dn_r != b->dn_w) {
        const VnbCc *e = &b->dn[b->dn_r % VNB_CC_N];
        uint8_t ev = e->ev, cause = e->cause, len = e->len < sizeof(e->num) ? e->len : sizeof(e->num);
        char num[13];
        memcpy(num, e->num, len);
        b->dn_r = (uint8_t) (b->dn_r + 1);
        if (ev != VNB_CC_SETUP && !n->cc_bridge)
            continue;
        vn_cc_far(n, ev, cause, num, len, true);
    }
    if (n->cc_bridge && !b->bridge && n->cc != CC_NULL) {   /* relay gone */
        n->cc_up_disc = true;
        if (n->cc == CC_MT_PAGE) {
            n->page_left = 0;
            n->cc = CC_NULL;
            n->cc_bridge = false;
        } else {
            vn_cc_net_disc(n, 41);
        }
    }
    if (n->cc == CC_MT_PAGE && n->page_left == 0 && !n->active) {   /* no paging response */
        vn_cc_up(n, VNB_CC_DISC, 18);
        n->cc = CC_NULL;
        n->cc_bridge = false;
    }
    if (n->cc_tev != CT_NONE && (int32_t) (n->clk - n->cc_t) >= 0) {
        uint8_t ev = n->cc_tev;
        n->cc_tev = CT_NONE;
        switch (ev) {
            case CT_ALERT:
                vn_cc_far(n, VNB_CC_ALERT, 0, 0, 0, false);
                vn_cc_timer(n, CT_CONNECT, 2);
                break;
            case CT_CONNECT:
                vn_cc_far(n, VNB_CC_CONNECT, 0, 0, 0, false);
                break;
            case CT_HANGUP:                                 /* "100": hang up, call back in 5 s */
                vn_cc_net_disc(n, 16);
                n->cb_t = n->clk + 5 * VN_HZ;
                break;
            case CT_GUARD:
                if (n->cc == CC_DISC || n->cc == CC_REL)
                    vn_cc_clear(n);
                break;
        }
    }
    if (n->cb_t && (int32_t) (n->clk - n->cb_t) >= 0 && !n->active && n->cc == CC_NULL) {
        n->cb_t = 0;
        vn_cc_mt(n, "100", 3, false);
    }
}

/* ---------------- USSD ---------------- */

#define VN_SS_WAIT (8 * VN_HZ)

static int vn_ber(const uint8_t *m, size_t len, size_t *i, uint8_t *tag, size_t *l) {
    if (*i + 2 > len)
        return 0;
    *tag = m[(*i)++];
    *l = m[(*i)++];
    if (*l == 0x81 && *i < len)
        *l = m[(*i)++];
    else if (*l > 0x7F)
        return 0;
    return *i + *l <= len;
}

static size_t vn_tl(uint8_t *o, uint8_t tag, size_t n) {
    o[0] = tag;
    if (n < 0x80) {
        o[1] = (uint8_t) n;
        return 2;
    }
    o[1] = 0x81;
    o[2] = (uint8_t) n;
    return 3;
}

static void vn_ss_done(VNet *n, const uint8_t *ucs2, size_t nt) {
    uint8_t rc[24 + VNB_USSD_A], body[16 + VNB_USSD_A];
    size_t o = 0, k, inner, seq, rr;

    n->ss_wait = false;
    rc[o++] = (uint8_t) (n->ss_ti | GSM48_PD_SS);
    rc[o++] = 0x2A;                                         /* RELEASE COMPLETE */
    if (ucs2 && nt) {
        if (nt > VNB_USSD_A)
            nt = VNB_USSD_A;
        inner = 3 + (nt < 0x80 ? 2 : 3) + nt;
        seq = 3 + (inner < 0x80 ? 2 : 3) + inner;
        rr = 3 + (seq < 0x80 ? 2 : 3) + seq;
        k = 0;
        k += vn_tl(&body[k], 0xA2, rr);                     /* returnResult */
        body[k++] = 0x02; body[k++] = 0x01; body[k++] = n->ss_inv;
        k += vn_tl(&body[k], 0x30, seq);
        body[k++] = 0x02; body[k++] = 0x01; body[k++] = 0x3B;   /* processUnstructuredSS-Request */
        k += vn_tl(&body[k], 0x30, inner);
        body[k++] = 0x04; body[k++] = 0x01; body[k++] = 0x48;   /* DCS: UCS2 */
        k += vn_tl(&body[k], 0x04, nt);
        memcpy(&body[k], ucs2, nt);
        k += nt;
        rc[o++] = 0x1C;                                     /* Facility IE */
        rc[o++] = (uint8_t) k;
        memcpy(&rc[o], body, k);
        o += k;
    }
    vn_queue(n, 0, rc, o);
    vn_release(n);
}

static void vn_l3_ss(VNet *n, const uint8_t *m, size_t len) {
    uint8_t mt = m[1] & 0x3F, tag, inv = 1, dcs = 0x0F;
    const uint8_t *str = 0;
    size_t i = 2, l, sl = 0, k = 0;
    char req[VNB_USSD_Q];

    if (mt != 0x3B) {                                       /* not REGISTER: done */
        n->ss_wait = false;
        vn_release(n);
        return;
    }
    while (i < len && m[i] != 0x1C)
        i += (m[i] & 0x80) ? 1 : 2 + (i + 1 < len ? m[i + 1] : len);
    if (i + 2 <= len) {
        size_t fe = i + 2 + m[i + 1], j = i + 2;
        if (fe <= len && vn_ber(m, fe, &j, &tag, &l) && tag == 0xA1) {    /* invoke */
            size_t ce = j + l;
            if (vn_ber(m, ce, &j, &tag, &l) && tag == 0x02 && l == 1)
                inv = m[j];
            j += l;
            if (vn_ber(m, ce, &j, &tag, &l))
                j += l;
            if (vn_ber(m, ce, &j, &tag, &l) && tag == 0x30) {
                size_t pe = j + l;
                if (vn_ber(m, pe, &j, &tag, &l) && tag == 0x04 && l == 1)
                    dcs = m[j];
                j += l;
                if (vn_ber(m, pe, &j, &tag, &l) && tag == 0x04) {
                    str = &m[j];
                    sl = l;
                }
            }
        }
    }
    if (str && !((dcs & 0xC0) == 0x40 && (dcs & 0x0C) != 0)) {   /* GSM 7-bit packed */
        size_t nc = sl * 8 / 7;
        for (size_t c = 0; c < nc && k < sizeof(req); c++) {
            size_t bit = c * 7;
            uint16_t w = (uint16_t) (str[bit / 8] | (bit / 8 + 1 < sl ? str[bit / 8 + 1] << 8 : 0));
            char ch = (char) ((w >> (bit % 8)) & 0x7F);
            if (ch == 0x0D || (c == nc - 1 && ch == 0))
                break;
            req[k++] = ch;
        }
    }
    vn_trace(n, 'W', (const uint8_t *) req, k > 14 ? 14 : k);
    n->ss_ti = (uint8_t) ((m[0] & 0xF0) ^ 0x80);
    n->ss_inv = inv;
    if (!n->box.bridge) {
        vn_ss_done(n, 0, 0);
        return;
    }
    n->box.ua_ack = n->box.ua_seq;
    memcpy(n->box.uq, req, k);
    n->box.uq_len = (uint8_t) k;
    n->box.uq_seq = (uint8_t) (n->box.uq_seq + 1);
    n->ss_wait = true;
    n->ss_t = n->clk;
}

static void vn_ss_poll(VNet *n) {
    VnBox *b = &n->box;
    if (!n->ss_wait) {
        b->ua_ack = b->ua_seq;
        return;
    }
    if (!n->active) {
        n->ss_wait = false;
        return;
    }
    if (b->ua_seq != b->ua_ack) {
        uint8_t a[VNB_USSD_A];
        size_t na = b->ua_len <= VNB_USSD_A ? b->ua_len : VNB_USSD_A;
        memcpy(a, b->ua, na);
        b->ua_ack = b->ua_seq;
        vn_ss_done(n, a, na);
    } else if ((int32_t) (n->clk - n->ss_t) > VN_SS_WAIT) {
        vn_ss_done(n, 0, 0);
    }
}

/* ---------------- LAPDm ---------------- */

static void vn_l3(VNet *n, int s, const uint8_t *m, size_t len) {
    if (len < 2 || (n->release && s == 0))
        return;
    vn_trace(n, 'U', m, len);
    if (s == 0 && (m[0] & 0x0F) == GSM48_PD_CC)
        vn_l3_cc(n, m, len);
    else if (s == 0 && (m[0] & 0x0F) == GSM48_PD_SS)
        vn_l3_ss(n, m, len);
    else if (s == 0)
        vn_l3_mm_rr(n, m, len);
    else if ((m[0] & 0x0F) == GSM48_PD_SMS)
        vn_l3_sms(n, m, len);
}

static void vn_ack(VnLink *l, uint8_t nr) {
    if (!l->unack || nr != ((l->vs + 1) & 7))
        return;
    l->vs = nr;
    l->unack = false;
    l->retx = 0;
    l->sent_off += l->sent_len;
    if (l->sent_off < l->q[l->qh].len)
        return;
    l->sent_off = 0;
    l->qh = (l->qh + 1) % VN_QUEUE;
    l->qn--;
}

static void vn_rx(VNet *n, const uint8_t *b) {
    uint8_t sapi = (b[0] >> 2) & 7, c = b[1], len = b[2] >> 2;
    bool more = b[2] & 2;
    int s = sapi == 0 ? 0 : sapi == 3 ? 1 : -1;
    VnLink *l;

    if (!n->active || s < 0 || !(b[0] & 1) || !(b[2] & 1))
        return;
    if (len > LAPDM_SDCCH_INFO_BYTES)
        len = LAPDM_SDCCH_INFO_BYTES;
    l = &n->l[s];
    if ((c & 1) == 0) {                                     /* I */
        uint8_t ns = (c >> 1) & 7;
        vn_ack(l, c >> 5);
        l->send_rr = true;
        if (!l->est || ns != l->vr)
            return;
        l->vr = (l->vr + 1) & 7;
        if (l->rx.len + len > VN_L3_MAX) {
            l->rx.len = 0;
            return;
        }
        memcpy(&l->rx.data[l->rx.len], &b[3], len);
        l->rx.len += len;
        if (!more) {
            uint16_t rl = l->rx.len;
            l->rx.len = 0;
            vn_l3(n, s, l->rx.data, rl);
        }
        return;
    }
    if ((c & 3) == 1) {                                     /* RR / RNR / REJ */
        vn_ack(l, c >> 5);
        if (c & 0x10)
            l->send_rr = true;
        return;
    }
    switch (c & 0xEF) {
        case LAPDM_CONTROL_SABM:
            if (!l->est) {
                l->est = true;
                l->vs = l->vr = 0;
                l->unack = false;
                l->sent_off = 0;
                memcpy(l->ua_info.data, &b[3], len);
                l->ua_info.len = len;
                if (len)
                    vn_l3(n, s, &b[3], len);
            }
            l->send_ua = true;
            break;
        case LAPDM_CONTROL_UA:
            if (l->send_sabm) {
                l->send_sabm = false;
                l->est = true;
                l->vs = l->vr = 0;
                l->retx = 0;
                if (s == 1 && n->mode == VN_MT)
                    vn_deliver(n);
            }
            break;
        case LAPDM_CONTROL_DISC:
            l->send_ua = true;
            l->ua_info.len = 0;
            l->disc = true;
            break;
        case 0x0F:                                          /* DM */
            if (l->send_sabm || l->est)
                vn_release(n);
            vn_link_reset(l, l->sapi);
            break;
    }
}

static void vn_frame(uint8_t *b, uint8_t addr, uint8_t ctrl, const uint8_t *info, size_t len, bool more) {
    b[0] = addr;
    b[1] = ctrl;
    b[2] = (uint8_t) ((len << 2) | (more ? 2 : 0) | 1);
    if (len)
        memcpy(&b[3], info, len);
}

static bool vn_link_tx(VNet *n, VnLink *l, uint8_t *b) {
    uint8_t cmd = (uint8_t) ((l->sapi << 2) | 3), rsp = (uint8_t) ((l->sapi << 2) | 1);

    if (l->send_ua) {
        l->send_ua = false;
        vn_frame(b, rsp, LAPDM_CONTROL_UA | LAPDM_CONTROL_PF, l->ua_info.data, l->ua_info.len, false);
        if (l->disc) {
            vn_link_reset(l, l->sapi);
            if (l == &n->l[0])
                vn_channel_end(n);
        }
        return true;
    }
    if (l->send_sabm) {
        if (++l->retx > LAPDM_N200) {
            l->send_sabm = false;
            vn_release(n);
            return false;
        }
        vn_frame(b, cmd, LAPDM_CONTROL_SABM | LAPDM_CONTROL_PF, 0, 0, false);
        return true;
    }
    if (l->est && l->qn > 0) {
        const VnMsg *m = &l->q[l->qh];
        size_t rem = m->len - l->sent_off, len = rem < LAPDM_SDCCH_INFO_BYTES ? rem : LAPDM_SDCCH_INFO_BYTES;
        if (l->unack && ++l->retx > LAPDM_N200) {
            vn_release(n);
            vn_link_reset(l, l->sapi);
            return false;
        }
        l->unack = true;
        l->sent_len = (uint16_t) len;
        l->send_rr = false;
        if (l->sapi == 0 && !n->tch && m->len >= 2 && m->data[0] == GSM48_PD_RR &&
            m->data[1] == GSM48_MT_RR_ASSIGNMENT_COMMAND)
            n->tch_go = true;
        vn_frame(b, cmd, (uint8_t) ((l->vr << 5) | (l->vs << 1)), &m->data[l->sent_off], len, len < rem);
        return true;
    }
    if (l->send_rr) {
        l->send_rr = false;
        vn_frame(b, rsp, (uint8_t) ((l->vr << 5) | LAPDM_CONTROL_RR), 0, 0, false);
        return true;
    }
    return false;
}

static void vn_sdcch_tx(VNet *n, uint8_t *b) {
    memset(b, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    n->heard = n->clk;
    if (n->release_req && n->l[1].qn == 0 && !n->l[1].send_sabm)
        vn_release_now(n);
    if (n->active && (vn_link_tx(n, &n->l[0], b) || vn_link_tx(n, &n->l[1], b))) {
        n->idle = 0;
        return;
    }
    vn_frame(b, 0x01, LAPDM_CONTROL_UI, 0, 0, false);
    if (n->active && n->release && n->l[0].qn == 0 && ++n->idle > 12)
        vn_channel_end(n);
}

/* FACCH: LAPDm gets a turn every 8th TCH block (~160 ms, T200-like) */
static bool vn_facch_tx(VNet *n, uint8_t *b) {
    if (++n->facch_n < 8) {
        n->heard = n->clk;
        return false;
    }
    n->facch_n = 0;
    vn_sdcch_tx(n, b);
    return !(b[0] == 0x01 && b[1] == LAPDM_CONTROL_UI && b[2] == 0x01);
}

static void vn_paging(VNet *n, uint8_t *b) {
    size_t o = 0;
    memset(b, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
    b[o++] = (uint8_t) (((3 + 1 + n->mi[0]) << 2) | 1);
    b[o++] = GSM48_PD_RR;
    b[o++] = GSM48_MT_RR_PAGING_REQUEST_1;
    b[o++] = 0x00;
    memcpy(&b[o], n->mi, 1 + n->mi[0]);
}

static void vn_poll(VNet *n) {
    VnBox *b = &n->box;
    n->clk++;
    if (n->page_left)
        n->page_left--;
    if (n->active && n->tch && (int32_t) (n->clk - n->tch_ul) > 6 * VN_HZ)
        vn_channel_end_why(n, 1);   /* no uplink on the TCH for 6 s */
    else if (n->active && (int32_t) (n->clk - n->heard) > VN_SILENT_TICKS)
        vn_channel_end_why(n, n->release ? 0 : 3);   /* the phone stopped listening: it left the channel */
    vn_cc_poll(n);
    vn_ss_poll(n);
    if (n->active)
        return;
    if (n->sms_pending && n->paging && n->page_left == 0) {
        if (getenv("GSM_RELAY_DEBUG"))
            info_report("gsm-bts relay: no paging response, SMS dropped (clk %u)", n->clk);
        if (n->mt_bridge)
            b->mt_state = VNB_MT_FAILED;
        n->sms_pending = false;
        n->paging = false;
    }
    if (n->sms_pending || n->cc != CC_NULL || b->mt_seq == b->mt_ack || !n->mi[0])
        return;
    b->stats[2]++;
    if (getenv("GSM_RELAY_DEBUG"))
        info_report("gsm-bts relay: SMS for the phone, %u bytes, paging (clk %u)", b->mt_len, n->clk);
    if (b->mt_len > VN_SMS_MAX || !vn_submit_to_deliver(n, b->mt, b->mt_len, b->mt_oa)) {
        if (getenv("GSM_RELAY_DEBUG"))
            info_report("gsm-bts relay: not an SMS-SUBMIT");
        b->mt_state = VNB_MT_FAILED;
        b->mt_ack = b->mt_seq;
        return;
    }
    b->mt_state = VNB_MT_PAGING;
    b->mt_ack = b->mt_seq;
    n->sms_pending = true;
    n->mt_bridge = true;
    n->page_left = VN_PAGE_BLOCKS;
    n->paging = true;
}

/* ---------------- the relay link (Bridge, br_*) ---------------- */

static void br_put(Bridge *b, char type, const uint8_t *p, int n) {
    if (n > 255 || b->txn + 2 + n > (int) sizeof(b->tx))
        return;
    b->tx[b->txn++] = (uint8_t) type;
    b->tx[b->txn++] = (uint8_t) n;
    if (n)
        memcpy(&b->tx[b->txn], p, n);
    b->txn += n;
}

static void br_connected(Bridge *b) {
    memset(b, 0, sizeof(*b));
    b->up = 1;
}

static void br_disconnected(Bridge *b, VnBox *box) {
    b->up = 0;
    box->bridge = 0;
}

static void br_input(Bridge *b, VnBox *x, const uint8_t *d, int n) {
    while (n > 0) {
        int k = (int) sizeof(b->rx) - b->rxn;
        if (k > n)
            k = n;
        memcpy(&b->rx[b->rxn], d, k);
        b->rxn += k;
        d += k;
        n -= k;
        while (b->rxn >= 2 && b->rxn >= 2 + b->rx[1]) {
            int len = b->rx[1];
            const uint8_t *p = &b->rx[2];
            if (b->rx[0] == 'D' && len >= 1 && p[0] >= 2 && p[0] <= 12 && len - 1 - p[0] <= VN_SMS_MAX &&
                b->dn < BR_DQ) {
                BrSms *s = &b->dq[(b->dh + b->dn) % BR_DQ];
                memset(s->oa, 0, sizeof(s->oa));
                memcpy(s->oa, p + 1, p[0]);
                s->len = (uint8_t) (len - 1 - p[0]);
                memcpy(s->tpdu, p + 1 + p[0], s->len);
                b->dn++;
            } else if (b->rx[0] == 'q' && len <= VNB_USSD_A) {
                memcpy(b->ua, p, len);
                b->ua_len = (uint8_t) len;
                b->ua_pending = 1;
            } else if (b->rx[0] == 'V' && len == VNB_SP_BYTES) {
                /* pacing guard: the relay sends 50 frames a second of wall clock, the cell plays them at
                 * the guest's 50 a second. A guest slower than real time (the browser) would fill the
                 * ring and hear ever older speech: above SP_HIGH the oldest frame goes, so the delay
                 * stays under SP_HIGH frames and the loss is spread evenly */
                if ((uint8_t) (x->sp_w - x->sp_r) >= SP_HIGH) {
                    x->sp_r = (uint8_t) (x->sp_r + 1);
                    x->sp_drop++;
                }
                memcpy(x->sp[x->sp_w % VNB_SP_N], p, VNB_SP_BYTES);
                x->sp_w = (uint8_t) (x->sp_w + 1);
            } else if (b->rx[0] == 'C' && len >= 2 && len - 2 <= 13 && b->cn < BR_CQ) {
                VnbCc *e = &b->cq[(b->ch + b->cn) % BR_CQ];
                memset(e, 0, sizeof(*e));
                e->ev = p[0];
                e->cause = p[1];
                e->len = (uint8_t) (len - 2);
                memcpy(e->num, p + 2, e->len);
                b->cn++;
            } else if (b->rx[0] == 'N') {
                int m = len < (int) sizeof(b->number) - 1 ? len : (int) sizeof(b->number) - 1;
                memcpy(b->number, p, m);
                b->number[m] = 0;
                info_report("gsm-bts relay: the phone's number is %s", b->number);
            }
            memmove(b->rx, &b->rx[2 + len], b->rxn - 2 - len);
            b->rxn -= 2 + len;
        }
        if (b->rxn == (int) sizeof(b->rx))
            b->rxn = 0;
    }
}

static void br_poll(Bridge *b, VnBox *box, uint32_t now_ms) {
    if (!b->up)
        return;
    if (now_ms - b->last_tx_ms > BR_KEEPALIVE_MS) {
        br_put(b, 'P', 0, 0);
        b->last_tx_ms = now_ms;
    }
    box->bridge = 1;
    if (!b->hello && box->imsi[0] && box->imsi[0] <= 8) {
        br_put(b, 'H', box->imsi, 1 + box->imsi[0]);
        b->hello = 1;
        b->last_tx_ms = now_ms;
    }
    if (box->mo_seq != box->mo_ack && b->txn + 2 + box->mo_len <= (int) sizeof(b->tx)) {
        uint8_t seq = box->mo_seq;
        br_put(b, 'S', box->mo, box->mo_len <= VN_SMS_MAX ? box->mo_len : VN_SMS_MAX);
        box->mo_ack = seq;
        b->last_tx_ms = now_ms;
    }
    while (box->up_r != box->up_w && b->txn + 2 + 2 + 13 <= (int) sizeof(b->tx)) {
        const VnbCc *e = &box->up[box->up_r % VNB_CC_N];
        uint8_t f[2 + 13], n = e->len <= 13 ? e->len : 13;
        f[0] = e->ev;
        f[1] = e->cause;
        memcpy(f + 2, e->num, n);
        box->up_r = (uint8_t) (box->up_r + 1);
        br_put(b, 'C', f, 2 + n);
        b->last_tx_ms = now_ms;
    }
    if (box->uq_seq != box->uq_ack && b->txn + 2 + VNB_USSD_Q <= (int) sizeof(b->tx)) {
        uint8_t seq = box->uq_seq;
        br_put(b, 'Q', box->uq, box->uq_len <= VNB_USSD_Q ? box->uq_len : VNB_USSD_Q);
        box->uq_ack = seq;
        b->last_tx_ms = now_ms;
    }
    if (b->ua_pending) {
        memcpy(box->ua, b->ua, b->ua_len);
        box->ua_len = b->ua_len;
        box->ua_seq = (uint8_t) (box->ua_seq + 1);
        b->ua_pending = 0;
    }
    if ((uint8_t) (box->tr_w - b->tr_r) > VNB_TR_N)
        b->tr_r = (uint8_t) (box->tr_w - VNB_TR_N);
    while (b->tr_r != box->tr_w && b->txn + 2 + 16 <= (int) sizeof(b->tx)) {
        uint8_t f[16], n = box->tr[b->tr_r % VNB_TR_N].len;
        if (n > 14)
            n = 14;
        f[0] = box->tr[b->tr_r % VNB_TR_N].kind;
        memcpy(f + 1, box->tr[b->tr_r % VNB_TR_N].d, n);
        b->tr_r = (uint8_t) (b->tr_r + 1);
        br_put(b, 'T', f, 1 + n);
    }
    if ((uint8_t) (box->su_w - box->su_r) > VNB_SU_N)
        box->su_r = box->su_w;
    while (box->su_r != box->su_w && b->txn + 2 + VNB_SP_BYTES <= (int) sizeof(b->tx)) {
        br_put(b, 'U', box->su[box->su_r % VNB_SU_N], VNB_SP_BYTES);
        box->su_r = (uint8_t) (box->su_r + 1);
    }
    while (b->cn && (uint8_t) (box->dn_w - box->dn_r) < VNB_CC_N) {
        box->dn[box->dn_w % VNB_CC_N] = b->cq[b->ch];
        box->dn_w = (uint8_t) (box->dn_w + 1);
        b->ch = (b->ch + 1) % BR_CQ;
        b->cn--;
    }
    if (b->dn && !b->waiting && box->mt_seq == box->mt_ack) {
        BrSms *s = &b->dq[b->dh];
        memcpy(box->mt_oa, s->oa, sizeof(box->mt_oa));
        memcpy(box->mt, s->tpdu, s->len);
        box->mt_len = s->len;
        box->mt_seq = (uint8_t) (box->mt_seq + 1);
        b->dh = (b->dh + 1) % BR_DQ;
        b->dn--;
        b->waiting = 1;
        return;
    }
    if (b->waiting && box->mt_seq == box->mt_ack &&
        (box->mt_state == VNB_MT_DELIVERED || box->mt_state == VNB_MT_FAILED)) {
        uint8_t st = box->mt_state;
        br_put(b, 'R', &st, 1);
        b->waiting = 0;
        b->last_tx_ms = now_ms;
    }
}

/* ---------------- TCH/F ---------------- */

/* GSM 05.03 table 2 (libosmocore gsm610_bitorder): d-bit k = codec frame bit GSM610_BITORDER[k] */
static const uint16_t GSM610_BITORDER[GSM_TCHFS_BITS] = {
    0, 47, 103, 159, 215, 1, 6, 12, 2, 7, 13, 17, 36, 92, 148, 204, 48, 104, 160, 216, 8, 22, 26, 37, 93, 149, 205,
    38, 94, 150, 206, 39, 95, 151, 207, 40, 96, 152, 208, 49, 105, 161, 217, 3, 18, 30, 41, 97, 153, 209, 23, 27,
    43, 99, 155, 211, 42, 98, 154, 210, 45, 101, 157, 213, 4, 9, 14, 33, 19, 24, 31, 44, 100, 156, 212, 50, 106, 162,
    218, 53, 56, 59, 62, 65, 68, 71, 74, 77, 80, 83, 86, 89, 109, 112, 115, 118, 121, 124, 127, 130, 133, 136, 139,
    142, 145, 165, 168, 171, 174, 177, 180, 183, 186, 189, 192, 195, 198, 201, 221, 224, 227, 230, 233, 236, 239,
    242, 245, 248, 251, 254, 257, 46, 102, 158, 214, 51, 107, 163, 219, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84,
    87, 90, 110, 113, 116, 119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 166, 169, 172, 175, 178, 181, 184, 187,
    190, 193, 196, 199, 202, 222, 225, 228, 231, 234, 237, 240, 243, 246, 249, 252, 255, 258, 5, 10, 15, 28, 32,
    34, 35, 16, 20, 21, 25, 52, 108, 164, 220, 55, 58, 61, 64, 67, 70, 73, 76, 79, 82, 85, 88, 91, 111, 114, 117,
    120, 123, 126, 129, 132, 135, 138, 141, 144, 147, 167, 170, 173, 176, 179, 182, 185, 188, 191, 194, 197, 200,
    203, 223, 226, 229, 232, 235, 238, 241, 244, 247, 250, 253, 256, 259, 11, 29,
};

#define SP_PREFILL 16              /* frames in the ring before playing starts */

/* the next downlink speech block: a frame of the far end from the ring (relay 'V'), else an all-zero block
 * (the phone's receiver takes it as a bad frame = silence) */
static void vt_speech_code(VNet *n, uint8_t coded[GSM_XCCH_CODED_BITS]) {
    VnBox *b = &n->box;
    uint8_t fill = (uint8_t) (b->sp_w - b->sp_r), d[GSM_TCHFS_BITS];
    const uint8_t *f;

    if (fill > VNB_SP_N) {
        b->sp_r = (uint8_t) (b->sp_w - SP_PREFILL);
        fill = SP_PREFILL;
    }
    if (!n->sp_on && fill >= SP_PREFILL)
        n->sp_on = true;
    if (n->sp_on && fill == 0)
        n->sp_on = false;
    if (!n->sp_on) {
        memset(coded, 0, GSM_XCCH_CODED_BITS);
        return;
    }
    f = b->sp[b->sp_r % VNB_SP_N];
    b->sp_r = (uint8_t) (b->sp_r + 1);
    for (int k = 0; k < GSM_TCHFS_BITS; k++)
        d[k] = (f[k >> 3] >> (7 - (k & 7))) & 1;
    gsm_tchfs_code_bits(d, coded);
}

/* the phone's own speech (a good uplink frame) -> libgsm / RTP format into the su ring ('U' frames) */
static void vt_speech_up(VNet *n, const uint8_t d[GSM_TCHFS_BITS]) {
    VnBox *b = &n->box;
    uint8_t *o;

    if ((uint8_t) (b->su_w - b->su_r) >= VNB_SU_N)
        return;
    o = b->su[b->su_w % VNB_SU_N];
    memset(o, 0, VNB_SP_BYTES);
    o[0] = 0xD0;
    for (int k = 0; k < GSM_TCHFS_BITS; k++) {
        unsigned j = 4 + GSM610_BITORDER[k];
        if (d[k])
            o[j >> 3] |= (uint8_t) (0x80 >> (j & 7));
    }
    b->su_w = (uint8_t) (b->su_w + 1);
}

/* TCH frame index 0..23 of a 26-multiframe position (12 = SACCH, 25 = idle: -1) */
static int vt_index(uint32_t fn) {
    uint32_t m = fn % 26;
    if (m == 12 || m == 25)
        return -1;
    return m < 12 ? (int) m : (int) m - 1;
}

/* SACCH/T burst index of a frame at FN % 26 == 12 for our timeslot (05.02 7 table 1: TN 2-3 start at 38) */
static uint32_t vt_sacch_index(uint32_t fn) {
    return ((fn % 104) / 26 + 4 - GSM_RELAY_TCH_TS / 2) % 4;
}

static void vt_downlink(GSMRelay *r, uint32_t fn, GSMBurst *burst) {
    VTch *v = &r->tch;
    VNet *n = &r->net;
    int idx = vt_index(fn);

    n->heard = n->clk;
    if (fn % 26 == 25) {
        gsm_dummy_burst(burst);
        return;
    }
    if (idx < 0) {
        uint32_t q = vt_sacch_index(fn);
        uint32_t start = fn - 26 * q;
        if (!v->sacch_valid || v->sacch_fn != start) {
            uint8_t blk[GSM_XCCH_BLOCK_BYTES];
            gsm_bts_sacch_transmit(n->bts, blk);
            gsm_xcch_encode(blk, v->sacch);
            v->sacch_fn = start;
            v->sacch_valid = true;
        }
        gsm_normal_burst(burst, GSM_BTS_TSC, v->sacch[q]);
        return;
    }
    if (!v->dl_valid || v->dl_fn != fn - idx % 4) {
        /* a new 4-burst group: the even bits of the block starting here + the odd bits of the one before;
         * stealing flags hu (58) = this block, hl (57) = the one before (05.03 4.2) */
        uint8_t coded[GSM_XCCH_CODED_BITS], blk[GSM_XCCH_BLOCK_BYTES];
        bool steal = vn_facch_tx(n, blk);

        if (steal)
            gsm_xcch_code_bits(blk, coded);
        else
            vt_speech_code(n, coded);
        memcpy(v->burst, v->odd, sizeof(v->burst));
        for (uint32_t k = 0; k < GSM_XCCH_CODED_BITS; k++) {
            uint32_t b = k & 7;
            if (b < 4)
                v->burst[b][v->pos[k]] = coded[k];
            else
                v->odd[b - 4][v->pos[k]] = coded[k];
        }
        for (uint32_t b = 0; b < 4; b++) {
            v->burst[b][58] = steal;
            v->burst[b][57] = v->prev_steal;
        }
        v->prev_steal = steal;
        v->dl_fn = fn - idx % 4;
        v->dl_valid = true;
    }
    gsm_normal_burst(burst, GSM_BTS_TSC, v->burst[idx % 4]);
}

static void vt_uplink(GSMRelay *r, uint32_t fn, const GSMUplinkBurst *burst) {
    VTch *v = &r->tch;
    VNet *n = &r->net;
    int idx = vt_index(fn);
    uint32_t g;
    size_t start;

    if (!gsm_uplink_find_normal_burst(burst, GSM_BTS_TSC, &start))
        return;
    n->tch_ul = n->clk;
    if (idx < 0)
        return;                                          /* SACCH/T (measurement reports) and idle: not read */
    g = (fn / 26) * 6 + idx / 4;
    if (!v->ul_ok[0] || v->ul_g[0] != g) {
        v->ul_g[1] = v->ul_g[0];
        v->ul_ok[1] = v->ul_ok[0];
        memcpy(v->ul[1], v->ul[0], sizeof(v->ul[1]));
        v->ul_g[0] = g;
        v->ul_ok[0] = true;
        memset(v->ul[0], GSM_SOFT_BIT_UNKNOWN, sizeof(v->ul[0]));
    }
    gsm_uplink_normal_burst_data(burst, start, v->ul[0][idx % 4]);
    if (idx % 4 != 3 || !v->ul_ok[1] || v->ul_g[1] + 1 != g)
        return;
    /* the block that started in the group before is complete */
    {
        uint8_t soft[GSM_XCCH_CODED_BITS], blk[GSM_XCCH_BLOCK_BYTES], d[GSM_TCHFS_BITS];
        unsigned flag = 0;

        for (uint32_t k = 0; k < GSM_XCCH_CODED_BITS; k++) {
            uint32_t b = k & 7;
            soft[k] = b < 4 ? v->ul[1][b][v->pos[k]] : v->ul[0][b - 4][v->pos[k]];
        }
        for (uint32_t b = 0; b < 4; b++)
            flag += v->ul[1][b][58] + v->ul[0][b][57];
        if (flag >= 8 * 128) {
            if (gsm_xcch_decode_bits(soft, blk)) {
                v->ul_facch++;
                gsm_bts_gsmtap(n->bts, true, GSM_RELAY_TCH_TS, fn, GSMTAP_CHANNEL_TCH_F, blk, sizeof(blk));
                vn_rx(n, blk);
            } else {
                v->ul_bad++;
            }
        } else if (gsm_tchfs_decode_bits(soft, d)) {
            v->ul_speech++;
            vt_speech_up(n, d);
        } else {
            v->ul_bad++;
        }
    }
}

static void vt_start(GSMRelay *r) {
    VTch *v = &r->tch;

    memset(v, 0, sizeof(*v));
    for (uint32_t k = 0; k < GSM_XCCH_CODED_BITS; k++) {
        uint32_t bit = 2 * ((49 * k) % 57) + (k % 8) / 4;
        v->pos[k] = (uint8_t) (bit < 57 ? bit : bit + 2);
    }
    vn_tch_start(&r->net);
}

static void vt_check_go(GSMRelay *r, uint32_t fn) {
    VNet *n = &r->net;
    if (n->tch_go && !n->tch && (int32_t) (fn - n->tch_go_fn) >= 0)
        vt_start(r);
}

/* ---------------- gsm-bts hooks ---------------- */

static void gsm_relay_flush(GSMBts *bts) {
    Bridge *b = &bts->relay_core->br;
    if (b->txn) {
        if (b->up)
            qemu_chr_fe_write_all(&bts->relay_chr, b->tx, b->txn);
        b->txn = 0;
    }
}

static void gsm_relay_tick(void *opaque) {
    GSMBts *bts = opaque;
    GSMRelay *r = bts->relay_core;

    vn_poll(&r->net);
    br_poll(&r->br, &r->net.box, (uint32_t) qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    gsm_relay_flush(bts);
    timer_mod(r->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / VN_HZ);
}

static int gsm_relay_can_read(void *opaque) {
    return 4096;
}

static void gsm_relay_read(void *opaque, const uint8_t *buf, int size) {
    GSMBts *bts = opaque;
    GSMRelay *r = bts->relay_core;

    br_input(&r->br, &r->net.box, buf, size);
}

static void gsm_relay_event(void *opaque, QEMUChrEvent event) {
    GSMBts *bts = opaque;
    GSMRelay *r = bts->relay_core;

    switch (event) {
        case CHR_EVENT_OPENED:
            info_report("gsm-bts relay: connected");
            br_connected(&r->br);
            r->br.tr_r = r->net.box.tr_w;                   /* old trace entries are not news */
            break;
        case CHR_EVENT_CLOSED:
            info_report("gsm-bts relay: disconnected");
            br_disconnected(&r->br, &r->net.box);
            break;
        default:
            break;
    }
}

/* chardevs are created after -object gsm-bts: connect once the machine is up */
static void gsm_relay_connect(Notifier *notifier, void *_data) {
    GSMBts *bts = container_of(notifier, GSMBts, relay_done);
    Chardev *chr = qemu_chr_find(bts->relay);
    GSMRelay *r = bts->relay_core;

    if (!chr) {
        error_report("gsm-bts: no chardev '%s' for relay", bts->relay);
        exit(1);
    }
    if (!qemu_chr_fe_init(&bts->relay_chr, chr, &error_fatal))
        return;
    qemu_chr_fe_set_handlers(&bts->relay_chr, gsm_relay_can_read, gsm_relay_read, gsm_relay_event, NULL, bts,
        NULL, true);
    timer_mod(r->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / VN_HZ);
}

void gsm_relay_init(GSMBts *bts) {
    GSMRelay *r;

    r = g_new0(GSMRelay, 1);
    r->net.bts = bts;
    vn_link_reset(&r->net.l[0], 0);
    vn_link_reset(&r->net.l[1], 3);
    bts->relay_core = r;
    r->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gsm_relay_tick, bts);
    bts->relay_done.notify = gsm_relay_connect;
    qemu_add_machine_init_done_notifier(&bts->relay_done);
}

void gsm_relay_cleanup(GSMBts *bts) {
    GSMRelay *r = bts->relay_core;

    if (!r)
        return;
    if (bts->relay_done.notify)
        qemu_remove_machine_init_done_notifier(&bts->relay_done);
    qemu_chr_fe_deinit(&bts->relay_chr, false);
    timer_free(r->timer);
    g_free(r);
    bts->relay_core = NULL;
}

void gsm_relay_rach(GSMBts *bts, uint8_t ra) {
    VNet *n = &bts->relay_core->net;

    vn_channel_start(n);
    vn_trace(n, 'R', &ra, 1);
}

bool gsm_relay_ccch(GSMBts *bts, uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    VNet *n = &bts->relay_core->net;

    if (!n->page_left || !n->mi[0] || n->active)
        return false;
    vn_paging(n, block);
    if (getenv("GSM_RELAY_DEBUG"))
        info_report("gsm-bts relay: paging (clk %u, %u left)", n->clk, n->page_left);
    return true;
}

void gsm_relay_sdcch_tx(GSMBts *bts, uint32_t block_start, uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    VNet *n = &bts->relay_core->net;
    bool go = n->tch_go;

    if (n->tch) {                                        /* the phone is on the TCH: the SDCCH idles */
        memset(block, GSM_MACBLOCK_PADDING, GSM_XCCH_BLOCK_BYTES);
        vn_frame(block, 0x01, LAPDM_CONTROL_UI, 0, 0, false);
        return;
    }
    vn_sdcch_tx(n, block);
    if (n->tch_go && !go)
        n->tch_go_fn = gsm_bts_frame_number_add(block_start, GSM_XCCH_BLOCK_BURSTS);
}

void gsm_relay_sdcch_rx(GSMBts *bts, const uint8_t block[GSM_XCCH_BLOCK_BYTES]) {
    VNet *n = &bts->relay_core->net;

    if (!n->tch)
        vn_rx(n, block);
}

bool gsm_relay_downlink(GSMBts *bts, uint32_t frame_number, uint8_t timeslot, GSMBurst *burst) {
    GSMRelay *r = bts->relay_core;

    vt_check_go(r, frame_number);
    if (timeslot != GSM_RELAY_TCH_TS || !r->net.tch)
        return false;
    vt_downlink(r, frame_number, burst);
    return true;
}

bool gsm_relay_uplink(GSMBts *bts, uint32_t frame_number, uint8_t timeslot, const GSMUplinkBurst *burst) {
    GSMRelay *r = bts->relay_core;

    if (timeslot != GSM_RELAY_TCH_TS || !r->net.tch)
        return false;
    vt_uplink(r, frame_number, burst);
    return true;
}
