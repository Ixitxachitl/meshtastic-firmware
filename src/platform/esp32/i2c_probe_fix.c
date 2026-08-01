/*
 * Workaround for an ESP-IDF bug in i2c_master_probe().
 *
 * Root cause: i2c_master_probe() uses a local 2-element ops array for the
 * probe transaction (START + STOP), but does NOT reset contains_read,
 * read_buf_pos, or read_len_static before use.  If a previous I2C read
 * transaction left contains_read=true (e.g. NACK during READ address phase
 * or a hardware timeout) and read_buf_pos >= 2, the probe ISR fires
 * i2c_isr_receive_handler() and dereferences
 *   i2c_trans.ops[read_buf_pos]  (the 2-element *local* array)
 * which is out of bounds.  The resulting garbage data pointer causes a
 * StoreProhibited fault in i2c_ll_read_rxfifo().
 *
 * Confirmed on ESP32-S3 with libesp_driver_i2c.a from Arduino-ESP32 55.03.39
 * (ESP-IDF 5.x).  The disassembly of i2c_master_probe shows writes to:
 *   bus_handle + 164 (trans_idx, s16i), + 195 (trans_done, s8i), + 196
 *   (bypass_nack_log, s8i), but NO writes to read_buf_pos/contains_read/
 *   read_len_static.
 *
 * Fix: wrap i2c_master_probe() with a thin shim that zeroes those three
 * fields before delegating to the real implementation.
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
 *     [ 24] i2c_transaction_t i2c_trans;    (12 B → ends at 36)
 *     [ 36] i2c_operation_t i2c_ops[N];     (16 B × SOC_I2C_CMD_REG_NUM)
 *     [36+N*16] _Atomic uint16_t trans_idx; (2 B + 2 pad)
 *     [36+N*16+4] SemaphoreHandle_t;        (4 B)
 *     [36+N*16+8] QueueHandle_t;            (4 B)
 *     [36+N*16+12] uint32_t read_buf_pos;   ← RESET
 *     [36+N*16+16] bool contains_read;      ← RESET (+ 3-byte pad)
 *     [36+N*16+20] uint32_t read_len_static;← RESET
 *     ...
 *   }
 *
 *   sizeof(i2c_operation_t) = 16 on all 32-bit ESP32 SoCs
 *     (hw_cmd 4B + data* 4B + bytes_used 2B + 2B pad + total_bytes 4B)
 *
 * SOC_I2C_CMD_REG_NUM values: 8 for ESP32-S3/C3/C6/H2/P4 (N=8 → offsets
 * 176/180/184), 16 for original ESP32 and ESP32-S2 (N=16 → 304/308/312).
 */

/*
 * Opt-in per board: a variant that needs this sets both
 *   -D MESHTASTIC_WRAP_I2C_MASTER_PROBE
 *   -Wl,--wrap=i2c_master_probe
 * in its platformio.ini. The two must be set together - the define without the
 * linker flag leaves __real_i2c_master_probe undefined.
 */
#ifdef MESHTASTIC_WRAP_I2C_MASTER_PROBE

#include "driver/i2c_master.h"
#include "esp_idf_version.h"
#include "soc/soc_caps.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * This pokes at fields of the private i2c_master_bus_t by hand-computed byte
 * offset, so it is only safe against layouts that have actually been checked.
 * If this fires, re-derive the offsets against the new i2c_private.h (and check
 * whether the underlying bug has been fixed upstream, in which case delete this
 * file and the two build flags) before widening the range.
 */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0) || ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 6, 0)
#error "i2c_probe_fix.c: i2c_master_bus_t layout not verified for this ESP-IDF version"
#endif

/* Layout below assumes 32-bit ESP32 SoCs; see the sizeof note further down. */
#if SOC_I2C_CMD_REG_NUM != 8 && SOC_I2C_CMD_REG_NUM != 16
#error "i2c_probe_fix.c: unexpected SOC_I2C_CMD_REG_NUM, re-check the field offsets"
#endif

/* Provided by the linker --wrap mechanism */
esp_err_t __real_i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms);

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

esp_err_t __wrap_i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms)
{
    if (bus_handle != NULL) {
        uint8_t *p = (uint8_t *)bus_handle;
        /* Reset stale read-transaction state left by a previous failed read. */
        *(volatile uint32_t *)(p + I2C_READ_BUF_POS_OFFSET) = 0;    /* read_buf_pos */
        *(volatile bool *)(p + I2C_CONTAINS_READ_OFFSET) = false;   /* contains_read */
        *(volatile uint32_t *)(p + I2C_READ_LEN_STATIC_OFFSET) = 0; /* read_len_static */
    }
    return __real_i2c_master_probe(bus_handle, address, xfer_timeout_ms);
}

#endif /* MESHTASTIC_WRAP_I2C_MASTER_PROBE */
