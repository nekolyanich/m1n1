/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * The watcher: a CPU taken from the guest that polls AFK rings in shared memory
 * and copies whatever the producers write into a flight-recorder log, with
 * CNTPCT timestamps. The guest takes no trap for any of it. It only ever reads
 * RAM (every address is checked against the boot args' RAM range) and writes
 * its own log, which the host allocates outside the guest's memory.
 */

#include "hv.h"
#include "memory.h"
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
    WATCH_REC_MMIO = 5,  // probe word changed: a = byte offset, b = new value
    WATCH_REC_GATE = 6,  // probe power changed: a = 1 on / 0 off, b = the PS value that decided
    WATCH_REC_NHI = 7,   // NHI ring descriptor changed: a = index, b = PA of the frame (low
                         // 32 bits, 0: untranslatable), data = 16-byte descriptor + frame
    WATCH_REC_NHI_ARM = 8, // NHI ring armed: a = descriptors, b = 0
    WATCH_REC_NHI_DB = 9,  // NHI doorbell: as NHI, taken in the trap on the guest's write
                           // of the ring index; RX: descriptor already re-posted (w2 is not
                           // the frame's), the frame itself is whole
    WATCH_REC_NHI_STAT = 10, // a = other traps on the doorbell page, b = doorbells so far,
                             // data = u32 {other, doorbells, skipped, first_assumed}
};

/*
 * NHI rings (USB4 control channel): descriptor arrays and frames live in RAM behind
 * the NHI's DART. A descriptor that changes is a frame posted (TX: by the AP) or
 * completed (RX: by the NHI); the frame is found by walking the DART's two-level
 * tables, which are RAM too. No register of the NHI or the DART is read here.
 */
#define WATCH_MAX_NHI 4
#define WATCH_NHI_MAX_DESC 1024
#define WATCH_NHI_FRAME 512

struct hv_watch_nhi_desc {
    u32 id;
    u32 ndesc;
    u64 desc_pa;  // the descriptor array, 16 bytes each, physically contiguous
    u64 l1[4];    // two levels: DART level-1 table per IOVA bits 37:36 (0: none)
    u64 pte_mask; // PTE bits holding the page number (PA >> 14)
    u64 l0;       // three levels (t8110 FOUR_LEVELS): the level-0 table, l1[] unused
    u64 rx;       // an RX ring: the NHI rewrites a descriptor for every frame it fills
    u64 doorbell; // the ring's prod/cons register (+8): trapped in C, no polling (0: poll)
};

/*
 * A TX ring's frames are followed by content as well: macOS sends every control
 * packet from the same buffer, and packets of one length leave the descriptors
 * unchanged. Up to WATCH_NHI_BUFS distinct buffers, each with the bytes last logged.
 */
#define WATCH_NHI_BUFS 8

struct watch_nhi_buf {
    u64 pa;
    u32 len;
    u32 slot; // the descriptor that last pointed here
    u8 shadow[WATCH_NHI_FRAME];
};

struct watch_nhi {
    struct hv_watch_nhi_desc d;
    bool fresh;
    u32 nbufs;
    u32 evict; // the next buffer to give up once all are taken
    bool have_last;
    u32 last;      // doorbell: the index the guest wrote last time
    u32 doorbells; // doorbell writes trapped
    u32 other;     // other accesses to the doorbell's register page, trapped too
    u32 skipped;   // writes to the doorbell word not understood (width, offset)
    u32 first_assumed; // 1: the first doorbell's batch was taken as one descriptor
    struct watch_nhi_buf bufs[WATCH_NHI_BUFS];
    u8 shadow[WATCH_NHI_MAX_DESC * 16];
};

static struct watch_nhi watch_nhis[WATCH_MAX_NHI];
static volatile bool watch_nhi_on[WATCH_MAX_NHI];
static u8 watch_nhi_rec[16 + WATCH_NHI_FRAME];

