/*
 * In-package OPI PSRAM on the SF32LB525.
 *
 * The part carries 8 MB of octal PSRAM behind MPI1, mapped at 0x60000000 -
 * a different aperture from MPI1's NOR window at 0x10000000. Zephyr's SiFli
 * support has no PSRAM driver: dts/bindings/mtd only describes
 * sifli,sf32lb-mpi-qspi-nor, and nothing brings the controller up.
 *
 * Bringing it up is not something to reimplement from a register map. The
 * vendor HAL's HAL_MPI_PSRAM_Init() runs the delay calibration, sets the
 * clock divider and programs read/write latency against the measured bus
 * clock, so this driver's job is to enable the controller clock, hand the HAL
 * a description of the device, and let it work.
 *
 * The memory becomes usable as the linker region named by the devicetree
 * node's zephyr,memory-region property; nothing here allocates from it.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/sf32lb.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "sifli_hal_aliases.h"

#include <bf0_hal.h>
#include <bf0_hal_mpi.h>

LOG_MODULE_REGISTER(psram_sf32lb, CONFIG_APPLICATION_INIT_PRIORITY);

#define PSRAM_NODE DT_NODELABEL(psram)
#define PSRAM_CTRL_NODE DT_NODELABEL(mpi1)

#if DT_NODE_HAS_STATUS(PSRAM_NODE, okay)

static const struct sf32lb_clock_dt_spec psram_clock = SF32LB_CLOCK_DT_SPEC_GET(PSRAM_CTRL_NODE);

static FLASH_HandleTypeDef psram_handle;

static int psram_sf32lb_init(void)
{
    qspi_configure_t cfg = {
        .Instance = (MPI_TypeDef *)DT_REG_ADDR_BY_NAME(PSRAM_CTRL_NODE, ctrl),
        /* Consulted only by the NOR paths; the OPI PSRAM init ignores it. */
        .line = 4,
        .base = DT_REG_ADDR(PSRAM_NODE),
        .msize = DT_REG_SIZE(PSRAM_NODE) / (1024U * 1024U),
        .SpiMode = SPI_MODE_OPSRAM,
    };

    if (!sf32lb_clock_is_ready_dt(&psram_clock)) {
        LOG_ERR("MPI1 clock not ready");
        return -ENODEV;
    }

    int err = sf32lb_clock_control_on_dt(&psram_clock);

    if (err < 0) {
        LOG_ERR("failed to enable MPI1 clock: %d", err);
        return err;
    }

    /* Divider is a formality: the OPI path programs the clock itself. */
    if (HAL_MPI_PSRAM_Init(&psram_handle, &cfg, 1) != HAL_OK) {
        LOG_ERR("PSRAM init failed");
        return -EIO;
    }

    LOG_INF("%u MB OPI PSRAM at 0x%08lx", (unsigned int)cfg.msize, (unsigned long)cfg.base);
    return 0;
}

/*
 * Before any allocator that might be pointed at the region, and before the
 * application. PRE_KERNEL_1 keeps it ahead of everything in the driver
 * initialisation levels that could be given a PSRAM buffer.
 */
SYS_INIT(psram_sf32lb_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif /* DT_NODE_HAS_STATUS(PSRAM_NODE, okay) */
