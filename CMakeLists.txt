cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pwr_logger C)

target_sources(app PRIVATE
    src/main.c
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
)

target_include_directories(app PRIVATE src)