/*
 * MMIO probes: a few 32-bit registers read every period_us, logged only when they
 * change. A read of a block that is powered off or held in reset can hang the bus
 * or abort, so a probe has gates, read in order and all re-read right before every
 * word: (read32(addr) & mask) == value. PMGR PS registers come first (actual state
 * on: mask 0xf0, value 0xf0), so that a later gate in the same block, such as the
 * RUN bit of a co-processor, is itself only read while that block is powered.
 */
#define WATCH_MAX_PROBES 16
#define WATCH_PROBE_WORDS 16
#define WATCH_MAX_GATES 6

struct watch_gate {
    u64 addr;
    u32 mask;
    u32 value;
};

struct hv_watch_probe_desc {
    u32 id;
    u32 nwords;
    u32 period_us;
    u32 ngates;
    u64 addr;
    struct watch_gate gates[WATCH_MAX_GATES];
};

struct watch_probe {
    struct hv_watch_probe_desc d;
    u64 period;
    u64 next;
    bool on;
    bool valid;
    u32 vals[WATCH_PROBE_WORDS];
};

static struct watch_probe watch_probes[WATCH_MAX_PROBES];
static volatile bool watch_probe_on[WATCH_MAX_PROBES];

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
    u64 flags;  // WATCH_FLAG_*: the mode this log was taken in
};

#define WATCH_FLAG_MMIO BIT(0) // MMIO probes allowed (otherwise RAM rings only)

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
static bool watch_allow_mmio = false;
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
        // the host drains up to head while we run: the bytes before the head that covers them
        sysop("dmb ish");
        watch_log->head += n;
        p += n;
        len -= n;
    }
}

/* The watcher CPU and the guest CPUs (NHI doorbell traps) both append to the log */
static DECLARE_SPINLOCK(watch_lock);

static void watch_emit(int type, u32 id, u32 a, u32 b, const void *data, u32 len)
{
    spin_lock(&watch_lock);
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
        spin_unlock(&watch_lock);
        return;
    }

    static const u8 zero[8];
    watch_put(&r, sizeof(r));
    watch_put(data, len);
    watch_put(zero, total - sizeof(r) - len);
    sysop("dmb ish");
    spin_unlock(&watch_lock);
}

static void watch_fresh(u64 pa, u64 len);

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
        // the watcher runs cached now: what DCP or the guest wrote, not our stale line
        watch_fresh(s->pa + off, n);
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

/* All gates open, in order? *ps gets the first value that is not, or the last one read */
static bool watch_gates_on(struct watch_probe *p, u32 *ps)
{
    for (u32 i = 0; i < p->d.ngates; i++) {
        *ps = read32(p->d.gates[i].addr);
        if ((*ps & p->d.gates[i].mask) != p->d.gates[i].value)
            return false;
    }
    return true;
}

static void watch_probe_poll(int i, u64 now)
{
    struct watch_probe *p = &watch_probes[i];
    u32 ps = 0;

    if (now < p->next)
        return;
    p->next = now + p->period;

    for (u32 w = 0; w < p->d.nwords; w++) {
        bool on = watch_gates_on(p, &ps);
        if (on != p->on) {
            watch_emit(WATCH_REC_GATE, p->d.id, on, ps, NULL, 0);
            p->on = on;
        }
        if (!on) {
            // values seen after power comes back are all news
            p->valid = false;
            return;
        }
        u32 v = read32(p->d.addr + 4 * w);
        if (!p->valid || v != p->vals[w])
            watch_emit(WATCH_REC_MMIO, p->d.id, 4 * w, v, NULL, 0);
        p->vals[w] = v;
    }
    p->valid = true;
}

static u64 watch_pte_page(struct watch_nhi *n, u64 pte)
{
    u64 mask = n->d.pte_mask;
    return ((pte & mask) >> __builtin_ctzll(mask)) << 14;
}

/*
 * What the device and the guest wrote, not a stale line of ours: the guest maps
 * memory a non-coherent device uses uncached, while the watcher reads it cached.
 * Clean+invalidate is harmless, the watcher never writes these lines.
 */
static void watch_fresh(u64 pa, u64 len)
{
    dc_civac_range((void *)pa, len);
}

