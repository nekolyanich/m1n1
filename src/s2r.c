/* SPDX-License-Identifier: MIT */

#include "s2r.h"
#include "adt.h"
#include "cpu_regs.h"
#include "cpufreq.h"
#include "heapblock.h"
#include "iodev.h"
#include "memory.h"
#include "pcie.h"
#include "smp.h"
#include "spmi.h"
#include "utils.h"
#include "wdt.h"
#include "xnuboot.h"

#include "libfdt/libfdt.h"

struct s2r_mailbox s2r_mailbox ALIGNED(64);
volatile u64 s2r_native_flags = S2R_F_DEFAULT;
volatile u64 s2r_boot_skip = S2R_SKIP_DEFAULT;
volatile u64 s2r_kboot_wd1_secs = S2R_KBOOT_WD1_SECS;

#define S2R_FAST_TICKS (3ULL * 24000000) /* the HV's SLEEP_RESUME_WAKE_WDT_THRESH=3 */
#define S2R_WD1_SECS   45                /* the wake's dead hand, until Linux's own watchdog takes over */
#define S2R_PMU_SID    0x0b
#define S2R_PMU_STAGE  0x6001
#define S2R_STAGE_WAKE 0x52

/* WD1 (the block's +0x10: count, +0x14 bite, +0x1c ctl): CTL 0, count 0, bite, reset enable. Also MMU off. */
static inline void s2r_wd1_arm(u64 wb, u32 secs)
{
    write32(wb + 0x1c, 0);
    write32(wb + 0x10, 0);
    write32(wb + 0x14, secs * 24000000U);
    write32(wb + 0x1c, 4);
}

/* everything the reset path needs, gathered at publish (MMU on) and cleaned to PoC: the reset path runs MMU off */
static struct {
    u64 wdt_base;
    u64 nf8;
    u64 f8_base[3];
    u64 f8_val[3];
    u64 pmgr1, pmgr5;
    u64 sep_ps;
    u32 sep_ps_boot;
    spmi_dev_t *spmi;
} st ALIGNED(64);

static const char *const s2r_skip_names[S2R_SKIP_N] = {
    "slc", "cpufreq", "smp", "mitig", "isp", "rng", "dartlock",
    "gpu", "tunables", "mca", "usb", "pcie", "dapf", "wfe",
};

bool s2r_skip(u64 bit)
{
    if (!(s2r_boot_skip & bit))
        return false;
    int i = __builtin_ctzl(bit);
    printf("s2r: boot skip: %s skipped (the bench guest's mask, notes/sleep/86 row 16)\n",
           i < S2R_SKIP_N ? s2r_skip_names[i] : "?");
    return true;
}

void s2r_skip_report(void)
{
    u64 f = s2r_boot_skip;
    printf("s2r: boot skip mask %#lx:", f);
    for (int i = 0; i < S2R_SKIP_N; i++)
        if (f & BIT(i))
            printf(" %s", s2r_skip_names[i]);
    printf("\n");
}

bool s2r_published(void)
{
    return s2r_mailbox.magic == S2R_MAGIC;
}

static void s2r_reserve(void *dt, u64 lo, u64 hi, const char *what)
{
    lo = ALIGN_DOWN(lo, SZ_16K);
    hi = ALIGN_UP(hi, SZ_16K);
    if (fdt_add_mem_rsv(dt, lo, hi - lo))
        printf("s2r: couldn't reserve %s %#lx..%#lx\n", what, lo, hi);
    else
        printf("s2r: reserved %s %#lx..%#lx\n", what, lo, hi);
}

/*
 * kboot, before /memory: publish the mailbox if a reset comes back here (every core's RVBAR locked to this image),
 * gather the wake's state, and keep this image, its heap and the ADT out of Linux's RAM (T0, notes/sleep/86 §3.0:
 * both lie inside the usable range kboot hands Linux).
 */
