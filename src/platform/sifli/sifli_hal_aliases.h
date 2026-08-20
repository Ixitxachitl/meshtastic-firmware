#pragma once

// Restores the short register aliases that sifli_soc_aliases.h retires.
//
// Only the vendor HAL sources want them. Attached to those files through
// source-level COMPILE_OPTIONS, which land after the target's own force
// includes, so this runs last and wins.

#include "sifli_soc_aliases.h"

#define LCDC1 hwp_lcdc1
#define FLASH1 hwp_qspi1
#define FLASH2 hwp_qspi2
#define MPI1 hwp_mpi1
#define MPI2 hwp_mpi2
#define BTIM1 hwp_btim1
#define BTIM2 hwp_btim2
