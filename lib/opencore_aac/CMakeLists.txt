# Copyright (c) 2026 Yuzuki Tsuru
# SPDX-License-Identifier: Apache-2.0

set(AAC_DIR ${ZEPHYR_BASE}/../modules/lib/opencore/codecs_v2/audio/aac/dec)

zephyr_library_named(opencore_aac)
file(GLOB AAC_SOURCES ${AAC_DIR}/src/*.cpp)
# decoder_aac.cpp is the C++ wrapper class, the C interface is used instead
list(FILTER AAC_SOURCES EXCLUDE REGEX "/decoder_aac\\.cpp$")
zephyr_library_sources(${AAC_SOURCES})
# the files are plain C with a .cpp name
set_source_files_properties(${AAC_SOURCES} PROPERTIES LANGUAGE C)
zephyr_library_include_directories(${AAC_DIR}/src ${AAC_DIR}/include
	${CMAKE_CURRENT_LIST_DIR}/include)
zephyr_include_directories(${AAC_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/include)
zephyr_library_compile_options(-std=gnu99 -Wno-shadow -Wno-unused-variable
	-Wno-unused-but-set-variable -Wno-implicit-fallthrough -Wno-parentheses)
