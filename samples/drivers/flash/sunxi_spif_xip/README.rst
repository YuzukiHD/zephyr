.. zephyr:code-sample:: sunxi_spif_xip
   :name: Execute in place from the SPIF flash window

   Run the whole Zephyr image from the memory window of the SPIF controller.

Overview
********

The image is linked at ``0x0e000000`` (``CONFIG_XIP``, ``zephyr,flash`` is the
window, see ``boards/f101_evb.overlay``): code and read only data are fetched
from the SPI NOR flash, ``.data`` is copied from the flash to RAM at boot.
The sample prints the addresses of ``main``, the rodata and the data, so that
the placement can be seen.

The window has to be mapped before the first instruction runs. That is done
by the SyterKit application ``xip-boot`` (``boards/yuzukineko/app_sram``): it
brings up the PSRAM and the flash, maps the flash from a fixed offset at the
window and jumps to it.

``spi0`` is disabled in the overlay: it uses the pins of the flash (PC0..PC5)
and switching them away would cut the window off.

Building and running
********************

.. code-block:: console

   west build -b f101_evb -d build/xip zephyr/samples/drivers/flash/sunxi_spif_xip

   # SyterKit (RISC-V toolchain on PATH)
   make O=out yuzukineko_rv32_sram_defconfig && make O=out

With the board in FEL mode:

.. code-block:: console

   xfel spinor write 0xa00000 build/xip/zephyr/zephyr.bin
   xfel write 0x20000 out/build/yuzukineko/app_sram/xip-boot/xip-boot_fel.bin
   xfel exec 0x20000

The offset ``0xa00000`` is ``XIP_FLASH_OFFSET`` of ``xip-boot``. The console
(UART3) shows the SyterKit log and then the sample.
