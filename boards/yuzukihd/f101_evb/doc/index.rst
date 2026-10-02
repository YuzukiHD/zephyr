.. _f101_evb:

YuzukiHD F101 EVB
#################

Overview
********

The F101 EVB is a development board for the Allwinner F101
(``sun252i_f101``) SoC. It is the YuzukiNeko configuration (see
:ref:`yuzukineko`) with the console on the EVB debug UART (UART3 on
PE08/PE09, muxsel 6). UART3 shares its pins with I2C0, so I2C0 is off,
and UART1 (PB00/PB01) is not wired to the console of the EVB.

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
