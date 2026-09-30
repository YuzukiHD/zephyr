# Copyright (c) 2026 Yuzuki Tsuru
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cherryusb_cdc_acm)

target_sources(app PRIVATE src/main.c
	${ZEPHYR_CHERRYUSB_MODULE_DIR}/demo/cdc_acm_template.c)
