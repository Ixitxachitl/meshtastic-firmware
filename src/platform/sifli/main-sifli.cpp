/*
 * main-sifli.cpp - Platform entry points for Nordic nRF54L15
 *
 * Adapted from src/platform/nrf52/main-nrf52.cpp.
 * SoftDevice, Adafruit BLE, and nRFCrypto are NOT available on nRF54L15.
 * Phase 2 will add proper BLE via Zephyr MPSL APIs.
 *
 * TODO items are marked with "TODO(sifli):"
 */

#include "configuration.h"
#include <SPI.h>
#include <Wire.h>
#include <assert.h>
#include <cstring>
#include <stdio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>

#include "NodeDB.h"
#include "Power.h"
#include "PowerMon.h"
#include "Router.h"
#include "error.h"
#include "main.h"
#include "mesh/MeshService.h"
#include "meshUtils.h"
#include <power/PowerHAL.h>

// ── Watchdog ──────────────────────────────────────────────────────────────
// TODO(sifli): nRF54L15 has a WDT peripheral but nrfx_wdt driver support
// may differ depending on the Zephyr SDK version. Enable once confirmed.
#define APP_WATCHDOG_SECS 90
static bool watchdog_running = false;

static inline void watchdog_feed() {} // TODO(sifli): replace with real WDT feed

// ── Weak variant hooks ────────────────────────────────────────────────────
void variant_shutdown() __attribute__((weak));
void variant_shutdown() {}

void variant_sifliLoopHook(void) __attribute__((weak));
void variant_sifliLoopHook(void) {}

// ── PowerHAL ─────────────────────────────────────────────────────────────
bool powerHAL_isVBUSConnected()
{
    // TODO(sifli): nRF54L15 has a USB POWER peripheral - read USBREGSTATUS
    return false;
}

bool powerHAL_isPowerLevelSafe()
{
    // TODO(sifli): implement SAADC VDD measurement similar to nRF52
    return true;
}

void powerHAL_platformInit()
{
    // TODO(sifli): configure POF comparator and analog reference if needed
}

// ── Utilities ─────────────────────────────────────────────────────────────
bool loopCanSleep()
{
    return !Serial;
}

void updateBatteryLevel(uint8_t level)
{
    (void)level;
}

void __attribute__((noreturn)) __assert_func(const char *file, int line, const char *func, const char *failedexpr)
{
    LOG_ERROR("assert failed %s: %d, %s, test=%s", file, line, func, failedexpr);
    NVIC_SystemReset();
}

// The SF32LB52x has no Zephyr hwinfo driver yet, so the BLE controller's
// identity address - programmed in efuse - is the fallback unique ID. It only
// becomes readable once bt_enable() has run, hence the cache.
static bool sifliUniqueId(uint8_t *out, size_t len)
{
    static uint8_t cached[8];
    static size_t cachedLen = 0;

    if (cachedLen == 0) {
        ssize_t rc = hwinfo_get_device_id(cached, sizeof(cached));
        if (rc > 0) {
            cachedLen = (size_t)rc;
        } else {
            bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
            size_t count = ARRAY_SIZE(addrs);
            bt_id_get(addrs, &count);
            if (count > 0) {
                memcpy(cached, addrs[0].a.val, sizeof(addrs[0].a.val));
                cachedLen = sizeof(addrs[0].a.val);
            }
        }
    }

    if (cachedLen == 0)
        return false;

    memset(out, 0, len);
    memcpy(out, cached, MIN(cachedLen, len));
    return true;
}

void getMacAddr(uint8_t *dmac)
{
    uint8_t id[8];
    if (sifliUniqueId(id, sizeof(id))) {
        memcpy(dmac, id, 6);
    } else {
        // Bring-up placeholder: every unprovisioned board reports the same
        // node number until a real unique ID source is wired up.
        static const uint8_t fallback[6] = {0xC2, 0x5F, 0x32, 0x52, 0x00, 0x01};
        memcpy(dmac, fallback, sizeof(fallback));
    }
    dmac[0] = (dmac[0] & 0xFE) | 0x02; // locally administered, unicast
}

