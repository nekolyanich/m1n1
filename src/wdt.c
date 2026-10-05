/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "wdt.h"
#include "adt.h"
#include "types.h"
#include "utils.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

/* The block has three watchdogs (WD0 also barks with an IRQ), 0x10 apart */
#define WDT_N              3
#define WDT_CUR(n)         (0x10 * (n))
#define WDT_CTRL(n)        (0x10 * (n) + 0xc)
#define WDT_CTRL_IRQ_EN    BIT(0)
#define WDT_CTRL_IRQ_STAT  BIT(1)
#define WDT_CTRL_RESET_EN  BIT(2)
#define WDT_CTRL_ARMED     (WDT_CTRL_IRQ_EN | WDT_CTRL_RESET_EN)

static u64 wdt_base = 0;
static u32 wdt_saved_cur[WDT_N];
static u32 wdt_saved_ctrl[WDT_N];
static bool wdt_frozen = false;

void wdt_disable(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);

    if (node < 0) {
        printf("WDT node not found!\n");
        return;
    }

    if (adt_get_reg(adt, path, "reg", 0, &wdt_base, NULL)) {
        printf("Failed to get WDT reg property!\n");
        return;
    }

    printf("Primary WDT register @ 0x%lx\n", wdt_base);
    write32(wdt_base + WDT_CTL, 0);
    printf("Primary WDT disabled\n");

    // disable secondary watchdog if wdt-version is 2 or 3
    u32 wdt_version;
    if (ADT_GETPROP(adt, node, "wdt-version", &wdt_version) < 0)
        return;

    if (wdt_version == 2 || wdt_version == 3) {
        u64 wdt_2nd = 0;
        if (adt_get_reg(adt, path, "reg", 2, &wdt_2nd, NULL)) {
            printf("Failed to get WDT reg[2] property!\n");
            return;
        }

        printf("Secondary WDT register @ 0x%lx\n", wdt_2nd);
        write32(wdt_2nd, 0);
        printf("Secondary WDT disabled\n");
    }
}

void wdt_reboot(void)
{
    if (!wdt_base)
        return;

    write32(wdt_base + WDT_ALARM, 0x100000);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, 4);
}

/*
 * The counters tick on wall time while the guest pets them on its own, stolen time:
 * a long stop in the HV proxy lets them bark (macOS: WD0 IRQ at 5 s) or bite (WD1,
 * 90 s). Disarm them for the stop and put the counts back on the way out.
 */
void wdt_freeze(void)
{
    if (!wdt_base || wdt_frozen)
        return;

    for (int i = 0; i < WDT_N; i++) {
        wdt_saved_ctrl[i] = read32(wdt_base + WDT_CTRL(i));
        if (!(wdt_saved_ctrl[i] & WDT_CTRL_ARMED))
            continue;
        write32(wdt_base + WDT_CTRL(i), 0);
        wdt_saved_cur[i] = read32(wdt_base + WDT_CUR(i));
    }
    wdt_frozen = true;
}

void wdt_thaw(void)
{
    if (!wdt_frozen)
        return;
    wdt_frozen = false;

    for (int i = 0; i < WDT_N; i++) {
        u32 ctrl = wdt_saved_ctrl[i];
        if (!(ctrl & WDT_CTRL_ARMED))
            continue;

        // If the count cannot be set back, a pet is the next best thing
        write32(wdt_base + WDT_CUR(i), wdt_saved_cur[i]);
        if (read32(wdt_base + WDT_CUR(i)) > (u64)wdt_saved_cur[i] + 24000)
            write32(wdt_base + WDT_CUR(i), 0);

        // IRQ_STAT is write-1-to-clear: drop a bark raised during the stop, keep an older one
        ctrl &= ~WDT_CTRL_IRQ_STAT;
        if (!(wdt_saved_ctrl[i] & WDT_CTRL_IRQ_STAT))
            ctrl |= WDT_CTRL_IRQ_STAT;
        write32(wdt_base + WDT_CTRL(i), ctrl);
    }
}
