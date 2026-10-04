.. zephyr:code-sample:: sunxi_spif
   :name: Allwinner SPIF flash controller

   Exercise the SPIF driver on the SPI NOR flash of the F101 EVB.

Overview
********

The sample prints the flash identification and a map of the used 64 KiB
blocks, then

* searches the sample point (``sunxi_spif_tune()``) at the 100 MHz of the
  devicetree and prints the passing delays of every sample mode,
* stores the result in the ``params`` partition and loads it back; at the next
  boot the driver applies the stored point and does not tune again,
* reads 1 MiB, erases/programs/verifies the ``scratch`` partition (also with an
  unaligned start, length and buffer),
* maps the ``xip_test`` partition into the address space, compares the window
  with ``flash_read()``, runs code from the window, and erases and rewrites the
  sector that holds the code while the window is mapped.

The partitions are in blank space of the EVB flash (``boards/f101_evb.overlay``):
the first 5 MiB, ``0x500000`` and ``0xf00000`` are used by other software and
are never written. The sample refuses to erase a partition that is not blank
and does not carry its own marker.

With ``-DEXTRA_CONF_FILE=xip.conf`` (``CONFIG_FLASH_SUNXI_SPIF_XIP_SECTIONS``)
functions and constants marked ``__xip_text`` / ``__xip_rodata`` are linked at
``0x0e000000``, deployed to the ``xip`` partition at init (only when it differs)
and run from the flash; the sample compares the speed with the same loop in RAM.

Building and running
********************

.. code-block:: console

   west build -b f101_evb -d build/spif zephyr/samples/drivers/flash/sunxi_spif
   west build -b f101_evb -d build/spifx zephyr/samples/drivers/flash/sunxi_spif \
        -- -DEXTRA_CONF_FILE=xip.conf

Download ``zephyr.bin`` with xfel as usual; the last line is ``PASS (0 failures)``.
The EVB flash is GD25Q128 (``c84018``): 100 MHz works with mode 0, delay 38
(window 13..63), reads reach about 29 MB/s, the XIP window about 38 MB/s.
