# Copyright (c) 2026 Yuzuki Tsuru
# SPDX-License-Identifier: Apache-2.0

set(HELIX_DIR ${ZEPHYR_BASE}/../modules/lib/libhelix/src/libhelix-aac)

zephyr_library_named(helix_aac)
file(GLOB HELIX_SOURCES ${HELIX_DIR}/*.c)
zephyr_library_sources(${HELIX_SOURCES} helix_glue.c)
zephyr_library_include_directories(${HELIX_DIR} ${HELIX_DIR}/.. ${CMAKE_CURRENT_LIST_DIR}/include)
zephyr_include_directories(${HELIX_DIR})
# ESP_PLATFORM selects the portable C versions of the multiply and clip helpers
zephyr_library_compile_definitions(ESP_PLATFORM)
zephyr_library_compile_options(-Wno-shadow -Wno-unused-variable -Wno-unused-but-set-variable
	-Wno-implicit-fallthrough)
zephyr_library_compile_definitions_ifdef(CONFIG_HELIX_AAC_SBR HELIX_FEATURE_AUDIO_CODEC_AAC_SBR)
