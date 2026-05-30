cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pwr_logger C)

# Bake a git-derived build tag into APP_BUILD_TAG so `pwr status` reports
# the exact commit the running image was built from. Format from `git
# describe --always --dirty --tags`:
#   - exact tag:           v1.1
#   - past a tag:          v1.1-3-gabcd123
#   - dirty tree:          v1.1-3-gabcd123-dirty
#   - no tags / shallow:   abcd123 (or abcd123-dirty)
# Falls back to "unknown" if git is unavailable.
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
message(STATUS "pwr-logger build tag: ${APP_GIT_TAG}")
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