bool getDeviceId(uint8_t *deviceId)
{
    uint8_t id[8];
    if (!sifliUniqueId(id, sizeof(id)))
        return false;
    memset(deviceId, 0, 16);
    memcpy(deviceId, id, sizeof(id));
    return true;
}

// ── Bluetooth ─────────────────────────────────────────────────────────────────

void setBluetoothEnable(bool enable)
{
    if (enable) {
        static bool initialized = false;
        if (!initialized) {
            sifliBluetooth = new SiFliBluetooth();
            sifliBluetooth->startDisabled();
            initialized = true;
        }
        if (sifliBluetooth) {
            sifliBluetooth->resumeAdvertising();
        }
    } else {
        if (sifliBluetooth) {
            sifliBluetooth->shutdown();
        }
    }
}

void clearBonds()
{
    if (!sifliBluetooth) {
        sifliBluetooth = new SiFliBluetooth();
        sifliBluetooth->setup();
    }
    sifliBluetooth->clearBonds();
}

void enterDfuMode()
{
    // TODO(sifli): nRF54L15 uses nRF Connect DFU (MCUboot/SUIT).
    // Trigger via Zephyr boot_request_upgrade() or similar.
    NVIC_SystemReset();
}

// ── printf via RTT ────────────────────────────────────────────────────────
// TODO(sifli): SEGGER_RTT may not be available with Zephyr; use printk()
// or a USB CDC console instead. Remove this override if it conflicts.
#ifdef SEGGER_RTT_PRINTF
int printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    auto res = SEGGER_RTT_vprintf(0, fmt, &args);
    va_end(args);
    return res;
}
#endif

// ── Deep sleep ────────────────────────────────────────────────────────────
void cpuDeepSleep(uint32_t msecToWake)
{
#if HAS_WIRE
    Wire.end();
#endif
    SPI.end();
    if (Serial)
        Serial.end();

    variant_shutdown();

    // TODO(sifli): use Zephyr pm_system_suspend() or WFI for proper low-power
    if (msecToWake != portMAX_DELAY) {
        delay(msecToWake);
        NVIC_SystemReset();
    } else {
        // System off equivalent - halt
        while (1) {
            __WFI();
        }
    }
}

// ── Setup / Loop ──────────────────────────────────────────────────────────
// Forward declaration - defined in SiFliBluetooth.cpp
void sifli_bt_preinit();

void sifliSetup()
{
    // nRF54L15 power peripheral layout differs from nRF52; RESETREAS not present here.
    // TODO(Phase 3): use zephyr/drivers/hwinfo.h hwinfo_get_reset_cause()
    LOG_DEBUG("Reset reason: (nRF54L15 power peripheral differs from nRF52, skipped)");

    // TODO(sifli): init SAADC, watchdog, and random seed via nrfx or Zephyr
    // For now seed with a fixed value; replace with hardware entropy source.
#if defined(NRF_FICR)
    randomSeed(analogRead(0) ^ (uint32_t)NRF_FICR->DEVICEADDR[0]);
#else
    randomSeed(analogRead(0));
#endif

    // Pre-initialize BT stack here on the main thread (CONFIG_MAIN_STACK_SIZE=8192).
    // bt_enable() overflows the smaller PowerFSMThread stack when called later.
    // SiFliBluetooth::setup() checks bt_initialized and skips bt_enable() if true.
    sifli_bt_preinit();
}

void sifliLoop()
{
    // First-call gate for the future WDT init - body will hold real init code, not just the bookkeeping flag.
    // cppcheck-suppress duplicateConditionalAssign
    if (!watchdog_running) {
        // TODO(sifli): enable WDT here
        watchdog_running = true;
    }
    watchdog_feed();

    variant_sifliLoopHook();
}