int s2r_dt_publish(void *dt)
{
    if (!in_el2()) {
        printf("s2r: no mailbox: m1n1 runs at EL%lu, not EL2\n", mrs(CurrentEL) >> 2);
        return 0;
    }
    if (!smp_resets_return_here()) {
        printf("s2r: no mailbox: the boot core's RVBAR doesn't lead back here, locked\n");
        return 0;
    }

    st.wdt_base = wdt_get_base();
    st.nf8 = cpufreq_cluster_bases(st.f8_base, 3);
    for (u64 i = 0; i < st.nf8; i++)
        st.f8_val[i] = read64(st.f8_base[i] + 0x200f8);

    int path[8];
    u64 sz;
    if (adt_path_offset_trace(adt, "/arm-io/pmgr", path) < 0 || adt_get_reg(adt, path, "reg", 1, &st.pmgr1, &sz) < 0 ||
        adt_get_reg(adt, path, "reg", 5, &st.pmgr5, &sz) < 0)
        st.pmgr1 = st.pmgr5 = 0;
    if (chip_id == T6020) { /* the SEP's PS word on j416s (hvlogs/j416s.adt, T0) */
        st.sep_ps = 0x290280c00;
        st.sep_ps_boot = read32(st.sep_ps) & ~BIT(10);
    }
    st.spmi = spmi_init("/arm-io/nub-spmi0");

    s2r_mailbox.magic = S2R_MAGIC;
    s2r_mailbox.boot_mpidr = mrs(MPIDR_EL1) & 0xFFFFFF;
    s2r_mailbox.resume_pa = 0;
    s2r_mailbox.resumes = 0;
    s2r_mailbox.edge_cntpct = 0;
    s2r_mailbox.regates = 0;
    dc_cvac_range(&s2r_mailbox, sizeof(s2r_mailbox));
    dc_cvac_range(&st, sizeof(st));
    dc_cvac_range((void *)&s2r_native_flags, sizeof(s2r_native_flags));
    sysop("dsb sy");

    u64 lo = (u64)_base, hi = (u64)heapblock_alloc(0);
    s2r_reserve(dt, lo, hi, "m1n1 image + heap");
    u64 a0 = (u64)adt, a1 = a0 + cur_boot_args.devtree_size;
    if (a0 < lo || a1 > hi)
        s2r_reserve(dt, a0, a1, "ADT");

    int node = fdt_path_offset(dt, "/chosen");
    if (node < 0 || fdt_setprop_u64(dt, node, "aurora,s2r-mailbox", (u64)&s2r_mailbox)) {
        printf("s2r: couldn't set aurora,s2r-mailbox\n");
        s2r_mailbox.magic = 0;
        dc_cvac_range(&s2r_mailbox, sizeof(s2r_mailbox));
        return -1;
    }
    printf("s2r: mailbox at %p (48 bytes), flags %#lx, wdt %#lx, %lu clusters' +0x200f8 %#lx %#lx %#lx, "
           "SEP PS %#lx = %#x\n",
           &s2r_mailbox, s2r_native_flags, st.wdt_base, st.nf8, st.f8_val[0], st.f8_val[1], st.f8_val[2], st.sep_ps,
           st.sep_ps ? read32(st.sep_ps) : 0);
    return 0;
}

/*
 * First thing in _cpu_reset_c, MMU off, nothing printed. Secondaries and resets the OS didn't arm go on. The boot
 * core within 3 s of the OS's edge is the gated WFI bouncing (pd196-198 on the bench): re-gate it quietly, as the
 * HV's wake detector does. A later entry is the wake: arm WD1 first, then _cpu_reset_c goes on to s2r_return_to_os.
 */
void s2r_reset_entry(void)
{
    volatile struct s2r_mailbox *m = &s2r_mailbox;
    if (m->magic != S2R_MAGIC || (mrs(MPIDR_EL1) & 0xFFFFFF) != m->boot_mpidr || !m->resume_pa)
        return;

    u64 edge = m->edge_cntpct;
    if (edge && mrs(CNTPCT_EL0) - edge < S2R_FAST_TICKS) {
        m->regates++;
        dc_civac_range((void *)m, sizeof(*m));
        sysop("dsb sy");
        reg_mask(SYS_IMP_APL_ACC_OVRD, ACC_OVRD_DIS_L2_FLUSH_ACC_SLEEP_MASK,
                 ACC_OVRD_PWR_DN_SRM(3) | ACC_OVRD_DIS_L2_FLUSH_ACC_SLEEP(2) | ACC_OVRD_TRAIN_DOWN_LINK(3) |
                     ACC_OVRD_POWER_DOWN_CPM(3) | ACC_OVRD_DISABLE_PIO_ON_WFI_CPU | ACC_OVRD_DEEP_SLEEP);
        u64 cyc = mrs(SYS_IMP_APL_CYC_OVRD) & ~(CYC_OVRD_FIQ_MODE_MASK | CYC_OVRD_IRQ_MODE_MASK |
                                                CYC_OVRD_WFI_MODE_MASK);
        msr(SYS_IMP_APL_CYC_OVRD, cyc | CYC_OVRD_WFI_MODE(3) | CYC_OVRD_DISABLE_WFI_RET);
        for (;;) {
            sysop("dsb sy");
            sysop("isb");
            sysop("wfi");
        }
    }

    if (st.wdt_base) /* the HV's late-entry fallback (start.S) */
        s2r_wd1_arm(st.wdt_base, S2R_WD1_SECS);
}

/*
 * The last thing kboot_boot does on a native boot (EL2; a guest m1n1 under the HV leaves the watchdogs to the HV):
 * a WD1 dead hand, so a Linux that dies before its watchdog runs still resets into the park (native-t2-b: WD1 off
 * since wdt_disable, no reset, power button). Linux's apple_wdt sees WD1 armed at probe (WDOG_HW_RUNNING), sets its
 * own timeout and pets it from the kernel until the test-initrd's busybox watchdog opens it; it never disables it.
 */
