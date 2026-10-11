# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
  ve_hw.c
  ve_mem.c
  ve_pool.c
  ve_stream.c
  ve_session.c
  vdec_sunxi.c
)

zephyr_library_sources_ifdef(CONFIG_VDEC_SUNXI_JPEG
  ve_jpeg.c
  ve_jpeg_parse.c
  ve_jpeg_hw.c
)
zephyr_library_sources_ifdef(CONFIG_VDEC_SUNXI_H264
  ve_h264.c
  ve_h264_hw.c
  ve_h264_parse.c
  ve_h264_dpb.c
)
zephyr_library_sources_ifdef(CONFIG_VDEC_SUNXI_PNG ve_png.c)
