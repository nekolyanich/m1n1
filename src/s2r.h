/* SPDX-License-Identifier: MIT */

#ifndef S2R_H
#define S2R_H

#include "types.h"

/*
 * Native S2R for an OS at EL2 (tanabota notes/sleep/86, bench/native-1): the aura 30896625 mailbox, published in
 * /chosen/aurora,s2r-mailbox where iBoot locked every core's RVBAR to this image. The OS writes resume_pa (and
 * edge_cntpct right before its last WFI); a boot-core reset comes back here: a fast re-entry (< 3 s after the edge,
 * the gated WFI bouncing) is re-gated, a late one is the wake and goes to resume_pa after the wake work.
 */
#define S2R_MAGIC 0x3152325341525541ULL /* "AURAS2R1" */

struct s2r_mailbox {
    u64 magic;       /* 0x00 m1n1, at publish */
    u64 boot_mpidr;  /* 0x08 m1n1: the only core that may take resume_pa */
    u64 resume_pa;   /* 0x10 OS, cleaned to PoC before it sleeps; m1n1 clears it when it takes the wake */
    u64 resumes;     /* 0x18 m1n1: resume_pa taken this many times */
    u64 edge_cntpct; /* 0x20 OS: CNTPCT right before cpu0's last WFI; m1n1 clears it with resume_pa */
    u64 regates;     /* 0x28 m1n1: fast re-entries re-gated (cumulative per boot) */
};

extern struct s2r_mailbox s2r_mailbox;

/* s2r_native_flags (a variable: the bench may poke it over the proxy before kboot) */
#define S2R_F_CPUFREQ  BIT(0) /* the wake writes each cluster's +0x200f8 back as published */
#define S2R_F_AON      BIT(1) /* the wake replays macOS's 29 AON writes (the HV's AON_REPLAY) */
#define S2R_F_PCIE     BIT(2) /* the wake reruns pcie_init_controller(APCIE) (the HV's PCIE_REINIT) */
#define S2R_F_CRUMB    BIT(4) /* the wake writes PMU boot_stage 0x52 before resume_pa (the user's yes, 11-20) */
#define S2R_F_SEP_PS   BIT(5) /* the wake writes the SEP PS word back if the cut left DEV_DISABLE (JJRPF) */
#define S2R_F_RVBARS   BIT(6) /* the wake rewrites unlocked secondary RVBARs to this image (T4b) */
#define S2R_F_DEFAULT  (S2R_F_CPUFREQ | S2R_F_AON | S2R_F_PCIE | S2R_F_CRUMB)
extern volatile u64 s2r_native_flags;

/* the bench guest's boot skips as a native constant (bit numbers as BENCH_GUEST_* in the bench branches) */
#define S2R_SKIP_SLC      BIT(0)
#define S2R_SKIP_CPUFREQ  BIT(1)
#define S2R_SKIP_SMP      BIT(2)
#define S2R_SKIP_MITIG    BIT(3)
#define S2R_SKIP_ISP      BIT(4)
#define S2R_SKIP_RNG      BIT(5)
#define S2R_SKIP_DARTLOCK BIT(6)
#define S2R_SKIP_GPU      BIT(7)
#define S2R_SKIP_TUNABLES BIT(8)
#define S2R_SKIP_MCA      BIT(9)
#define S2R_SKIP_USB      BIT(10)
#define S2R_SKIP_PCIE     BIT(11)
#define S2R_SKIP_DAPF     BIT(12)
#define S2R_SKIP_WFE      BIT(13)
#define S2R_SKIP_N        14
/* the c1pr template's SLEEP_LINUX_GUEST_SKIP=slc,mitig,isp,rng,dartlock,tunables,mca,usb,wfe */
#define S2R_SKIP_DEFAULT                                                                                         \
    (S2R_SKIP_SLC | S2R_SKIP_MITIG | S2R_SKIP_ISP | S2R_SKIP_RNG | S2R_SKIP_DARTLOCK | S2R_SKIP_TUNABLES |       \
     S2R_SKIP_MCA | S2R_SKIP_USB | S2R_SKIP_WFE)
extern volatile u64 s2r_boot_skip;
bool s2r_skip(u64 bit);
void s2r_skip_report(void);

int s2r_dt_publish(void *dt);
bool s2r_published(void);
void s2r_reset_entry(void);
void s2r_return_to_os(void) __attribute__((noreturn));

#endif
