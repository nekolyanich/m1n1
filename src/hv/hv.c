/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "hv.h"
#include "adt.h"
#include "assert.h"
#include "cpu_regs.h"
#include "display.h"
#include "gxf.h"
#include "hv_sprr.h"
#include "memory.h"
#include "pcie.h"
#include "smp.h"
#include "string.h"
#include "uart.h"
#include "usb.h"
#include "utils.h"
#include "wdt.h"

#define HV_TICK_RATE      1000
#define HV_SLOW_TICK_RATE 1

DECLARE_SPINLOCK(bhl);

void hv_enter_guest(u64 x0, u64 x1, u64 x2, u64 x3, void *entry);
void hv_exit_guest(void) __attribute__((noreturn));

extern char _hv_vectors_start[0];

u64 hv_tick_interval;
u64 hv_secondary_tick_interval;

int hv_pinned_cpu;
int hv_want_cpu;

enum hv_sptm_mode hv_sptm_mode = SPTM_NONE;

static bool hv_has_ecv;
static bool hv_should_exit[MAX_CPUS];
bool hv_started_cpus[MAX_CPUS];
u64 hv_cpus_in_guest;
u64 hv_saved_sp[MAX_CPUS];

static u64 hv_cpu_state_regs[MAX_CPUS];

/*
 * macOS 15 powers idle cores down (CYC_OVRD DISABLE_WFI_RET + WFI) and expects
 * them to come back through RVBAR on the next IPI or CPUSTART. A parked core is
 * taken out of the guest and re-entered at RVBAR when the guest wakes it.
 */
static bool hv_cpu_parked[MAX_CPUS];
static int hv_parked_ipis_logged[MAX_CPUS];
static u64 hv_cpu_wake_mask;

static bool hv_cpu_state_access(struct exc_info *ctx, u64 ipa, u64 *val, bool write, int width)
{
    if (width != 2 && width != 3)
        return false;
    if (write)
        return hv_pa_rw(ctx, ipa, val, write, width);

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        u64 base = hv_cpu_state_regs[cpu];
        if (!base)
            continue;
        if (ipa != base)
            continue;

        /* Clear out the lower byte when the core hasn't been started by the guest yet */
        if (!hv_pa_rw(ctx, ipa, val, write, width))
            return false;
        if (!hv_started_cpus[cpu])
            *val &= ~0xffUL;
        return true;
    }

    return false;
}

static bool hv_cpu_state_setup_hook(void)
{
    int node;

    memset(hv_cpu_state_regs, 0, sizeof(hv_cpu_state_regs));

    node = adt_path_offset(adt, "/cpus");
    if (node < 0)
        return false;

    ADT_FOREACH_CHILD(adt, node)
    {
        u32 cpu;
        u64 reg[2];

        if (ADT_GETPROP(adt, node, "cpu-id", &cpu) != sizeof(cpu))
            return false;
        if (ADT_GETPROP_ARRAY(adt, node, "cpu-impl-reg", reg) != sizeof(reg))
            return false;

        hv_cpu_state_regs[cpu] = reg[0] + 0x100;
        if (hv_map_hook(hv_cpu_state_regs[cpu], hv_cpu_state_access, 8) < 0)
            return false;
    }

    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    return true;
}

struct hv_secondary_info_t {
    uint64_t hcr;
    uint64_t hacr;
    uint64_t vtcr, vttbr;
    uint64_t mdcr;
    uint64_t mdscr;
    uint64_t amx_ctl;
    uint64_t apvmkeylo, apvmkeyhi, apsts;
    uint64_t actlr_el2;
    uint64_t actlr_el1;
    uint64_t cnthctl;
    uint64_t sprr_config;
    uint64_t gxf_config;
    uint64_t agt_cnt_rdir_el1;
    uint64_t agt_cnt_rdir_el12;
};

static struct hv_secondary_info_t hv_secondary_info;

u64 nvMem[MAX_CPUS][0x800] __attribute__((aligned(0x4000)));
u64 anvMem[MAX_CPUS][0x800] __attribute__((aligned(0x4000)));