void s2r_kboot_deadhand(void)
{
    u64 secs = s2r_kboot_wd1_secs, wb = wdt_get_base();
    if (!in_el2() || !secs || !wb) {
        printf("s2r: no kboot dead hand (EL%lu, %lu s, wdt %#lx)\n", mrs(CurrentEL) >> 2, secs, wb);
        return;
    }
    if (secs > S2R_WD1_MAX_SECS)
        secs = S2R_WD1_MAX_SECS;
    s2r_wd1_arm(wb, secs);
    printf("s2r: kboot dead hand: WD1 bites in %lu s unless Linux's watchdog takes it over\n", secs);
}

/* the HV's v47 AON_REPLAY (pd314's macOS wake write log), as is */
static const struct {
    u8 reg;
    u32 off, val;
} s2r_aon_rep[] = {
    {1, 0x58, 0x1f0000ff},  {1, 0x60, 0x1400004f},  {1, 0x68, 0x1400004f}, {1, 0x70, 0x1f0000ff},
    {1, 0x80, 0x00ff},      {1, 0x80, 0x00ff},      {1, 0x80, 0x100000ff}, {1, 0x88, 0x1f00000f},
    {1, 0xb0, 0x1000000f},  {1, 0xb8, 0x1000200f},  {1, 0xc0, 0x1f0f00ff}, {1, 0xe8, 0x00ff},
    {1, 0xe8, 0x00ff},      {1, 0xe8, 0x100000ff},  {1, 0xf0, 0x1400004f}, {1, 0xf8, 0x00ff},
    {1, 0xf8, 0x00ff},      {1, 0xf8, 0x100000ff},  {1, 0x100, 0x1400004f}, {1, 0x118, 0x1000000f},
    {1, 0x120, 0x1400004f}, {1, 0x128, 0x1400004f}, {1, 0x130, 0x0000},    {1, 0x130, 0x000f},
    {1, 0x130, 0x1000000f}, {1, 0xc8, 0x03f0},      {1, 0x90, 0x03f0},     {1, 0x8000, 0x1f0003ff},
    {5, 0x40304, 0x10000000},
};

static void s2r_aon_replay(void)
{
    if (!st.pmgr1 || !st.pmgr5)
        return;
    for (u32 i = 0; i < ARRAY_SIZE(s2r_aon_rep); i++) {
        u64 a = (s2r_aon_rep[i].reg == 1 ? st.pmgr1 : st.pmgr5) + s2r_aon_rep[i].off;
        u32 v = s2r_aon_rep[i].val;
        write32(a, v);
        if (s2r_aon_rep[i].reg != 1)
            continue;
        u64 end = mrs(CNTPCT_EL0) + 240000; /* 10 ms */
        while ((((read32(a) >> 4) & 0xf) != (v & 0xf)) && mrs(CNTPCT_EL0) < end)
            ;
    }
    sysop("dsb sy");
}

/*
 * The boot core back from a sleep, with the OS live: after init_cpu and exception_initialize, interrupts masked.
 * No console (the OS owns the UART and the display). The wake work with the MMU on (ADT walks are unaligned), then
 * MMU and caches off, and resume_pa at EL2h as at a cold kboot. A reset the OS didn't arm reboots.
 */
void s2r_return_to_os(void)
{
    msr(DAIF, 0xf << 6);
    volatile struct s2r_mailbox *m = &s2r_mailbox;
    u64 entry = m->resume_pa;
    if (!entry || (mrs(MPIDR_EL1) & 0xFFFFFF) != m->boot_mpidr)
        reboot();

    for (int i = 0; i < IODEV_MAX; i++)
        iodev_set_usage(i, 0);

    mmu_wake_boot_cpu();
    u64 f = s2r_native_flags;

    if (f & S2R_F_CPUFREQ)
        for (u64 i = 0; i < st.nf8; i++)
            write64(st.f8_base[i] + 0x200f8, st.f8_val[i]);
    if (f & S2R_F_AON)
        s2r_aon_replay();
    if (f & S2R_F_PCIE)
        pcie_reinit_apcie();
    if ((f & S2R_F_SEP_PS) && st.sep_ps && (read32(st.sep_ps) & BIT(10)))
        write32(st.sep_ps, st.sep_ps_boot);
    if (f & S2R_F_RVBARS)
        smp_s2r_fix_rvbars();
    if ((f & S2R_F_CRUMB) && st.spmi) {
        u8 v = S2R_STAGE_WAKE;
        spmi_ext_write_long(st.spmi, S2R_PMU_SID, S2R_PMU_STAGE, &v, 1);
    }

    m->resume_pa = 0;
    m->edge_cntpct = 0;
    m->resumes++;
    mmu_disable(); /* caches off, everything cleaned by set/way */
    dc_civac_range((void *)m, sizeof(*m));
    sysop("dsb sy");
    sysop("ic iallu");
    sysop("isb");

    ((void (*)(void))entry)();
    reboot();
}
