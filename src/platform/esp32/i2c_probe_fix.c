/*
 * Workaround for an ESP-IDF bug in i2c_master_probe().
 *
 * Root cause: i2c_master_probe() points the transaction at a 2-element array
 * that lives on its own stack frame
 *
 *   i2c_operation_t i2c_ops[] = {{.hw_cmd = I2C_TRANS_START_COMMAND},
 *                                {.hw_cmd = I2C_TRANS_STOP_COMMAND}};
 *   bus_handle->i2c_trans = (i2c_transaction_t){ .ops = i2c_ops, ... };
 *
 * and resets cmd_idx, trans_idx, trans_done, status and bypass_nack_log - but
 * NOT contains_read, read_buf_pos or read_len_static.  Those are only cleared
 * on a read's normal completion path, so a read that aborts partway (NACK
 * during the read address phase, or a hardware timeout) leaves
 * contains_read = true with read_buf_pos >= 2.
 *
 * The ISR gates on that stale flag and then indexes with the stale position:
 *
 *   if (i2c_master->contains_read == true) { ... i2c_isr_receive_handler(); }
 *   i2c_operation_t *op = &i2c_master->i2c_trans.ops[i2c_master->read_buf_pos];
 *   i2c_ll_read_rxfifo(hal->dev, op->data + op->bytes_used, ...);
 *
 * ops[2] and beyond is adjacent stack memory reinterpreted as an
 * i2c_operation_t, so op->data is garbage and the RX FIFO contents get written
 * through it.  A StoreProhibited fault in i2c_ll_read_rxfifo() is the visible
 * symptom; silent memory corruption is the quieter one.
 *
 * Reachable on any ESP32 in this repo: Wire.endTransmission() with no payload
 * lands in i2cWrite(size == 0), which calls i2c_master_probe() directly
 * (cores/esp32/esp32-hal-i2c-ng.c), and that is exactly what ScanI2CTwoWire
 * does for every address at boot.
 *
 * Fix: wrap i2c_master_probe() with a shim that zeroes those three fields
 * before delegating to the real implementation.
 *
 * Field offsets inside i2c_master_bus_t (verified against i2c_private.h):
 *
 *   struct i2c_master_bus_t {
 *     [  0] i2c_bus_t *base;                (4 B)
 *     [  4] SemaphoreHandle_t bus_lock_mux; (4 B)
 *     [  8] int cmd_idx;                    (4 B)
 *     [ 12] _Atomic i2c_master_status_t;    (4 B)
 *     [ 16] i2c_master_event_t event;       (4 B)
 *     [ 20] int rx_cnt;                     (4 B)
 *     [ 24] i2c_transaction_t i2c_trans;    (12 B -> ends at 36)
 *     [ 36] i2c_operation_t i2c_ops[N];     (16 B x SOC_I2C_CMD_REG_NUM)
 *     [36+N*16] _Atomic uint16_t trans_idx; (2 B + 2 pad)
 *     [36+N*16+4] SemaphoreHandle_t;        (4 B)
 *     [36+N*16+8] QueueHandle_t;            (4 B)
 *     [36+N*16+12] uint32_t read_buf_pos;   <- RESET
 *     [36+N*16+16] bool contains_read;      <- RESET (+ 3-byte pad)
 *     [36+N*16+20] uint32_t read_len_static;<- RESET
 *     ...
 *   }
 *
 *   sizeof(i2c_operation_t) = 16 on all 32-bit ESP32 SoCs
 *     (hw_cmd 4B + data* 4B + bytes_used 2B + 2B pad + total_bytes 4B)
 *
 * SOC_I2C_CMD_REG_NUM values: 8 for ESP32-S3/C3/C6/H2/P4 (N=8 -> offsets
 * 176/180/184), 16 for original ESP32 and ESP32-S2 (N=16 -> 304/308/312).
 */

/*
 * Enabled from variants/esp32/esp32-common.ini, which sets both
 *   -D MESHTASTIC_WRAP_I2C_MASTER_PROBE
 *   -Wl,--wrap=i2c_master_probe
 * The two must stay together: the linker flag without this file leaves
 * __wrap_i2c_master_probe undefined, and this file without the linker flag
 * leaves __real_i2c_master_probe undefined.
 */
