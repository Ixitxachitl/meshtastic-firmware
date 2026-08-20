# Third-party Arduino libraries, pinned to the same archives platformio.ini
# uses so this build tracks the PlatformIO targets. FetchContent caches them
# under the west workspace, outside the build directory.

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(FETCHCONTENT_BASE_DIR ${MESHTASTIC_DEPS_DIR})

# Any dependency can be pointed at a working copy instead of the pinned
# archive, which is how device-ui gets developed alongside the firmware:
#   -DFETCHCONTENT_SOURCE_DIR_DEVICE_UI=/path/to/device-ui

# A macro, not a function: FetchContent_Populate sets <name>_SOURCE_DIR in the
# current scope, and a function scope would drop it before the caller sees it -
# leaving an empty prefix and turning every glob below into a filesystem-wide
# search.
# Optional third argument: a patch to apply to the extracted archive. Patches
# are skipped when the dependency is pointed at a working copy.
macro(mt_declare name url)
  set(_patch_cmd "")
  if(${ARGC} GREATER 2)
    set(_patch_cmd PATCH_COMMAND patch -p1 --forward --reject-file=- --no-backup-if-mismatch -i ${ARGV2})
  endif()
  # INACTIVITY_TIMEOUT turns a stalled download into an error rather than a
  # build that hangs forever - codeload stalls on the larger archives.
  FetchContent_Declare(${name} URL ${url} DOWNLOAD_EXTRACT_TIMESTAMP TRUE INACTIVITY_TIMEOUT 60 ${_patch_cmd})
  FetchContent_Populate(${name})
  if(NOT ${name}_SOURCE_DIR)
    message(FATAL_ERROR "FetchContent left ${name}_SOURCE_DIR empty")
  endif()
endmacro()