static void hv_enable_sme_zt0(void)
{
    u64 sme = FIELD_GET(ID_AA64PFR1_SME, mrs(SYS_ID_AA64PFR1_EL1));
    if (sme != ID_AA64PFR1_SME2)
        return;

    /* Don't trap SME instructions to EL2 */
    reg_set(SYS_SMCR_EL2, SMCR_EZT0);
}

/* must be called before hv_init */
void hv_sptm_set_mode(enum hv_sptm_mode mode)
{
    printf("HV: SPTM mode %d\n", mode);
    hv_sptm_mode = mode;
}

void hv_init(void)
{
    pcie_shutdown();
    // Make sure we wake up DCP if we put it to sleep, just quiesce it to match ADT
    if (display_is_external && display_start_dcp() >= 0)
        display_shutdown(DCP_QUIESCED);
    // reenable hpm interrupts for the guest for unused iodevs
    usb_hpm_restore_irqs(0);
    smp_start_secondaries();
    smp_set_wfe_mode(true);
    hv_wdt_init();

    hv_pt_init();
    hv_sprr_init();
    hv_aic_init();

    // Configure hypervisor defaults
    hv_write_hcr(HCR_API | // Allow PAuth instructions
                 HCR_APK | // Allow PAuth key registers
                 HCR_TEA | // Trap external aborts
                 HCR_E2H | // VHE mode (forced)
                 HCR_RW |  // AArch64 guest
                 HCR_AMO | // Trap SError exceptions
                 HCR_VM);  // Enable stage 2 translation
    sysop("isb");

    if (hv_sptm_mode == SPTM_NESTED_VIRT) {
        printf("Enabling NV\n");
        hv_write_hcr(mrs(SYS_HCR_EL2) | HCR_NV2 | HCR_NV);
        msr(VNCR_EL2, &nvMem[0]);
        msr(SYS_IMP_APL_AVNCR_EL2, &anvMem[0]);
    }

    // No guest vectors initially
    msr(VBAR_EL12, 0);

    // Compute tick interval
    hv_tick_interval = mrs(CNTFRQ_EL0) / HV_TICK_RATE;

    hv_has_ecv = mrs(ID_AA64MMFR0_EL1) & (0xfULL << 60);

    if (hv_has_ecv) {
        printf("HV: ECV enabled\n");
        reg_set(CNTHCTL_EL2,
                CNTHCTL_EL1NVVCT | CNTHCTL_EL1NVPCT | CNTHCTL_EL1TVT | CNTHCTL_EL1PCTEN);
        hv_secondary_tick_interval = mrs(CNTFRQ_EL0) / HV_SLOW_TICK_RATE;
    } else {
        printf("HV: No ECV supported\n");
        // Enable physical timer for EL1
        msr(CNTHCTL_EL2, CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN);

        hv_secondary_tick_interval = hv_tick_interval;
    }

    // Set deep WFI back to defaults
    if (cpu_features->apple_sysregs_unlocked)
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK, CYC_OVRD_WFI_MODE(0));

    hv_enable_sme_zt0();

    sysop("dsb ishst");
    sysop("tlbi alle1is");
    sysop("dsb ish");
    sysop("isb");
}

static void hv_set_gxf_vbar(void)
{
    msr(SYS_IMP_APL_VBAR_GL1, _hv_vectors_start);
}

