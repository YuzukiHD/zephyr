# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mp4_player)

# the statistics overlay is shared with the H.264 file player
set(PLAYER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../vdec/h264_player/src)
target_sources(app PRIVATE src/main.c ${PLAYER_SRC}/overlay.c)
target_include_directories(app PRIVATE ${PLAYER_SRC})
