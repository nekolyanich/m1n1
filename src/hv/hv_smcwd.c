/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * macOS' SMC-enforced watchdog (AppleARMWatchdogTimer::_armWatchdogMechanism):
 * every ~5 s it writes deadline = (uptime ns + 10 s) / 1e9 into one 32-bit word,
 * the "secondary" WDT word of /arm-io/wdt (reg[2]) that m1n1 zeroes at boot. The
 * uptime is the guest's, which lags real time by the time the HV stole; the SMC
 * compares with real time, so once more than ~10 s are stolen every deadline is
 * already past and the SMC resets the machine.
 *
 * So the guest's writes to that word are trapped: the hardware gets the deadline
 * plus the stolen time, rounded up to whole seconds, and reads give the guest its
 * own value back. During an HV stop the word is 0 (off, as m1n1 leaves it at boot);
 * on the way out it gets the recomputed deadline. Shifting only ever moves the
 * deadline later, so a guest that stops writing is still reset by the SMC.
 */

#include "hv.h"
#include "utils.h"
#include "wdt.h"

static u64 smcwd_reg;
static u32 smcwd_guest;
static bool smcwd_frozen;

static u32 smcwd_hw(u32 guest)
{
    if (!guest)
        return 0;
    u64 frq = mrs(CNTFRQ_EL0);
    return guest + (hv_stolen_time() + frq - 1) / frq;
}

static bool smcwd_access(struct exc_info *ctx, u64 ipa, u64 *val, bool write, int width)
{
    // the hook covers 8 bytes: anything but the 32-bit deadline word passes through
    if (ipa != smcwd_reg || width != 2)
        return hv_pa_rw(ctx, ipa, val, write, width);

    if (write) {
        smcwd_guest = *val;
        if (!smcwd_frozen)
            write32(smcwd_reg, smcwd_hw(smcwd_guest));
    } else {
        *val = smcwd_guest;
    }
    return true;
}

int hv_smcwd_hook(u64 addr)
{
    // only the word m1n1 itself knows as the secondary WDT (t6020: 0x29e2a002c, so
    // not 8-byte aligned: the hook covers the aligned 8 bytes around it)
    if (!addr || addr != wdt_secondary_addr() || (addr & 3))
        return -1;

    smcwd_reg = addr;
    smcwd_guest = read32(addr);
    if (hv_map_hook(addr & ~7UL, smcwd_access, 8) < 0) {
        smcwd_reg = 0;
        return -2;
    }
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    printf("HV: SMC watchdog deadline @%#lx follows the stolen time\n", addr);
    return 0;
}

void hv_smcwd_freeze(void)
{
    if (!smcwd_reg || smcwd_frozen)
        return;
    smcwd_frozen = true;
    write32(smcwd_reg, 0);
}

void hv_smcwd_thaw(void)
{
    if (!smcwd_reg || !smcwd_frozen)
        return;
    smcwd_frozen = false;
    write32(smcwd_reg, smcwd_hw(smcwd_guest));
}
