# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bthome_full_node)
target_sources(app PRIVATE
    src/main.c
    src/sensors/sensor_die_temp.c
    src/sensors/sensor_imu.c
    src/sensors/sensor_pir.c
)
target_include_directories(app PRIVATE src)
