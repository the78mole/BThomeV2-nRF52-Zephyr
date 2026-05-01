# SPDX-License-Identifier: MIT
# BThomeV2 nRF52 Zephyr module root CMakeLists.txt

cmake_minimum_required(VERSION 3.20)

if(CONFIG_BTHOME_V2)
  add_subdirectory(lib/bthome_v2)
endif()