/* One DART table entry, valid, 0 otherwise */
static u64 watch_pte(u64 table, u64 index)
{
    u64 p = table + 8 * index;
    if (!watch_in_ram(p, 8))
        return 0;
    watch_fresh(p, 8);
    u64 pte = read64(p);
    return (pte & 1) ? pte : 0;
}

/* IOVA -> PA through the DART tables in RAM, 0 if not mapped */
static u64 watch_iova2pa(struct watch_nhi *n, u64 iova)
{
    u64 l1, pte;

    if (n->d.l0) {
        if (!(pte = watch_pte(n->d.l0, (iova >> 36) & 0x7ff)))
            return 0;
        l1 = watch_pte_page(n, pte);
    } else {
        l1 = n->d.l1[(iova >> 36) & 3];
    }
    if (!l1 || !(pte = watch_pte(l1, (iova >> 25) & 0x7ff)))
        return 0;
    if (!(pte = watch_pte(watch_pte_page(n, pte), (iova >> 14) & 0x7ff)))
        return 0;
    return watch_pte_page(n, pte) | (iova & 0x3fff);
}

/* Remember the bytes just logged for a TX buffer, so a change of them is news */
static void watch_nhi_track(struct watch_nhi *n, u64 pa, u32 len, u32 slot, const u8 *bytes)
{
    struct watch_nhi_buf *buf = NULL;
    for (u32 b = 0; b < n->nbufs; b++)
        if (n->bufs[b].pa == pa)
            buf = &n->bufs[b];
    if (!buf) {
        // full: the entry added first goes (FIFO, not LRU: a handful of buffers)
        if (n->nbufs < WATCH_NHI_BUFS) {
            buf = &n->bufs[n->nbufs++];
        } else {
            buf = &n->bufs[n->evict];
            n->evict = (n->evict + 1) % WATCH_NHI_BUFS;
        }
        buf->pa = pa;
    }
    buf->len = len;
    buf->slot = slot;
    memcpy(buf->shadow, bytes, len);
}

static void watch_nhi_poll(int i)
{
    struct watch_nhi *n = &watch_nhis[i];
    u8 desc[16];

    watch_fresh(n->d.desc_pa, 16 * n->d.ndesc);
    if (n->fresh) {
        memcpy(n->shadow, (void *)n->d.desc_pa, 16 * n->d.ndesc);
        watch_emit(WATCH_REC_NHI_ARM, n->d.id, n->d.ndesc, 0, NULL, 0);
        n->fresh = false;
        return;
    }

    for (u32 k = 0; k < n->d.ndesc; k++) {
        memcpy(desc, (void *)(n->d.desc_pa + 16 * k), 16);
        if (!memcmp(desc, n->shadow + 16 * k, 16))
            continue;
        memcpy(n->shadow + 16 * k, desc, 16);
        sysop("dmb ld");

        u64 iova = *(u64 *)desc;
        u32 len = (*(u32 *)(desc + 8)) & 0xfff;
        if (!len)
            len = 4096;
        len = min(len, (u32)WATCH_NHI_FRAME);
        u64 pa = watch_iova2pa(n, iova);
        // a frame never crosses its 16K page in the NHI's pool; never read past one
        if (pa && (pa & 0x3fff) + len > 0x4000)
            len = 0x4000 - (pa & 0x3fff);
        if (!pa || !watch_in_ram(pa, len))
            len = 0;

        memcpy(watch_nhi_rec, desc, 16);
        if (len) {
            watch_fresh(pa, len);
            memcpy(watch_nhi_rec + 16, (void *)pa, len);
            if (!n->d.rx)
                watch_nhi_track(n, pa, len, k, watch_nhi_rec + 16);
        }
        watch_emit(WATCH_REC_NHI, n->d.id, k, pa, watch_nhi_rec, 16 + len);
    }

    // TX buffers whose bytes changed under an unchanged descriptor: a new packet
    for (u32 b = 0; !n->d.rx && b < n->nbufs; b++) {
        struct watch_nhi_buf *buf = &n->bufs[b];
        watch_fresh(buf->pa, buf->len);
        if (!memcmp(buf->shadow, (void *)buf->pa, buf->len))
            continue;
        memcpy(buf->shadow, (void *)buf->pa, buf->len);
        memcpy(watch_nhi_rec, n->shadow + 16 * buf->slot, 16);
        memcpy(watch_nhi_rec + 16, buf->shadow, buf->len);
        watch_emit(WATCH_REC_NHI, n->d.id, buf->slot, buf->pa, watch_nhi_rec, 16 + buf->len);
    }
}

