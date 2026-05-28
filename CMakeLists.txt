cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pwr_logger C)

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
    src/config.c
    src/clock.c
    src/cli.c
    src/logctl.c
    src/power.c
    src/cal.c
    src/controls.c
    src/stats.c
    src/cfgfile.c
)

# Optional SSD1306 OLED (CONFIG_APP_DISPLAY) — compiled only when enabled.
target_sources_ifdef(CONFIG_APP_DISPLAY app PRIVATE src/display.c src/ui.c src/icons_font.c)

target_include_directories(app PRIVATE src)
