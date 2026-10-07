.. _f101_evb:

YuzukiHD F101 EVB
#################

Overview
********

The F101 EVB is a development board for the Allwinner F101
(``sun252i_f101``) SoC with the console on the EVB debug UART (UART3 on
PE08/PE09, muxsel 6). UART3 shares its pins with I2C0, so I2C0 is off;
the LCD owns PD0..PD21, so the MIPI DBI, SPI1 and PWM0 are off too.
The board file is standalone, :ref:`yuzukineko` is a separate board.

The board has the 1024x600 RGB666 panel, backlight, SD card slot, USB
(OTG plus a host port), on-chip audio codec, S/PDIF, the 2D accelerator
and the video engine.

Build
*****

.. code-block:: console

   west build -b f101_evb samples/hello_world

References
**********

* `YuzukiHD <https://yuzukihd.top>`_
