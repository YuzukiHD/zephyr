# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
  ve_core.c
  ve_fifo.c
  ve_fbm.c
  ve_sbm.c
  ve_decoder.c
  vdec_sunxi.c
  glue/glue_core.c
  glue/glue_irq.c
  glue/glue_clk.c
  glue/glue_rst.c
  glue/glue_sem.c
  glue/glue_pthread.c
  glue/glue_libc.c
  glue/glue_mem.c
)

if(CONFIG_VDEC_SUNXI_LIBCEDARC_PATH STREQUAL "")
  set(_cedarc_lib ${CMAKE_CURRENT_LIST_DIR}/lib/libcedarc.a)
else()
  set(_cedarc_lib ${CONFIG_VDEC_SUNXI_LIBCEDARC_PATH})
endif()
if(NOT EXISTS ${_cedarc_lib})
  message(FATAL_ERROR "decoder archive ${_cedarc_lib} not found")
endif()
# The archive lists some members twice, so it is linked as a plain archive
# (members pulled on demand) rather than whole.
zephyr_link_libraries(${_cedarc_lib})