void hv_start(void *entry, u64 regs[4])
{
    if (boot_cpu_idx == -1) {
        printf("Boot CPU has not been found, can't start hypervisor\n");
        return;
    }

    if (!hv_cpu_power_init()) {
        printf("HV: CPU power protection hooks failed; can't start the hypervisor\n");
        return;
    }

    if (!hv_amcc_init()) {
        printf("HV: AMCC interrupt hook failed; can't start the hypervisor\n");
        return;
    }

    if (!hv_cpu_state_setup_hook()) {
        printf("CPU state hook setup failed, can't start the hypervisor\n");
        return;
    }

    memset(hv_should_exit, 0, sizeof(hv_should_exit));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));

    hv_started_cpus[boot_cpu_idx] = true;

    msr(VMPIDR_EL2, mrs(MPIDR_EL1));
    msr(VBAR_EL1, _hv_vectors_start);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

    hv_secondary_info.hcr = mrs(HCR_EL2);
    hv_secondary_info.hacr = mrs(HACR_EL2);
    hv_secondary_info.vtcr = mrs(VTCR_EL2);
    hv_secondary_info.vttbr = mrs(VTTBR_EL2);
    hv_secondary_info.mdcr = mrs(MDCR_EL2);
    hv_secondary_info.mdscr = mrs(MDSCR_EL1);
    hv_secondary_info.amx_ctl = mrs(SYS_IMP_APL_AMX_CTL_EL2);
    hv_secondary_info.apvmkeylo = mrs(SYS_IMP_APL_APVMKEYLO_EL2);
    hv_secondary_info.apvmkeyhi = mrs(SYS_IMP_APL_APVMKEYHI_EL2);
    hv_secondary_info.apsts = mrs(SYS_IMP_APL_APSTS_EL12);
    hv_secondary_info.actlr_el2 = mrs(ACTLR_EL2);
    if (cpu_features->actlr_el2)
        hv_secondary_info.actlr_el1 = mrs(SYS_ACTLR_EL12);
    else
        hv_secondary_info.actlr_el1 = mrs(SYS_IMP_APL_ACTLR_EL12);
    hv_secondary_info.cnthctl = mrs(CNTHCTL_EL2);
    hv_secondary_info.sprr_config = mrs(SYS_IMP_APL_SPRR_CONFIG_EL1);
    hv_secondary_info.gxf_config = mrs(SYS_IMP_APL_GXF_CONFIG_EL1);
    if (cpu_features->counter_redirect) {
        hv_secondary_info.agt_cnt_rdir_el1 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL1);
        hv_secondary_info.agt_cnt_rdir_el12 = mrs(SYS_IMP_APL_AGTCNTRDIR_EL12);
    }

    hv_arm_tick(false);
    hv_pinned_cpu = -1;
    hv_want_cpu = -1;
    hv_cpus_in_guest = BIT(smp_id());

    hv_enter_guest(regs[0], regs[1], regs[2], regs[3], entry);

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
    spin_lock(&bhl);

    hv_wdt_stop();
    hv_watch_stop();

    printf("HV: Exiting hypervisor (main CPU)\n");

    spin_unlock(&bhl);
    // Wait a bit for the guest CPUs to exit on their own if they are in the process.
    udelay(200000);
    spin_lock(&bhl);

    hv_started_cpus[boot_cpu_idx] = false;

    for (int i = 0; i < MAX_CPUS; i++) {
        if (i == boot_cpu_idx) {
            continue;
        }
        hv_should_exit[i] = true;
        if (hv_started_cpus[i]) {
            printf("HV: Waiting for CPU %d to exit\n", i);
            spin_unlock(&bhl);
            smp_wait(i);
            spin_lock(&bhl);
            hv_started_cpus[i] = false;
        }
    }

    printf("HV: All CPUs exited\n");
    spin_unlock(&bhl);
}