static void hv_watch_main(void)
{
    while (hv_watch_active) {
        for (int i = 0; i < WATCH_MAX_RINGS; i++)
            if (watch_ring_on[i])
                watch_poll(i);
        for (int i = 0; i < WATCH_MAX_NHI; i++)
            if (watch_nhi_on[i] && !watch_nhis[i].d.doorbell)
                watch_nhi_poll(i);
        u64 now = mrs(CNTPCT_EL0);
        for (int i = 0; i < WATCH_MAX_PROBES; i++)
            if (watch_probe_on[i])
                watch_probe_poll(i, now);
        watch_log->polls++;
        sysop("dmb ish");
    }
}

int hv_watch_start(int cpu, u64 log, u64 size, u64 flags)
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
    watch_log->flags = flags & WATCH_FLAG_MMIO;
    watch_allow_mmio = flags & WATCH_FLAG_MMIO;
    for (int i = 0; i < WATCH_MAX_RINGS; i++)
        watch_ring_on[i] = false;
    for (int i = 0; i < WATCH_MAX_PROBES; i++)
        watch_probe_on[i] = false;
    for (int i = 0; i < WATCH_MAX_NHI; i++)
        watch_nhi_on[i] = false;
    sysop("dsb ish");

    /*
     * The guest never starts this CPU (it is out of its ADT), so nothing turned its MMU
     * on: with it off every access is Device-nGnRnE, the log lock's exclusives are
     * unpredictable (that reset the machine) and every poll goes past the caches.
     */
    mmu_init_secondary(cpu);
    hv_watch_cpu = cpu;
    hv_watch_active = true;
    smp_call4(cpu, hv_watch_main, 0, 0, 0, 0);
    printf("HV watch: CPU %d, log %#lx (%ld bytes), %s\n", cpu, log, size,
           watch_allow_mmio ? "rings+mmio" : "rings only");
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

/* Arm (or re-arm) the NHI ring with the given id; desc == 0 drops it. RAM only. */
/*
 * NHI doorbells: the guest's write of a ring index, trapped in C on the guest's CPU.
 * TX: the new producer index; descriptors and frames up to it are complete. RX: the
 * new consumer index re-posts buffers; until this write reaches the NHI it cannot
 * refill them, so the frames the guest just took are still whole in them. The write
 * goes through to the NHI after the capture.
 */
/* Outside the log lock: the frame goes to the caller's buffer, only the append locks */
static void watch_nhi_capture(struct watch_nhi *n, u32 idx)
{
    u8 rec[16 + WATCH_NHI_FRAME];
    u64 dpa = n->d.desc_pa + 16 * idx;
    watch_fresh(dpa, 16);
    memcpy(rec, (void *)dpa, 16);

    u64 iova = *(u64 *)rec;
    u32 len = n->d.rx ? WATCH_NHI_FRAME : (*(u32 *)(rec + 8)) & 0xfff;
    if (!len)
        len = 4096;
    len = min(len, (u32)WATCH_NHI_FRAME);
    u64 pa = watch_iova2pa(n, iova);
    if (pa && (pa & 0x3fff) + len > 0x4000)
        len = 0x4000 - (pa & 0x3fff);
    if (!pa || !watch_in_ram(pa, len))
        len = 0;
    if (len) {
        watch_fresh(pa, len);
        memcpy(rec + 16, (void *)pa, len);
    }
    watch_emit(WATCH_REC_NHI_DB, n->d.id, idx, pa, rec, 16 + len);
}

