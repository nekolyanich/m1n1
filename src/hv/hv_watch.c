/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The watcher: a CPU taken from the guest that polls AFK rings in shared memory
 * and copies whatever the producers write into a flight-recorder log, with
 * CNTPCT timestamps. The guest takes no trap for any of it. It only ever reads
 * RAM (every address is checked against the boot args' RAM range) and writes
 * its own log, which the host allocates outside the guest's memory.
 */

#include "hv.h"
#include "smp.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define WATCH_MAX_RINGS 32
#define WATCH_MAX_SEGS  16
#define WATCH_MAX_DATA  0x20000

#define WATCH_LOG_MAGIC 0x4c575648 // "HVWL"
#define WATCH_REC_MAGIC 0x43455257 // "WREC"

enum {
    WATCH_REC_PAD = 0,
    WATCH_REC_ADD = 1,   // ring armed: a = rptr, b = wptr, data = the whole data area
    WATCH_REC_WPTR = 2,  // producer moved: a = old wptr, b = new wptr, data = [old, new)
    WATCH_REC_RPTR = 3,  // consumer moved: a = old rptr, b = new rptr
    WATCH_REC_BAD = 4,   // ring header no longer makes sense, ring dropped: a = rptr, b = wptr
};

struct watch_seg {
    u64 pa;
    u64 len;
};

/* Written by the host, same layout as HV.watch_ring() packs */
struct hv_watch_desc {
    u32 id;
    u32 nsegs;
    u32 blk;     // AFK block: bufsize at 0, rptr at blk, wptr at 2 * blk, data at 3 * blk
    u32 bufsize; // bytes of data
    struct watch_seg segs[WATCH_MAX_SEGS];
};

struct hv_watch_log {
    u32 magic;
    u32 version;
    u64 size;   // bytes of record area after this header
    u64 head;   // bytes ever written; the record area is a ring
    u64 seq;    // records ever written
    u64 drops;  // records too big for the log
    u64 polls;  // watcher loop passes
    u64 cntfrq;
    u64 rsvd;
};

struct watch_rec {
    u32 magic;
    u16 type;
    u16 id;
    u64 seq;
    u64 ts;
    u32 a;
    u32 b;
    u32 len; // data bytes after this header; the record is padded to 8
    u32 rsvd;
};

struct watch_ring {
    struct hv_watch_desc d;
    bool fresh; // armed, the watcher has not logged it yet
    u32 rptr;
    u32 wptr;
};

static struct watch_ring watch_rings[WATCH_MAX_RINGS];
static volatile bool watch_ring_on[WATCH_MAX_RINGS];
static volatile bool hv_watch_active = false;
static int hv_watch_cpu = -1;
static struct hv_watch_log *watch_log;
static u8 *watch_area;
static u8 watch_buf[WATCH_MAX_DATA];

static bool watch_in_ram(u64 pa, u64 len)
{
    u64 lo = cur_boot_args.phys_base, hi = lo + cur_boot_args.mem_size;
    return pa >= lo && len <= hi - lo && pa <= hi - len;
}

static void watch_put(const void *src, u64 len)
{
    const u8 *p = src;
    while (len) {
        u64 off = watch_log->head % watch_log->size;
        u64 n = min(len, watch_log->size - off);
        memcpy(watch_area + off, p, n);
        watch_log->head += n;
        p += n;
        len -= n;
    }
}

static void watch_emit(int type, u32 id, u32 a, u32 b, const void *data, u32 len)
{
    struct watch_rec r = {
        .magic = WATCH_REC_MAGIC,
        .type = type,
        .id = id,
        .seq = watch_log->seq++,
        .ts = mrs(CNTPCT_EL0),
        .a = a,
        .b = b,
        .len = len,
    };
    u64 total = ALIGN_UP(sizeof(r) + len, 8);

    if (total > watch_log->size / 4) {
        watch_log->drops++;
        return;
    }

    static const u8 zero[8];
    watch_put(&r, sizeof(r));
    watch_put(data, len);
    watch_put(zero, total - sizeof(r) - len);
    sysop("dmb ish");
}

/* Copy [off, off + len) of the ring region, walking the physical segments */
static void watch_read(struct watch_ring *r, u64 off, void *dst, u64 len)
{
    u8 *d = dst;
    for (u32 i = 0; i < r->d.nsegs && len; i++) {
        struct watch_seg *s = &r->d.segs[i];
        if (off >= s->len) {
            off -= s->len;
            continue;
        }
        u64 n = min(len, s->len - off);
        memcpy(d, (void *)(s->pa + off), n);
        d += n;
        len -= n;
        off = 0;
    }
}

static u32 watch_read32(struct watch_ring *r, u64 off)
{
    u32 v;
    watch_read(r, off, &v, 4);
    return v;
}

/* [from, to) of the data area, wrapping at bufsize, into watch_buf */
static u32 watch_copy(struct watch_ring *r, u32 from, u32 to)
{
    u64 base = 3 * r->d.blk, bs = r->d.bufsize;
    u32 len = (to + bs - from) % bs;

    if (len > WATCH_MAX_DATA)
        len = WATCH_MAX_DATA;
    u32 first = min(len, bs - from);
    watch_read(r, base + from, watch_buf, first);
    if (len > first)
        watch_read(r, base, watch_buf + first, len - first);
    return len;
}

static void watch_poll(int i)
{
    struct watch_ring *r = &watch_rings[i];

    // only the watcher writes the log: it logs a newly armed ring itself
    if (r->fresh) {
        r->rptr = watch_read32(r, r->d.blk);
        r->wptr = watch_read32(r, 2 * r->d.blk);
        sysop("dmb ld");
        u32 len = min(r->d.bufsize, (u32)WATCH_MAX_DATA);
        watch_read(r, 3 * r->d.blk, watch_buf, len);
        watch_emit(WATCH_REC_ADD, r->d.id, r->rptr, r->wptr, watch_buf, len);
        r->fresh = false;
        return;
    }

    u32 wptr = watch_read32(r, 2 * r->d.blk);
    u32 rptr = watch_read32(r, r->d.blk);

    if (wptr >= r->d.bufsize || rptr >= r->d.bufsize) {
        watch_emit(WATCH_REC_BAD, r->d.id, rptr, wptr, NULL, 0);
        watch_ring_on[i] = false;
        return;
    }

    // the data before the pointer that announces it
    sysop("dmb ld");
    if (wptr != r->wptr) {
        u32 len = watch_copy(r, r->wptr, wptr);
        watch_emit(WATCH_REC_WPTR, r->d.id, r->wptr, wptr, watch_buf, len);
        r->wptr = wptr;
    }
    if (rptr != r->rptr) {
        watch_emit(WATCH_REC_RPTR, r->d.id, r->rptr, rptr, NULL, 0);
        r->rptr = rptr;
    }
}

static void hv_watch_main(void)
{
    while (hv_watch_active) {
        for (int i = 0; i < WATCH_MAX_RINGS; i++)
            if (watch_ring_on[i])
                watch_poll(i);
        watch_log->polls++;
        sysop("dmb ish");
    }
}

int hv_watch_start(int cpu, u64 log, u64 size)
{
    if (hv_watch_active || cpu == smp_id() || !smp_is_alive(cpu))
        return -1;
    if (size < 0x10000 || !watch_in_ram(log, size))
        return -1;

    watch_log = (void *)log;
    watch_area = (u8 *)log + sizeof(*watch_log);
    memset(watch_log, 0, sizeof(*watch_log));
    watch_log->magic = WATCH_LOG_MAGIC;
    watch_log->version = 1;
    watch_log->size = size - sizeof(*watch_log);
    watch_log->cntfrq = mrs(CNTFRQ_EL0);
    for (int i = 0; i < WATCH_MAX_RINGS; i++)
        watch_ring_on[i] = false;
    sysop("dsb ish");

    hv_watch_cpu = cpu;
    hv_watch_active = true;
    smp_call4(cpu, hv_watch_main, 0, 0, 0, 0);
    printf("HV watch: CPU %d, log %#lx (%ld bytes)\n", cpu, log, size);
    return 0;
}

/* Wait until the watcher has started a fresh pass, so it no longer looks at a ring */
static void watch_sync(void)
{
    u64 p = watch_log->polls;
    u64 t = timeout_calculate(100000);
    while (watch_log->polls < p + 2 && !timeout_expired(t))
        sysop("dmb ish");
}

/* Arm (or re-arm) the ring with the given id; desc == 0 drops it */
int hv_watch_add(u32 id, u64 desc)
{
    if (!hv_watch_active)
        return -1;

    int slot = -1;
    for (int i = 0; i < WATCH_MAX_RINGS; i++) {
        if (watch_ring_on[i] && watch_rings[i].d.id == id) {
            watch_ring_on[i] = false;
            slot = i;
        }
    }
    watch_sync();
    if (!desc)
        return 0;

    if (slot < 0)
        for (int i = 0; i < WATCH_MAX_RINGS; i++)
            if (!watch_ring_on[i]) {
                slot = i;
                break;
            }
    if (slot < 0)
        return -2;

    struct hv_watch_desc d;
    memcpy(&d, (void *)desc, sizeof(d));
    if (d.nsegs == 0 || d.nsegs > WATCH_MAX_SEGS || d.blk == 0 || d.bufsize == 0)
        return -3;
    u64 total = 0;
    for (u32 i = 0; i < d.nsegs; i++) {
        if (!watch_in_ram(d.segs[i].pa, d.segs[i].len))
            return -4;
        total += d.segs[i].len;
    }
    if (total < 3 * (u64)d.blk + d.bufsize)
        return -5;

    struct watch_ring *r = &watch_rings[slot];
    r->d = d;
    r->d.id = id;
    r->fresh = true;
    sysop("dsb ish");
    watch_ring_on[slot] = true;
    sysop("dsb ish");
    return 0;
}

void hv_watch_stop(void)
{
    if (!hv_watch_active)
        return;
    hv_watch_active = false;
    smp_wait(hv_watch_cpu);
    hv_watch_cpu = -1;
}