static void hv_init_secondary(struct hv_secondary_info_t *info)
{
    if (cpu_features->apple_sysregs_unlocked)
        gxf_init();

    msr(VMPIDR_EL2, mrs(MPIDR_EL1));
    msr(VBAR_EL1, _hv_vectors_start);

    msr(HCR_EL2, info->hcr);
    msr(HACR_EL2, info->hacr);
    msr(VTCR_EL2, info->vtcr);
    msr(VTTBR_EL2, info->vttbr);
    msr(MDCR_EL2, info->mdcr);
    msr(MDSCR_EL1, info->mdscr);

    if (cpu_features->apple_sysregs_unlocked) {
        msr(SYS_IMP_APL_AMX_CTL_EL2, info->amx_ctl);
        msr(SYS_IMP_APL_APVMKEYLO_EL2, info->apvmkeylo);
        msr(SYS_IMP_APL_APVMKEYHI_EL2, info->apvmkeyhi);
        msr(SYS_IMP_APL_APSTS_EL12, info->apsts);
    }
    msr(ACTLR_EL2, info->actlr_el2);
    if (cpu_features->actlr_el2)
        msr(SYS_ACTLR_EL12, info->actlr_el1);
    else
        msr(SYS_IMP_APL_ACTLR_EL12, info->actlr_el1);
    if (cpu_features->apple_sysregs_unlocked) {
        msr(SYS_IMP_APL_SPRR_CONFIG_EL1, info->sprr_config);
        /* GXF_CONFIG is UNDEF unless SPRR is enabled */
        if (info->sprr_config & SPRR_CONFIG_EN)
            msr(SYS_IMP_APL_GXF_CONFIG_EL1, info->gxf_config);
    }
    if (cpu_features->counter_redirect) {
        msr(SYS_IMP_APL_AGTCNTRDIR_EL1, info->agt_cnt_rdir_el1);
        msr(SYS_IMP_APL_AGTCNTRDIR_EL12, info->agt_cnt_rdir_el12);
    }

    if (hv_sptm_mode == SPTM_NESTED_VIRT) {
        msr(VNCR_EL2, &nvMem[smp_id()]);
        msr(SYS_IMP_APL_AVNCR_EL2, &anvMem[smp_id()]);
    }

    if (cpu_features->apple_sysregs_unlocked)
        reg_mask(SYS_IMP_APL_CYC_OVRD, CYC_OVRD_WFI_MODE_MASK, CYC_OVRD_WFI_MODE(0));

    hv_enable_sme_zt0();

    // For M3 and up, CNTHCTL_EL2 must be written after the counter redirection
    sysop("isb");
    msr(CNTHCTL_EL2, info->cnthctl);

    if (gxf_enabled())
        gl2_call(hv_set_gxf_vbar, 0, 0, 0, 0);

    hv_arm_tick(true);
}

static void hv_enter_secondary(void *entry, u64 regs[4])
{
    if (hv_cpu_parked[smp_id()]) {
        /* coming back from a guest power-down: the guest expects reset state */
        hv_cpu_parked[smp_id()] = false;
        msr(SYS_SCTLR_EL12, SCTLR_LSMAOE | SCTLR_nTLSMD | SCTLR_SPAN | SCTLR_EIS | SCTLR_TSCXT |
                                BIT(11) /* EOS */);
        sysop("isb");
    }

    hv_enter_guest(regs[0], regs[1], regs[2], regs[3], entry);

    spin_lock(&bhl);

    printf("HV: Exiting from CPU %d\n", smp_id());

    __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);

    hv_started_cpus[smp_id()] = false;
    spin_unlock(&bhl);
}

void hv_start_secondary(int cpu, void *entry, u64 regs[4])
{
    /* CPUSTART for a parked core: drop a wake the IPI path may have queued */
    __atomic_and_fetch(&hv_cpu_wake_mask, ~BIT(cpu), __ATOMIC_ACQ_REL);
    hv_should_exit[cpu] = false;

    printf("HV: Initializing secondary %d%s\n", cpu, hv_cpu_parked[cpu] ? " (parked)" : "");
    iodev_console_flush();

    if (hv_sptm_mode == SPTM_EMULATION)
        hv_sprr_init_secondary(cpu);

    if (!hv_cpu_parked[cpu])
        mmu_init_secondary(cpu);
    iodev_console_flush();
    smp_call4(cpu, hv_init_secondary, (u64)&hv_secondary_info, 0, 0, 0);
    smp_wait(cpu);
    iodev_console_flush();

    printf("HV: Entering guest secondary %d at %p\n", cpu, entry);
    hv_started_cpus[cpu] = true;
    __atomic_or_fetch(&hv_cpus_in_guest, BIT(cpu), __ATOMIC_ACQUIRE);

    iodev_console_flush();
    smp_call4(cpu, hv_enter_secondary, (u64)entry, (u64)regs, 0, 0);
}

void hv_exit_cpu(int cpu)
{
    if (cpu == -1)
        cpu = smp_id();

    printf("HV: Requesting exit of CPU#%d from the guest\n", cpu);
    hv_should_exit[cpu] = true;
}

bool hv_park_cpu(void)
{
    int cpu = smp_id();

    /* RVBAR is only readable with the Apple sysregs unlocked; never park the boot CPU */
    if (cpu == boot_cpu_idx || !cpu_features->apple_sysregs_unlocked || !hv_cpu_state_regs[cpu])
        return false;

    printf("HV: CPU %d powered down by the guest, parking\n", cpu);
    hv_parked_ipis_logged[cpu] = 0;
    hv_cpu_parked[cpu] = true;
    hv_should_exit[cpu] = true;
    return true;
}