static void watch_nhi_stat(struct watch_nhi *n)
{
    u32 s[4] = {n->other, n->doorbells, n->skipped, n->first_assumed};
    watch_emit(WATCH_REC_NHI_STAT, n->d.id, n->other, n->doorbells, s, sizeof(s));
}

/*
 * The hook covers the ring's register page from the doorbell on (the base, +0..+7,
 * is Python's arming hook), so every other access to that page comes through here
 * too and is counted: the page traps for the guest either way once part of it does.
 */
static bool watch_nhi_doorbell(struct exc_info *ctx, u64 ipa, u64 *val, bool write, int width)
{
    struct watch_nhi *n = NULL;
    u64 page = ipa & ~0x3fffUL;
    // only a live ring: a stopped one keeps its doorbell in a slot that a restart of the
    // same ring may not reuse, and picking that one dropped every frame after a replug
    for (int i = 0; i < WATCH_MAX_NHI; i++)
        if (watch_nhi_on[i] && watch_nhis[i].d.doorbell &&
            (watch_nhis[i].d.doorbell & ~0x3fffUL) == page)
            n = &watch_nhis[i];

    /*
     * The doorbell word (+8): cons in bits 15:0 (RX), prod in bits 31:16 (TX). Linux
     * writes it 32-bit; taken also: RX 16-bit at +8, TX 16-bit at +10, 64-bit at +8
     * (the size word above it rides along). Any other write into the word is counted
     * as skipped, so a guest writing it some other way shows up instead of going quiet.
     */
    bool doorbell = false;
    u32 idx = 0;
    if (n && write && ipa >= n->d.doorbell && ipa < n->d.doorbell + 4) {
        u64 off = ipa - n->d.doorbell;
        if (off == 0 && (width == 2 || width == 3 || (width == 1 && n->d.rx))) {
            doorbell = true;
            idx = n->d.rx ? (*val & 0xffff) : ((*val >> 16) & 0xffff);
        } else if (off == 2 && width == 1 && !n->d.rx) {
            doorbell = true;
            idx = *val & 0xffff;
        } else {
            n->skipped++;
            watch_nhi_stat(n);
        }
    }
    if (n && !doorbell) {
        // not the doorbell: count, and log the count when it crosses a power of two
        u32 c = ++n->other;
        if (!(c & (c - 1)) || !(c & 0xfff))
            watch_nhi_stat(n);
    }
    if (doorbell && watch_nhi_on[n - watch_nhis]) {
        n->doorbells++;
        // ring size (+12, the next word): the NHI is up, the guest is writing to it
        u32 size = read32(n->d.doorbell + 4) & 0xffff;
        if (size && size <= n->d.ndesc && idx < size) {
            // only the index bookkeeping under the lock; frames are read outside it
            spin_lock(&watch_lock);
            u32 from = n->have_last ? n->last : (idx + size - 1) % size;
            if (!n->have_last) {
                // where the guest stood before is unknown: a first batch shows its last only
                n->first_assumed = 1;
                watch_nhi_stat(n);
            }
            n->last = idx;
            n->have_last = true;
            spin_unlock(&watch_lock);
            for (u32 k = 0; from != idx && k < size; k++) {
                watch_nhi_capture(n, from);
                from = (from + 1) % size;
            }
        }
    }
    return hv_pa_rw(ctx, ipa, val, write, width);
}