#ifdef MESHTASTIC_WRAP_I2C_MASTER_PROBE

#include "driver/i2c_master.h"
#include "esp_idf_version.h"
#include "soc/soc_caps.h"
#include <stdbool.h>
#include <stdint.h>

/* Provided by the linker --wrap mechanism */
esp_err_t __real_i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms);

/*
 * Not every target is affected.  The unguarded dereference above sits in the
 * `else` half of i2c_isr_receive_handler(), which i2c_master.c compiles under
 *   #if !SOC_I2C_STOP_INDEPENDENT
 * and SOC_I2C_STOP_INDEPENDENT is defined (as 1) only for the original ESP32 -
 * it is absent for S2/S3/C3/C6/H2/P4, where `#if !undefined` is therefore true.
 *
 * The surviving `if` half needs status == I2C_STATUS_READ, which a probe
 * (START + STOP, no read operation) never produces.  So the original ESP32 is
 * unaffected and only carries the pass-through below to satisfy the linker.
 */
#if !SOC_I2C_STOP_INDEPENDENT
#define MESHTASTIC_I2C_PROBE_NEEDS_FIXUP 1
#endif

#ifdef MESHTASTIC_I2C_PROBE_NEEDS_FIXUP

/*
 * The offsets below are hand-computed against a private struct, so they are
 * only safe for layouts that have actually been checked.  If this fires,
 * re-derive them from the new i2c_private.h - and first check whether the
 * driver bug has been fixed upstream, in which case delete this file and the
 * two build flags rather than widening the range.
 */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0) || ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 6, 0)
#error "i2c_probe_fix.c: i2c_master_bus_t layout not verified for this ESP-IDF version"
#endif

/* Layout below assumes 32-bit ESP32 SoCs; see the sizeof note above. */
#if SOC_I2C_CMD_REG_NUM != 8 && SOC_I2C_CMD_REG_NUM != 16
#error "i2c_probe_fix.c: unexpected SOC_I2C_CMD_REG_NUM, re-check the field offsets"
#endif

/* sizeof(i2c_operation_t) on every 32-bit ESP32 SoC */
#define I2C_OPERATION_SIZE 16

/* Byte offset of i2c_ops[0] inside i2c_master_bus_t */
#define I2C_OPS_FIELD_OFFSET 36

/* Byte offset of trans_idx (uint16_t) = end of i2c_ops array */
#define I2C_TRANS_IDX_OFFSET (I2C_OPS_FIELD_OFFSET + (SOC_I2C_CMD_REG_NUM)*I2C_OPERATION_SIZE)

/*
 * Offsets of the three fields that probe fails to reset.
 * Layout after trans_idx:  2B trans_idx + 2B pad + 4B cmd_semphr + 4B event_queue = +12
 * Then: read_buf_pos (4B), contains_read (bool 1B + 3B pad), read_len_static (4B)
 */
#define I2C_READ_BUF_POS_OFFSET (I2C_TRANS_IDX_OFFSET + 12)
#define I2C_CONTAINS_READ_OFFSET (I2C_READ_BUF_POS_OFFSET + 4)
#define I2C_READ_LEN_STATIC_OFFSET (I2C_CONTAINS_READ_OFFSET + 4)

#endif /* MESHTASTIC_I2C_PROBE_NEEDS_FIXUP */

esp_err_t __wrap_i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms)
{
#ifdef MESHTASTIC_I2C_PROBE_NEEDS_FIXUP
    if (bus_handle != NULL) {
        uint8_t *p = (uint8_t *)bus_handle;
        /* Reset stale read-transaction state left by a previous aborted read. */
        *(volatile uint32_t *)(p + I2C_READ_BUF_POS_OFFSET) = 0;    /* read_buf_pos */
        *(volatile bool *)(p + I2C_CONTAINS_READ_OFFSET) = false;   /* contains_read */
        *(volatile uint32_t *)(p + I2C_READ_LEN_STATIC_OFFSET) = 0; /* read_len_static */
    }
#endif
    return __real_i2c_master_probe(bus_handle, address, xfer_timeout_ms);
}

#endif /* MESHTASTIC_WRAP_I2C_MASTER_PROBE */