/* Every IPI the guest aims at a parked core, the first few per park: does it ever try? */
void hv_log_parked_ipi(int cpu, u64 rr)
{
    if (cpu < 0 || cpu >= MAX_CPUS || !hv_cpu_parked[cpu])
        return;
    if (hv_parked_ipis_logged[cpu]++ < 16)
        printf("HV: IPI_RR %#lx from CPU %d to parked CPU %d\n", rr, smp_id(), cpu);
}

void hv_wake_cpu(int cpu)
{
    if (cpu >= 0 && cpu < MAX_CPUS && hv_cpu_parked[cpu])
        __atomic_or_fetch(&hv_cpu_wake_mask, BIT(cpu), __ATOMIC_ACQ_REL);
}

/* Called with bhl released: a core still on its way out takes bhl to finish */
void hv_wake_parked(void)
{
    static u64 zero_regs[4];

    /* on every exception exit: stay off the cache line unless a wake is queued */
    if (!__atomic_load_n(&hv_cpu_wake_mask, __ATOMIC_RELAXED))
        return;

    u64 mask = __atomic_exchange_n(&hv_cpu_wake_mask, 0, __ATOMIC_ACQ_REL);

    for (int cpu = 0; mask && cpu < MAX_CPUS; cpu++) {
        if (!(mask & BIT(cpu)))
            continue;
        mask &= ~BIT(cpu);

        smp_wait(cpu);
        if (!hv_cpu_parked[cpu] || hv_started_cpus[cpu])
            continue;

        void *entry = (void *)(read64(hv_cpu_state_regs[cpu] - 0x100) & 0xfffffffffffUL);
        printf("HV: waking parked CPU %d at %p\n", cpu, entry);

        hv_should_exit[cpu] = false;
        smp_call4(cpu, hv_init_secondary, (u64)&hv_secondary_info, 0, 0, 0);
        smp_wait(cpu);
        hv_started_cpus[cpu] = true;
        __atomic_or_fetch(&hv_cpus_in_guest, BIT(cpu), __ATOMIC_ACQUIRE);
        smp_call4(cpu, hv_enter_secondary, (u64)entry, (u64)zero_regs, 0, 0);
    }
}

/* DEBUG(macos15-nomon): what the last rendezvous saw */
u64 hv_rdv_sent, hv_rdv_ing0;
int hv_rdv_iters;

void hv_rendezvous(void)
{
    int timeout = 1000000;

    hv_rdv_sent = 0;
    hv_rdv_iters = 0;
    hv_rdv_ing0 = __atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE);
    if (!hv_rdv_ing0)
        return;

    /* IPI all CPUs. This might result in spurious IPIs to the guest... */
    for (int i = 0; i < MAX_CPUS; i++) {
        if (i != smp_id() && hv_started_cpus[i]) {
            smp_send_ipi(i);
            hv_rdv_sent |= BIT(i);
        }
    }

    while (timeout--) {
        hv_rdv_iters++;
        if (!__atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE))
            return;
    }

    hv_panic("HV: Failed to rendezvous, missing CPUs: 0x%lx (current: %d)\n",
             __atomic_load_n(&hv_cpus_in_guest, __ATOMIC_ACQUIRE), smp_id());
}

bool hv_switch_cpu(int cpu)
{
    if (cpu > MAX_CPUS || cpu < 0 || !hv_started_cpus[cpu]) {
        printf("HV: CPU #%d is inactive or invalid\n", cpu);
        return false;
    }
    printf("HV: switching to CPU #%d\n", cpu);
    hv_want_cpu = cpu;
    hv_rendezvous();
    return true;
}

void hv_pin_cpu(int cpu)
{
    hv_pinned_cpu = cpu;
}

void hv_write_hcr(u64 val)
{
    if (gxf_enabled() && !in_gl12())
        gl2_call(hv_write_hcr, val, 0, 0, 0);
    else
        msr(HCR_EL2, val);
}

void hv_write_hacr(u64 val)
{
    if (gxf_enabled() && !in_gl12())
        gl2_call(hv_write_hacr, val, 0, 0, 0);
    else
        msr(HACR_EL2, val);
}

