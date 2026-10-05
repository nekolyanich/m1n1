/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "wdt.h"
#include "adt.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

/* The block has three watchdogs (WD0 also barks with an IRQ), 0x10 apart */
#define WDT_N              3
#define WDT_CUR(n)         (0x10 * (n))
#define WDT_BITE(n)        (0x10 * (n) + 0x4)
#define WDT_CTRL(n)        (0x10 * (n) + 0xc)
#define WDT_CTRL_IRQ_EN    BIT(0)
#define WDT_CTRL_IRQ_STAT  BIT(1)
#define WDT_CTRL_RESET_EN  BIT(2)
#define WDT_CTRL_ARMED     (WDT_CTRL_IRQ_EN | WDT_CTRL_RESET_EN)

static u64 wdt_base = 0;
static u64 wdt_2nd = 0; // the secondary watchdog's control word (wdt-version 2/3)
static u32 wdt_saved_cur[WDT_N];
static u32 wdt_saved_ctrl[WDT_N];
static u32 wdt_saved_bite1;
static u32 wdt_saved_2nd;
static bool wdt_2nd_freeze = false;
static u32 wdt_shown[WDT_N + 1] = {~0u, ~0u, ~0u, ~0u};
static bool wdt_frozen = false;

/* The dead hand on WD1 while frozen: 170 s of the 24 MHz reference, close to the u32 limit */
#define WDT_DEADHAND_TICKS (170ULL * 24000000)

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
        if (adt_get_reg(adt, path, "reg", 2, &wdt_2nd, NULL)) {
            wdt_2nd = 0;
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
 * 90 s; Linux: WD1, 30 s). Disarm them for the stop and put the counts back on the
 * way out. For the stop itself WD1 becomes a dead hand that every proxy request
 * pets (wdt_pet()), so an HV that wedges inside the proxy still resets the machine.
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

    // the secondary watchdog m1n1 disables at boot (0 is off there): read always, and
    // switched off for the stop only when asked, its meaning under macOS is not known
    wdt_saved_2nd = wdt_2nd ? read32(wdt_2nd) : 0;
    if (wdt_saved_2nd && wdt_2nd_freeze)
        write32(wdt_2nd, 0);

    wdt_saved_bite1 = read32(wdt_base + WDT_BITE(1));
    write32(wdt_base + WDT_CUR(1), 0);
    write32(wdt_base + WDT_BITE(1), WDT_DEADHAND_TICKS);
    write32(wdt_base + WDT_CTRL(1), WDT_CTRL_RESET_EN);
    wdt_frozen = true;

    // which watchdogs the guest had armed, once per change (this runs on every trap)
    u32 now[WDT_N + 1] = {wdt_saved_ctrl[0], wdt_saved_ctrl[1], wdt_saved_ctrl[2], wdt_saved_2nd};
    if (memcmp(now, wdt_shown, sizeof(now))) {
        memcpy(wdt_shown, now, sizeof(now));
        printf("WDT freeze: ctrl WD0 %x WD1 %x WD2 %x, secondary %x, WD1 bite %x\n", now[0],
               now[1], now[2], now[3], wdt_saved_bite1);
    }
}

void wdt_set_2nd_freeze(bool on)
{
    wdt_2nd_freeze = on;
    printf("WDT: secondary watchdog %s during HV stops\n", on ? "switched off" : "only read");
}

void wdt_pet(void)
{
    if (wdt_frozen)
        write32(wdt_base + WDT_CUR(1), 0);
}

static void wdt_release_deadhand(void)
{
    write32(wdt_base + WDT_CTRL(1), 0);
    write32(wdt_base + WDT_BITE(1), wdt_saved_bite1);
    wdt_frozen = false;
}

/* The guest is gone: drop the dead hand and leave its watchdogs disarmed */
void wdt_drop(void)
{
    if (wdt_frozen)
        wdt_release_deadhand();
}

void wdt_thaw(void)
{
    if (!wdt_frozen)
        return;
    wdt_release_deadhand();

    if (wdt_saved_2nd && wdt_2nd_freeze)
        write32(wdt_2nd, wdt_saved_2nd);

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
