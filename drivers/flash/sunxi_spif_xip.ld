/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Code and constants marked __xip_text / __xip_rodata run from the memory
 * window of the SPIF controller. They are linked at the address of the
 * window, but their bytes are part of the flat image, right behind the other
 * sections: the image then carries the contents of the flash partition that
 * the window maps, and the driver programs the partition from there when it
 * differs. The location counter jumps over the window address and back.
 */
	__spif_xip_load = .;
	.spif_xip CONFIG_FLASH_SUNXI_SPIF_XIP_BASE : AT(__spif_xip_load)
	{
		__spif_xip_start = .;
		KEEP(*(.xip_text .xip_text.*))
		KEEP(*(.xip_rodata .xip_rodata.*))
		. = ALIGN(4096);
		__spif_xip_end = .;
	}
	__spif_xip_size = __spif_xip_end - __spif_xip_start;
	. = __spif_xip_load + __spif_xip_size;