u64 hv_get_spsr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_SPSR_GL1);
    else
        return mrs(SPSR_EL2);
}

void hv_set_spsr(u64 val)
{
    if (in_gl12())
        return msr(SYS_IMP_APL_SPSR_GL1, val);
    else
        return msr(SPSR_EL2, val);
}

u64 hv_get_esr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_ESR_GL1);
    else
        return mrs(ESR_EL2);
}

u64 hv_get_far(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_FAR_GL1);
    else
        return mrs(FAR_EL2);
}

u64 hv_get_afsr1(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_AFSR1_GL1);
    else
        return mrs(AFSR1_EL2);
}

u64 hv_get_elr(void)
{
    if (in_gl12())
        return mrs(SYS_IMP_APL_ELR_GL1);
    else
        return mrs(ELR_EL2);
}

void hv_set_elr(u64 val)
{
    if (in_gl12())
        return msr(SYS_IMP_APL_ELR_GL1, val);
    else
        return msr(ELR_EL2, val);
}

void hv_arm_tick(bool secondary)
{
    if (secondary)
        msr(CNTP_TVAL_EL0, hv_secondary_tick_interval);
    else
        msr(CNTP_TVAL_EL0, hv_tick_interval);
    msr(CNTP_CTL_EL0, CNTx_CTL_ENABLE);
    hv_sprr_timer_arm();
}

void hv_maybe_exit(void)
{
    if (hv_should_exit[smp_id()]) {
        /* hv_exc_exit counted us in the guest already: a rendezvous must not wait for us */
        __atomic_and_fetch(&hv_cpus_in_guest, ~BIT(smp_id()), __ATOMIC_ACQUIRE);
        hv_exit_guest();
    }
}

/*
 * Host heartbeat: while the guest runs, run_guest sends HV_HOST_HEARTBEAT every few
 * seconds on the proxy iodev. A host that has sent one and then stays silent for
 * HV_HOST_TIMEOUT_S (USB link wedged, run_guest gone) gets the machine rebooted, so
 * bench-guest can park it: neither the HV watchdog (the HV still ticks) nor the
 * guest's (the guest still pets it) catches that. Any other byte is a user interrupt.
 */
#define HV_HOST_HEARTBEAT '.'
#define HV_HOST_TIMEOUT_S 60

static volatile u64 hv_host_seen;
static volatile bool hv_host_beats;
static volatile bool hv_user_irq;

void hv_host_alive(void)
{
    hv_host_seen = mrs(CNTPCT_EL0);
}

void hv_tick(struct exc_info *ctx)
{
    hv_wdt_pet();
    iodev_handle_events(uartproxy_iodev);
    while (!hv_user_irq && iodev_can_read(uartproxy_iodev)) {
        u8 b;
        if (iodev_read(uartproxy_iodev, &b, 1) != 1)
            break;
        if (b == HV_HOST_HEARTBEAT) {
            hv_host_beats = true;
            hv_host_alive();
        } else {
            // the proxy syncs on its packet magic, so the byte itself is not needed
            hv_user_irq = true;
        }
    }
    if (hv_user_irq && (hv_pinned_cpu == -1 || hv_pinned_cpu == smp_id())) {
        hv_user_irq = false;
        printf("HV: User interrupt\n");
        iodev_console_flush();
        hv_exc_proxy(ctx, START_HV, HV_USER_INTERRUPT, NULL);
    }
    if (hv_host_beats && is_boot_cpu() &&
        mrs(CNTPCT_EL0) - hv_host_seen > HV_HOST_TIMEOUT_S * mrs(CNTFRQ_EL0)) {
        // nothing over USB: the link may be what died, and a console flush into it hung
        // here until the HV watchdog and the WD1 dead hand reset the machine 170 s later
        uart_puts("HV: no heartbeat from the host, rebooting");
        wdt_reboot();
        while (1)
            sysop("wfi");
    }
    hv_vuart_poll();
}

bool hv_is_nested(void)
{
    u64 hcr = mrs(HCR_EL2);
    return (hcr | HCR_NV2 | HCR_NV) == hcr;
}
