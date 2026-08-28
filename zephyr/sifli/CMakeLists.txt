cmake_minimum_required(VERSION 3.20.0)

# Meshtastic firmware for SiFli SF32LB52x, built as a Zephyr application.
#   west build -b t_display_sf32 <repo>/zephyr/sifli -- -DBOARD_ROOT=<repo>/zephyr/sifli

get_filename_component(MESHTASTIC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
# Kept outside the source tree: the repo's linters and PlatformIO both walk it,
# and FetchContent's sub-builds are noisy. Override with -DMESHTASTIC_DEPS_DIR.
set(MESHTASTIC_DEPS_DIR $ENV{HOME}/.cache/meshtastic-sifli-deps
    CACHE PATH "Third-party library download cache")

# Bindings for hardware Zephyr's SiFli support does not describe yet.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(meshtastic_sifli LANGUAGES C CXX ASM)

include(${CMAKE_CURRENT_LIST_DIR}/deps.cmake)

# ── userPrefs.jsonc, force-included the way PlatformIO injects it as -D flags ─
set(MT_USERPREFS_HEADER ${CMAKE_BINARY_DIR}/mt_generated/userprefs_generated.h)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/mt_generated)
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/userprefs_to_header.py
          ${MESHTASTIC_ROOT}/userPrefs.jsonc ${MT_USERPREFS_HEADER}
  RESULT_VARIABLE MT_USERPREFS_RC
)
if(NOT MT_USERPREFS_RC EQUAL 0)
  message(FATAL_ERROR "failed to generate ${MT_USERPREFS_HEADER}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MESHTASTIC_ROOT}/userPrefs.jsonc)

# ── Version, mirroring bin/platformio-custom.py ─────────────────────────────
file(READ ${MESHTASTIC_ROOT}/version.properties MT_VERSION_PROPS)
string(REGEX MATCH "major = ([0-9]+)" _ ${MT_VERSION_PROPS})
set(MT_VER_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "minor = ([0-9]+)" _ ${MT_VERSION_PROPS})
set(MT_VER_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "build = ([0-9]+)" _ ${MT_VERSION_PROPS})
set(MT_VER_BUILD ${CMAKE_MATCH_1})
execute_process(
  COMMAND git -C ${MESHTASTIC_ROOT} rev-parse --short HEAD
  OUTPUT_VARIABLE MT_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT MT_GIT_HASH)
  set(MT_GIT_HASH "unknown")
endif()
set(MT_VERSION_SHORT "${MT_VER_MAJOR}.${MT_VER_MINOR}.${MT_VER_BUILD}")
set(MT_VERSION_LONG "${MT_VERSION_SHORT}.${MT_GIT_HASH}")

# ── Firmware sources ────────────────────────────────────────────────────────
# Mirrors build_src_filter from platformio.ini: everything under src/ except
# the subsystems this platform does not build (other platforms' shims, WiFi,
# Ethernet, MQTT, the web/serial APIs, and the niche/e-ink graphics stacks).
file(GLOB_RECURSE MT_SOURCES ${MESHTASTIC_ROOT}/src/*.cpp ${MESHTASTIC_ROOT}/src/*.c)

# PacketAPI and PacketServer from src/mesh/api are what DeviceUI talks to, so
# that directory stays in; only its WiFi and Ethernet servers come out.
set(MT_EXCLUDE
  "/src/platform/esp32/"
  "/src/platform/nrf52/"
  "/src/platform/nrf54l15/"
  "/src/platform/portduino/"
  "/src/platform/rp2xx0/"
  "/src/platform/stm32wl/"
  "/src/graphics/niche/"
  "/src/graphics/eink/"
  "/src/nimble/"
  # No WiFi on this board: the only radio is the keyboard module's ESP32-C6,
  # an AT modem on a UART it shares with the GNSS. Meshtastic's WiFi wants a
  # native socket stack, so supporting it means writing an AT transport first.
  "/src/mesh/wifi/"
  "/src/mesh/http/"
  "/src/mesh/api/WiFiServerAPI"
  "/src/mesh/api/ethServerAPI"
  "/src/mesh/eth/"
  "/src/mesh/raspihttp/"
  "/src/modules/esp32/"
  "/src/serialization/"
  "/src/mqtt/"
)
foreach(pattern ${MT_EXCLUDE})
  list(FILTER MT_SOURCES EXCLUDE REGEX ${pattern})
endforeach()

# The map tile decoder and its file-backed source live under niche/ but are not
# InkHUD; put them back after the blanket exclusion above has run.
file(GLOB MT_MAP_SOURCES ${MESHTASTIC_ROOT}/src/graphics/niche/Map/*.cpp)
list(APPEND MT_SOURCES ${MT_MAP_SOURCES})

file(GLOB MT_VARIANT_SOURCES ${MESHTASTIC_ROOT}/variants/sifli/t-display-sf32/*.cpp)

# Vendored nanopb runtime (nanopb-0.4.9/ at the repo root).
set(MT_NANOPB_SOURCES
  ${MESHTASTIC_ROOT}/nanopb-0.4.9/pb_common.c
  ${MESHTASTIC_ROOT}/nanopb-0.4.9/pb_decode.c
  ${MESHTASTIC_ROOT}/nanopb-0.4.9/pb_encode.c
)

# The vendor LCDC driver from hal_sifli. Zephyr's module does not build it -
# its own mipi_dbi driver programs the registers directly and only implements
# 8080 and 3/4-wire SPI - but the CO5300 needs the controller's QSPI mode
# (LCDC_INTF_SPI_NODCX_4DATA), which this driver already knows how to set up.
set(MT_SIFLI_HAL_LCDC ${ZEPHYR_HAL_SIFLI_MODULE_DIR}/hal/src/bf0_hal_lcdc.c)

# Same story for the MPI controller: Zephyr describes QSPI NOR on it and
# nothing else, so the in-package OPI PSRAM is brought up through the vendor
# HAL by zephyr/sifli/drivers/psram_sf32lb.c.
# And again for audio: Zephyr has no SiFli I2S or codec driver at all, and the
# vendor's own path is RT-Thread's audio server rather than anything portable.
# src/platform/sifli/SiFliAudioOut.cpp drives these directly.
set(MT_SIFLI_HAL_AUDIO
  ${ZEPHYR_HAL_SIFLI_MODULE_DIR}/hal/src/bf0_hal_audprc.c
  # _m, not the plain one: the AUDCODEC handle is #ifdef SF32LB52X down to a
  # single Instance, and bf0_hal_audcodec.c drives an Instance_lp this SoC's
  # headers do not define.
  ${ZEPHYR_HAL_SIFLI_MODULE_DIR}/hal/src/bf0_hal_audcodec_m.c
)

set(MT_SIFLI_HAL_MPI
  ${ZEPHYR_HAL_SIFLI_MODULE_DIR}/hal/src/bf0_hal_mpi.c
  ${ZEPHYR_HAL_SIFLI_MODULE_DIR}/hal/src/bf0_hal_mpi_ex.c
  ${ZEPHYR_HAL_SIFLI_MODULE_DIR}/hal/src/bf0_hal_mpi_psram.c
)

target_sources(app PRIVATE
  ${MT_SIFLI_HAL_LCDC}
  ${MT_SIFLI_HAL_MPI}
  ${MT_SIFLI_HAL_AUDIO}
  ${CMAKE_CURRENT_LIST_DIR}/drivers/psram_sf32lb.c
  ${MT_SOURCES}
  ${MT_VARIANT_SOURCES}
  ${MT_NANOPB_SOURCES}
  ${MT_DEP_SOURCES}
)

target_include_directories(app PRIVATE
  ${MESHTASTIC_ROOT}/src
  ${MESHTASTIC_ROOT}/src/mesh
  ${MESHTASTIC_ROOT}/src/mesh/generated
  ${MESHTASTIC_ROOT}/src/gps
  ${MESHTASTIC_ROOT}/src/buzz
  ${MESHTASTIC_ROOT}/src/platform/sifli
  ${MESHTASTIC_ROOT}/variants/sifli/t-display-sf32
  ${MESHTASTIC_ROOT}/nanopb-0.4.9
)

# SYSTEM, so the compiler stops diagnosing headers we do not maintain. SensorLib
# alone lands in every translation unit - configuration.h probes for its
# SensorRtcHelper.hpp with __has_include - and buried the build output in
# double-promotion noise from code we are never going to change.
target_include_directories(app SYSTEM PRIVATE
  ${MT_DEP_INCLUDES}
)

# The vendor driver is one of the few places that legitimately uses the SoC's
# short register aliases, which sifli_soc_aliases.h retires everywhere else.
set_source_files_properties(${MT_SIFLI_HAL_MPI} PROPERTIES
  COMPILE_DEFINITIONS "HAL_MPI_MODULE_ENABLED"
  # drivers/ supplies the drv_io.h stub the vendor driver includes.
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/drivers"
  COMPILE_OPTIONS "-include;${MESHTASTIC_ROOT}/src/platform/sifli/sifli_hal_aliases.h")

# Both audio sources are wrapped in their module guard, so without these they
# compile to an empty object and the driver fails to link.
set_source_files_properties(${MT_SIFLI_HAL_AUDIO} PROPERTIES
  COMPILE_DEFINITIONS "HAL_AUDPRC_MODULE_ENABLED;HAL_AUDCODEC_MODULE_ENABLED"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/drivers"
  COMPILE_OPTIONS "-include;${MESHTASTIC_ROOT}/src/platform/sifli/sifli_hal_aliases.h")

set_source_files_properties(${MT_SIFLI_HAL_LCDC} PROPERTIES
  COMPILE_DEFINITIONS "HAL_LCD_MODULE_ENABLED"
  # Two list elements, not a SHELL: string - that prefix is target-level only.
  COMPILE_OPTIONS "-include;${MESHTASTIC_ROOT}/src/platform/sifli/sifli_hal_aliases.h")

target_compile_definitions(app PRIVATE
  APP_VERSION=${MT_VERSION_LONG}
  APP_VERSION_SHORT=${MT_VERSION_SHORT}
  APP_ENV=t-display-sf32
  APP_REPO=meshtastic
  ARDUINO=100
  # Compiles LovyanGFX's QSPI AMOLED panels, Panel_CO5300 among them.
  LGFX_USE_QSPI
  USE_THREAD_NAMES
  TINYGPS_OPTION_NO_CUSTOM_FIELDS
  PB_ENABLE_MALLOC=1
  PB_VALIDATE_UTF8=1
  MAX_THREADS=40
  MAX_NUM_NODES=40
  HAS_WIRE=1
  T_DISPLAY_SF32
  # BaseUI on the AMOLED: TFTDisplay picks up the variant's LGFX class.
  HAS_SCREEN=1
  # DeviceUI (MUI). VIEW_320x240 is the view that adapts to larger panels.
  HAS_TFT=1
  VIEW_320x240
  LGFX_DRIVER=LGFX
  # PacketAPI/PacketServer, the link between the firmware and DeviceUI.
  USE_PACKET_API
  LV_LVGL_H_INCLUDE_SIMPLE
  LV_CONF_INCLUDE_SIMPLE
  LV_COMP_CONF_INCLUDE_SIMPLE
  LV_USE_LOG=0
  LV_USE_SYSMON=0
  LV_USE_PERF_MONITOR=0
  LV_USE_MEM_MONITOR=0
  # LVGL's heap lives in the upper half of the PSRAM aperture. The lower half
  # is what the PSRAM linker region hands out, so the two cannot collide until
  # something places 4 MB of data there.
  RAM_SIZE=4096
  LV_MEM_ADR=0x60400000
  # Image cache cap. The 1.5 MB default is sized for map tiles in PSRAM.
  LV_CACHE_DEF_SIZE=32768
  # L76K on the keyboard module, over usart2 (Serial1).
  HAS_GPS=1
  # CST9220 touch, read by the variant rather than through the panel.
  # BME280 on the keyboard module.
  HAS_TELEMETRY=1
  HAS_SENSOR=1
  # BHI260AP IMU, 0x28, on the main board.
  HAS_BHI260AP=1
  # AW86224 haptics, 0x58, on the keyboard module. Probed at runtime.
  HAS_AW86224=1
  # BaseUI map frame. Tiles come from a MAP.BIN on the LittleFS partition;
  # the SD path needs SdFat, which this build does not carry.
  BASEUI_HAS_MAP=1
  # Audio gain must be a build flag: RtttlPcm.h never includes variant.h, so a
  # define there is silently ignored. 0 is the codec's 0dB point.
  AUDIO_DAC_VOLUME=0
  # Same rule for the digital amplitude RtttlPcm renders at. 1536 is the
  # historical SetGain(0.2); raise it if the NS4150B turns out to be quiet.
  AUDIO_RTTTL_AMPLITUDE=1536
  # Routes buzz.cpp down the RTTTL-through-a-codec path and gets an AudioThread
  # created in main(). There is no piezo here, so this is the only tone path.
  HAS_I2S=1
  HAS_TOUCHSCREEN=1
  VARIANT_TOUCHSCREEN=1
  USE_TFTDISPLAY=1
  VARIANT_DISPLAY_DRIVER
  # Subsystems left out of the first bring-up image.
  MESHTASTIC_EXCLUDE_AUDIO=1
  MESHTASTIC_EXCLUDE_MQTT=1
  MESHTASTIC_EXCLUDE_PAXCOUNTER=1
  MESHTASTIC_EXCLUDE_DROPZONE=1
  MESHTASTIC_EXCLUDE_REPLYBOT=1
  MESHTASTIC_EXCLUDE_RANGETEST=1
  MESHTASTIC_EXCLUDE_REMOTEHARDWARE=1
  MESHTASTIC_EXCLUDE_HEALTH_TELEMETRY=1
  MESHTASTIC_EXCLUDE_POWERSTRESS=1
  MESHTASTIC_EXCLUDE_GENERIC_THREAD_MODULE=1
  MESHTASTIC_EXCLUDE_POWERMON=1
  MESHTASTIC_EXCLUDE_FILES_MANIFEST=1
  RADIOLIB_EXCLUDE_CC1101=1
  RADIOLIB_EXCLUDE_NRF24=1
  RADIOLIB_EXCLUDE_RF69=1
  RADIOLIB_EXCLUDE_SX1231=1
  RADIOLIB_EXCLUDE_SX1233=1
  RADIOLIB_EXCLUDE_SI443X=1
  RADIOLIB_EXCLUDE_RFM2X=1
  RADIOLIB_EXCLUDE_AFSK=1
  RADIOLIB_EXCLUDE_BELL=1
  RADIOLIB_EXCLUDE_HELLSCHREIBER=1
  RADIOLIB_EXCLUDE_MORSE=1
  RADIOLIB_EXCLUDE_RTTY=1
  RADIOLIB_EXCLUDE_SSTV=1
  RADIOLIB_EXCLUDE_AX25=1
  RADIOLIB_EXCLUDE_DIRECT_RECEIVE=1
  RADIOLIB_EXCLUDE_PAGER=1
  RADIOLIB_EXCLUDE_FSK4=1
  RADIOLIB_EXCLUDE_APRS=1
  RADIOLIB_EXCLUDE_ADSB=1
  RADIOLIB_EXCLUDE_LORAWAN=1
)

# SHELL: keeps each option and its argument together; without it CMake
# de-duplicates the repeated -include flag and the second header arrives as a
# bare filename, which the compiler reads as a second input file.
# device-ui's factory instantiates LGFXDriver<LGFX_DRIVER>, so the board's LGFX
# class has to be visible there - and only there. LovyanGFX declares a global
# `fonts` namespace that collides with the generated UI's `fonts[]` table, so
# this cannot be a target-wide force include.
set_source_files_properties(${device_ui_SOURCE_DIR}/source/graphics/driver/DisplayDriverFactory.cpp PROPERTIES
  COMPILE_OPTIONS "-include;${MESHTASTIC_ROOT}/variants/sifli/t-display-sf32/variant_display.h")

target_compile_options(app PRIVATE
  "SHELL:-include ${MT_USERPREFS_HEADER}"
  # device-ui instantiates LGFXDriver<LGFX_DRIVER>, so the class has to be
  # visible in its translation units. Forcing the header in avoids carrying a
  # device-ui fork just to add a board header include. C++ only - LVGL is C,
  # and the header is a LovyanGFX class.
  # Retires the SoC header's short register aliases (CRC, SPI1, ...) before
  # they can collide with ordinary identifiers in portable code. C and C++.
  "SHELL:-include ${MESHTASTIC_ROOT}/src/platform/sifli/sifli_soc_aliases.h"
  # The SDK's libstdc++ has no gthreads, so std::mutex/condition_variable/
  # thread come from the platform layer instead. Force-included so it lands
  # ahead of any library's own <mutex>.
  "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${MESHTASTIC_ROOT}/src/platform/sifli/zephyr_std_threads.hpp>"
  -Os
  -fpermissive
  -Wno-missing-field-initializers
  -Wno-format
  -Wno-unused-variable
)