mt_declare(radiolib     https://github.com/jgromes/RadioLib/archive/510e00cfb05bbc3c2b7b524262785454944adb6e.zip)
mt_declare(mt_crypto    https://github.com/meshtastic/Crypto/archive/591ff9a690e8168ccb7a36abde8d7783e448d395.zip)
mt_declare(arduinothread https://github.com/meshtastic/ArduinoThread/archive/b841b0415721f1341ea41cccfb4adccfaf951567.zip)
mt_declare(arduinofsm   https://github.com/meshtastic/arduino-fsm/archive/7db3702bf0cfe97b783d6c72595e3f38e0b19159.zip)
mt_declare(onebutton    https://github.com/meshtastic/OneButton/archive/fa352d668c53f290cfa480a5f79ad422cd828c70.zip)
mt_declare(tinygpsplus  https://github.com/meshtastic/TinyGPSPlus/archive/71a82db35f3b973440044c476d4bcdc673b104f4.zip)
mt_declare(erriezcrc32  https://github.com/Erriez/ErriezCRC32/archive/refs/tags/1.0.1.zip)
mt_declare(rtttl        https://github.com/end2endzone/NonBlockingRTTTL/archive/refs/tags/1.4.0.zip)
mt_declare(ssd1306      https://github.com/meshtastic/esp8266-oled-ssd1306/archive/ace0fcbc108d357e1801cc24a45ba8a80e160c9b.zip)
mt_declare(lovyangfx    https://github.com/lovyan03/LovyanGFX/archive/refs/tags/1.2.26.zip)
# Environmental telemetry: the keyboard module carries a BME280.
mt_declare(ada_busio    https://github.com/adafruit/Adafruit_BusIO/archive/refs/tags/1.17.4.zip)
mt_declare(ada_sensor   https://github.com/adafruit/Adafruit_Sensor/archive/refs/tags/1.1.15.zip)
mt_declare(ada_bme280   https://github.com/adafruit/Adafruit_BME280_Library/archive/refs/tags/2.3.0.zip)

# DeviceUI (MUI) and its own dependencies, versions from device-ui's library.json.
mt_declare(device_ui     https://github.com/meshtastic/device-ui/archive/adfbd3811a53b6aed0649c8d8f078118c042a407.zip
                         ${CMAKE_CURRENT_LIST_DIR}/patches/device-ui-non-esp32.patch)
mt_declare(lvgl          https://github.com/lvgl/lvgl/archive/refs/tags/v9.3.0.zip)
mt_declare(pngdec        https://github.com/mverch67/PNGdec/archive/d88b6fe2ec8d49c2b097a529b34d1d615ca5cf1b.zip)
mt_declare(sdfat         https://github.com/mverch67/SdFat/archive/152a52251fc5e1d581303b42378ea712ab229246.zip)

# Header search paths. Libraries that ship a src/ subdirectory expose it,
# the rest are flat.
set(MT_DEP_INCLUDES
  ${radiolib_SOURCE_DIR}/src
  ${mt_crypto_SOURCE_DIR}
  ${arduinothread_SOURCE_DIR}
  ${arduinofsm_SOURCE_DIR}
  ${onebutton_SOURCE_DIR}/src
  ${tinygpsplus_SOURCE_DIR}/src
  ${erriezcrc32_SOURCE_DIR}/src
  ${rtttl_SOURCE_DIR}/src
  ${ssd1306_SOURCE_DIR}/src
  ${lovyangfx_SOURCE_DIR}/src
  ${ada_busio_SOURCE_DIR}
  ${ada_sensor_SOURCE_DIR}
  ${ada_bme280_SOURCE_DIR}
  # DeviceUI. The 320x240 view is the one that adapts itself to larger panels;
  # TFTView_320x240::apply_hotfix() resizes for 480-pixel displays at runtime.
  ${device_ui_SOURCE_DIR}
  ${device_ui_SOURCE_DIR}/include
  ${device_ui_SOURCE_DIR}/generated/ui_320x240
  ${device_ui_SOURCE_DIR}/locale
  ${lvgl_SOURCE_DIR}
  ${lvgl_SOURCE_DIR}/src
  ${pngdec_SOURCE_DIR}/src
)

# Sources compiled into the image. RadioLib and Crypto are globbed whole; the
# rest are small enough that their entire src/ is safe to build.
file(GLOB_RECURSE MT_RADIOLIB_SRC ${radiolib_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE MT_CRYPTO_SRC ${mt_crypto_SOURCE_DIR}/*.cpp)
# TFTDisplay derives from this library's OLEDDisplay, so the BaseUI path needs
# it compiled in, not just on the include path.
file(GLOB MT_OLED_SRC ${ssd1306_SOURCE_DIR}/src/*.cpp)

file(GLOB MT_SENSOR_SRC
  ${ada_busio_SOURCE_DIR}/*.cpp
  ${ada_sensor_SOURCE_DIR}/*.cpp
  ${ada_bme280_SOURCE_DIR}/*.cpp
)

file(GLOB MT_MISC_SRC
  ${arduinothread_SOURCE_DIR}/*.cpp
  ${arduinofsm_SOURCE_DIR}/*.cpp
  ${onebutton_SOURCE_DIR}/src/*.cpp
  ${tinygpsplus_SOURCE_DIR}/src/*.cpp
  ${erriezcrc32_SOURCE_DIR}/src/*.c
  ${rtttl_SOURCE_DIR}/src/*.cpp
)
# LovyanGFX picks its platform layer from ARDUINO being defined, so the whole
# src/ tree compiles against our shim; per-platform files guard themselves.
file(GLOB_RECURSE MT_LGFX_SRC ${lovyangfx_SOURCE_DIR}/src/*.cpp)
# Its generic Arduino SPI bus drives the panel over an Arduino SPI object. This
# board's panel hangs off the LCDC instead, and the bus wants transfer variants
# the shim does not implement, so drop the file rather than carry dead code.
list(FILTER MT_LGFX_SRC EXCLUDE REGEX "arduino_default/Bus_SPI.cpp")

# device-ui's extra_script.py builds source/, resources/, locale/ and the one
# generated view its VIEW_* macro selects; mirror that here.
file(GLOB_RECURSE MT_DEVICE_UI_SRC
  ${device_ui_SOURCE_DIR}/source/*.cpp
  ${device_ui_SOURCE_DIR}/resources/*.c
  ${device_ui_SOURCE_DIR}/locale/*.c
  ${device_ui_SOURCE_DIR}/generated/ui_320x240/*.c
)
# X11 and framebuffer drivers are Portduino-only. The two SD tile services
# want the Arduino SD and SdFat libraries; this board's card shares the radio's
# SPI bus and neither service is reachable without HAS_SDCARD.
list(FILTER MT_DEVICE_UI_SRC EXCLUDE REGEX "(X11Driver|FBDriver|SDCardService|SdFatService)\\.cpp")

file(GLOB_RECURSE MT_LVGL_SRC ${lvgl_SOURCE_DIR}/src/*.c)
file(GLOB MT_PNGDEC_SRC ${pngdec_SOURCE_DIR}/src/*.cpp)

set(MT_DEP_SOURCES ${MT_RADIOLIB_SRC} ${MT_CRYPTO_SRC} ${MT_MISC_SRC} ${MT_OLED_SRC} ${MT_SENSOR_SRC}
    ${MT_LGFX_SRC} ${MT_LVGL_SRC} ${MT_PNGDEC_SRC} ${MT_DEVICE_UI_SRC})
