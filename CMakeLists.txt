cmake_minimum_required(VERSION 3.20.0)

# ---- Auto-derived version --------------------------------------------------
# Two version surfaces that need to track every push, not stay pinned:
#
#   1. APP_BUILD_TAG (visible in `pwr status`): `git describe`-derived, e.g.
#      v1.0-13-gde99493 or v1.0-13-gde99493-dirty.
#
#   2. MCUboot image version (visible in `pwrctl ota` image list and on
#      the swap trailer): MAJOR.MINOR.PATCH+TWEAK. MAJOR/MINOR pinned in
#      CMake; PATCHLEVEL = number of commits past the v1.0 tag (mod 256
#      because Zephyr's version parser is byte-limited). Result: every
#      committed-and-pushed image gets a unique version number.
#
# Both fall back to "unknown" / 0 if git isn't usable (clean tarball
# extract etc.); functional, just less informative.
#
# The VERSION file is generated here (NOT tracked in git) so the source
# tree never carries a stale snapshot. Zephyr's find_package reads it
# during boilerplate; computing + writing it must happen BEFORE that.

set(APP_VERSION_MAJOR 1)
set(APP_VERSION_MINOR 1)

execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_rc
    ERROR_QUIET
)
if(NOT _git_rc EQUAL 0 OR APP_GIT_TAG STREQUAL "")
    set(APP_GIT_TAG "unknown")
endif()

# Commit count past v1.0; goes into PATCHLEVEL. Mod 256 to stay within the
# byte cap (just bump VERSION_MINOR before we run out at commit 256).
execute_process(
    COMMAND git rev-list --count v1.0..HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_COMMIT_COUNT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _count_rc
    ERROR_QUIET
)
if(NOT _count_rc EQUAL 0 OR APP_COMMIT_COUNT STREQUAL "")
    set(APP_COMMIT_COUNT 0)
endif()
math(EXPR APP_VERSION_PATCH "${APP_COMMIT_COUNT} % 256")

file(WRITE ${CMAKE_SOURCE_DIR}/VERSION
    "VERSION_MAJOR = ${APP_VERSION_MAJOR}\n"
    "VERSION_MINOR = ${APP_VERSION_MINOR}\n"
    "PATCHLEVEL = ${APP_VERSION_PATCH}\n"
    "VERSION_TWEAK = 0\n"
    "EXTRAVERSION =\n"
)

message(STATUS "pwr-logger build tag:    ${APP_GIT_TAG}")
message(STATUS "pwr-logger image version: ${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}.${APP_VERSION_PATCH}+0")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pwr_logger C)

add_compile_definitions(APP_BUILD_TAG="${APP_GIT_TAG}")

target_sources(app PRIVATE
    src/main.c
    src/board_init.c
    src/bringup.c
    src/timebase.c
    src/acquisition.c
    src/ina228.c
    src/aggregate.c
    src/tdms.c
    src/storage.c
    src/logger.c
    src/ble_stream.c
    src/usb_msc.c
    src/usbd_pwr_bulk.c
    src/config.c
    src/clock.c
    src/cli.c
    src/logctl.c
    src/power.c
    src/cal.c
    src/controls.c
    src/stats.c
    src/cfgfile.c
    src/wdt.c
    src/ota.c
)

# Optional SSD1306 OLED (CONFIG_APP_DISPLAY) — compiled only when enabled.
target_sources_ifdef(CONFIG_APP_DISPLAY app PRIVATE src/display.c src/ui.c src/icons_font.c)

target_include_directories(app PRIVATE src)
