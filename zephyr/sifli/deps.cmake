# Third-party Arduino libraries, pinned to the same archives platformio.ini
# uses so this build tracks the PlatformIO targets. FetchContent caches them
# under the west workspace, outside the build directory.

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(FETCHCONTENT_BASE_DIR ${MESHTASTIC_DEPS_DIR})

# A macro, not a function: FetchContent_Populate sets <name>_SOURCE_DIR in the
# current scope, and a function scope would drop it before the caller sees it -
# leaving an empty prefix and turning every glob below into a filesystem-wide
# search.
macro(mt_declare name url)
  FetchContent_Declare(${name} URL ${url} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
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
)

# Sources compiled into the image. RadioLib and Crypto are globbed whole; the
# rest are small enough that their entire src/ is safe to build.
file(GLOB_RECURSE MT_RADIOLIB_SRC ${radiolib_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE MT_CRYPTO_SRC ${mt_crypto_SOURCE_DIR}/*.cpp)
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

set(MT_DEP_SOURCES ${MT_RADIOLIB_SRC} ${MT_CRYPTO_SRC} ${MT_MISC_SRC} ${MT_LGFX_SRC})
