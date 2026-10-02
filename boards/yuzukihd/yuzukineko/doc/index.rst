.. _yuzukineko:

YuzukiHD YuzukiNeko
###################

Overview
********

The YuzukiNeko is a development board built around the Allwinner F101
(``sun252i_f101``) SoC, which integrates a single T-Head Xuantie C907
RISC-V 32-bit core (rv32imac), a PLIC, a CLINT, internal SRAM/PSRAM and
up to 6 UARTs. The SoC has no MMU and no external DDR.

The port provides the SoC peripherals: GPIO, pinctrl, UART, I2C, SPI, PWM,
ADC, DMA, watchdog, the on-chip audio codec, I2S and S/PDIF, the USB device
and host controllers, the SD card slot, the 2D accelerator, the video engine
and the RGB LCD pipeline with its panel and backlight (the console stays on
UART1: starting the backlight takes PB0/PB1 over, so the console goes silent
once the backlight runs).

Memory
******

The image is loaded into the internal SRAM/PSRAM at ``0x40010000`` by the
bootloader (boot0 -> u-boot), which also performs clock, UART1 pinmux
(PB00/PB01, muxsel 4) and DRAM/PSRAM initialization. UART1 runs at
115200 baud with a 24 MHz UART clock (the bootloader default).

Build
*****

.. code-block:: console

   west build -b yuzukineko samples/hello_world

The resulting ``build/zephyr/zephyr.bin`` can be packed into a boot
image and loaded by u-boot.

References
**********

* `YuzukiHD <https://yuzukihd.top>`_