int hv_watch_nhi(u32 id, u64 desc)
{
    if (!hv_watch_active)
        return -1;

    int slot = -1;
    bool rearm = false;
    for (int i = 0; i < WATCH_MAX_NHI; i++) {
        if (watch_nhi_on[i] && watch_nhis[i].d.id == id) {
            // the final counts of the ring as it was, exact, not just their last power of 2
            watch_nhi_stat(&watch_nhis[i]);
            watch_nhi_on[i] = false;
            slot = i;
        }
    }
    watch_sync();
    // a stopped earlier run of this ring, in whatever slot: forget it, a restart is new
    for (int i = 0; i < WATCH_MAX_NHI; i++) {
        if (!watch_nhi_on[i] && watch_nhis[i].d.ndesc && watch_nhis[i].d.id == id) {
            rearm = true;
            if (slot < 0)
                slot = i;
            if (i != slot)
                watch_nhis[i].d.doorbell = 0;
        }
    }
    if (slot >= 0)
        rearm = true;
    if (!desc)
        return 0;

    if (slot < 0)
        for (int i = 0; i < WATCH_MAX_NHI; i++)
            if (!watch_nhi_on[i]) {
                slot = i;
                break;
            }
    if (slot < 0)
        return -2;

    struct hv_watch_nhi_desc d;
    memcpy(&d, (void *)desc, sizeof(d));
    if (!d.ndesc || d.ndesc > WATCH_NHI_MAX_DESC || !d.pte_mask ||
        !watch_in_ram(d.desc_pa, 16 * d.ndesc))
        return -3;
    for (int i = 0; i < 4; i++)
        if (d.l1[i] && !watch_in_ram(d.l1[i], 0x4000))
            return -4;
    if (d.l0 && !watch_in_ram(d.l0, 0x4000))
        return -4;
    // the doorbell is an NHI register, never RAM; the hook runs to the end of its page
    if (d.doorbell && ((d.doorbell & 7) || watch_in_ram(d.doorbell, 8)))
        return -5;

    struct watch_nhi *n = &watch_nhis[slot];
    n->d = d;
    n->d.id = id;
    n->nbufs = 0;
    n->evict = 0;
    n->have_last = false;
    n->doorbells = 0;
    n->other = 0;
    n->skipped = 0;
    n->first_assumed = 0;
    n->fresh = true;
    if (d.doorbell) {
        u64 end = (d.doorbell | 0x3fffUL) + 1;
        if (hv_map_hook(d.doorbell, watch_nhi_doorbell, end - d.doorbell) < 0)
            return -6;
        sysop("dsb ishst");
        sysop("tlbi vmalls12e1is");
        sysop("dsb ish");
        sysop("isb");
    }
    sysop("dsb ish");
    watch_nhi_on[slot] = true;
    sysop("dsb ish");
    // a doorbell ring is never polled, so its arming is logged here: b = 1 for a re-arm
    if (d.doorbell)
        watch_emit(WATCH_REC_NHI_ARM, id, d.ndesc, rearm, NULL, 0);
    return 0;
}

/* Arm (or re-arm) the MMIO probe with the given id; desc == 0 drops it */
int hv_watch_probe(u32 id, u64 desc)
{
    if (!hv_watch_active)
        return -1;
    // a watcher started for RAM rings only never touches a register
    if (!watch_allow_mmio)
        return -6;

    int slot = -1;
    for (int i = 0; i < WATCH_MAX_PROBES; i++) {
        if (watch_probe_on[i] && watch_probes[i].d.id == id) {
            watch_probe_on[i] = false;
            slot = i;
        }
    }
    watch_sync();
    if (!desc)
        return 0;

    if (slot < 0)
        for (int i = 0; i < WATCH_MAX_PROBES; i++)
            if (!watch_probe_on[i]) {
                slot = i;
                break;
            }
    if (slot < 0)
        return -2;

    struct hv_watch_probe_desc d;
    memcpy(&d, (void *)desc, sizeof(d));
    // never ungated, never RAM (rings are for that), never more than a handful of words
    if (d.ngates == 0 || d.ngates > WATCH_MAX_GATES || d.nwords == 0 ||
        d.nwords > WATCH_PROBE_WORDS || d.period_us < 10 || (d.addr & 3) ||
        watch_in_ram(d.addr, 4 * d.nwords))
        return -3;
    for (u32 i = 0; i < d.ngates; i++)
        if ((d.gates[i].addr & 3) || watch_in_ram(d.gates[i].addr, 4) || !d.gates[i].mask)
            return -4;

    struct watch_probe *p = &watch_probes[slot];
    p->d = d;
    p->d.id = id;
    p->period = mrs(CNTFRQ_EL0) * d.period_us / 1000000;
    p->next = 0;
    p->on = false;
    p->valid = false;
    sysop("dsb ish");
    watch_probe_on[slot] = true;
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
