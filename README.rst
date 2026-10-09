.. raw:: html

   <a href="https://www.zephyrproject.org">
     <p align="center">
       <picture>
         <source media="(prefers-color-scheme: dark)" srcset="doc/_static/images/logo-readme-dark.svg">
         <source media="(prefers-color-scheme: light)" srcset="doc/_static/images/logo-readme-light.svg">
         <img src="doc/_static/images/logo-readme-light.svg">
       </picture>
     </p>
   </a>

   <a href="https://bestpractices.coreinfrastructure.org/projects/74"><img src="https://bestpractices.coreinfrastructure.org/projects/74/badge"></a>
   <a href="https://scorecard.dev/viewer/?uri=github.com/zephyrproject-rtos/zephyr"><img src="https://api.securityscorecards.dev/projects/github.com/zephyrproject-rtos/zephyr/badge"></a>
   <a href="https://github.com/zephyrproject-rtos/zephyr/actions/workflows/twister.yaml?query=branch%3Amain"><img src="https://github.com/zephyrproject-rtos/zephyr/actions/workflows/twister.yaml/badge.svg?event=push"></a>


Zephyr for the Allwinner F101
*****************************

This is a fork of the `Zephyr Project`_ with a port to the Allwinner F101
(``sun252i_f101``): a single T-Head XuanTie C907 core (``rv32imafdcv``, vector
extension, optional Sv32 MMU), internal SRAM and 16 MB PSRAM, no external DDR.

Boards
======

========================  =====================================================
``yuzukineko``            YuzukiHD Neko with the MIPI DSI carrier board: panel,
                          GT967 touch, SD card, USB, audio, AIC8800 WiFi/Bluetooth,
                          G2D and video engine. Console: UART1 (PB0/PB1).
``f101_evb``              The physical F101 EVB: RGB/LVDS/DSI panel options,
                          PWM backlight. Console: UART3 (PE8/PE9).
========================  =====================================================

SoC code is in ``soc/allwinner/sun252i_f101``, the devicetree in
``dts/riscv/allwinner/sun252i_f101.dtsi``, the boards in ``boards/yuzukihd``.

What is ported
==============

Drivers are written for Zephyr's own APIs, with bindings under
``dts/bindings/*/allwinner,sunxi-*.yaml``:

* clock and reset controller, pinctrl, GPIO, DMA, watchdog, hwinfo (SID fuses),
  CPU clock rate
* I2C, SPI, PWM (including panel backlight), ADC, SD/MMC (``sdhc``)
* SPIF SPI NOR flash controller: quad reads at 100 MHz with a tuned sample point,
  saved parameters, XIP window and XIP code sections
* audio: on-chip codec (DAC/ADC with analog part), I2S0, S/PDIF
* display: planes, TCON, RGB/LVDS/DSI encoders, D-PHY, backlight, scaling video
  plane for YUV/RGB pictures; MIPI DBI
* G2D 2D accelerator (``drivers/g2d``, ``include/zephyr/drivers/g2d.h``): fill,
  blit with format conversion, scaling and rotation, blend
* video engine (``drivers/vdec``): H.264, JPEG and PNG decoding
* USB: PHY, and CherryUSB device (CDC ACM, MSC, vendor display class) and host
  (EHCI/OHCI) ports; CherryUSB is a west project
* GT967 capacitive touch controller (``drivers/input``)
* RISC-V: vector extension context saving, a machine-mode Sv32 MMU with demand
  paging (RAM, file and swap backing stores), ``lib/mp4`` and an AAC decoder
  (``lib/opencore_aac``)

Build and run
=============

.. code-block:: console

   . ./env.sh                       # in the workspace: venv, ZEPHYR_BASE, toolchain
   west build -b yuzukineko -d build/hello zephyr/samples/hello_world

The image is linked at ``0x40010000``. Without boot media it is loaded over USB
with ``xfel`` (FEL mode, power cycle before every download)::

   xfel ddr f101-s3
   xfel write 0x40010000 build/hello/zephyr/zephyr.bin
   xfel exec 0x40010000

Or write it to the SPI NOR flash and let the boot loader start it, see
``uf2-downloader`` in the ``zephyr-components`` repository (flash by copying a
``.uf2`` file onto a USB drive).

Samples for the hardware are under ``samples/drivers`` (``display``, ``g2d``,
``audio``, ``flash``, ``usb``, ``mp4_player``, ``vdec``, ``rvv``, ...) and
``samples/subsys`` (``demand_paging_*``, ``llext``). Applications built on this
port (emulators, WiFi, MicroPython, PocketJS, ...) live in ``zephyr-components``.

The rest of this file is the README of upstream Zephyr.

Zephyr Project
**************

The Zephyr Project is a scalable real-time operating system (RTOS) supporting
multiple hardware architectures, optimized for resource constrained devices,
and built with security in mind.

The Zephyr OS is based on a small-footprint kernel designed for use on
resource-constrained systems: from simple embedded environmental sensors and
LED wearables to sophisticated smart watches and IoT wireless gateways.

The Zephyr kernel supports multiple architectures, including ARM (Cortex-A,
Cortex-R, Cortex-M), Intel x86, ARC, Tensilica Xtensa, and RISC-V,
SPARC, MIPS, and a large number of `supported boards`_.

.. below included in doc/introduction/introduction.rst


Getting Started
***************

Welcome to Zephyr! See the `Introduction to Zephyr`_ for a high-level overview,
and the documentation's `Getting Started Guide`_ to start developing.

.. start_include_here

Community Support
*****************

Community support is provided via mailing lists and Discord; see the Resources
below for details.

.. _project-resources:

Resources
*********

Here's a quick summary of resources to help you find your way around:

Getting Started
---------------

  | 📖 `Zephyr Documentation`_
  | 🚀 `Getting Started Guide`_
  | 🙋🏽 `Tips when asking for help`_
  | 💻 `Code samples`_

Code and Development
--------------------

  | 🌐 `Source Code Repository`_
  | 📦 `Releases`_
  | 🤝 `Contribution Guide`_

Community and Support
---------------------

  | 💬 `Discord Server`_ for real-time community discussions
  | 📧 `User mailing list (users@lists.zephyrproject.org)`_
  | 📧 `Developer mailing list (devel@lists.zephyrproject.org)`_
  | 📬 `Other project mailing lists`_
  | 📚 `Project Wiki`_

Issue Tracking and Security
---------------------------

  | 🐛 `GitHub Issues`_
  | 🔒 `Security documentation`_
  | 🛡️ `Security Advisories Repository`_
  | ⚠️ Report security vulnerabilities at vulnerabilities@zephyrproject.org

Additional Resources
--------------------
  | 🌐 `Zephyr Project Website`_
  | 📺 `Zephyr Tech Talks`_

.. _Zephyr Project: https://www.zephyrproject.org
.. _Zephyr Project Website: https://www.zephyrproject.org
.. _Discord Server: https://chat.zephyrproject.org
.. _supported boards: https://docs.zephyrproject.org/latest/boards/index.html
.. _Zephyr Documentation: https://docs.zephyrproject.org
.. _Introduction to Zephyr: https://docs.zephyrproject.org/latest/introduction/index.html
.. _Getting Started Guide: https://docs.zephyrproject.org/latest/develop/getting_started/index.html
.. _Contribution Guide: https://docs.zephyrproject.org/latest/contribute/index.html
.. _Source Code Repository: https://github.com/zephyrproject-rtos/zephyr
.. _GitHub Issues: https://github.com/zephyrproject-rtos/zephyr/issues
.. _Releases: https://github.com/zephyrproject-rtos/zephyr/releases
.. _Project Wiki: https://github.com/zephyrproject-rtos/zephyr/wiki
.. _User mailing list (users@lists.zephyrproject.org): https://lists.zephyrproject.org/g/users
.. _Developer mailing list (devel@lists.zephyrproject.org): https://lists.zephyrproject.org/g/devel
.. _Other project mailing lists: https://lists.zephyrproject.org/g/main/subgroups
.. _Code samples: https://docs.zephyrproject.org/latest/samples/index.html
.. _Security documentation: https://docs.zephyrproject.org/latest/security/index.html
.. _Security Advisories Repository: https://github.com/zephyrproject-rtos/zephyr/security
.. _Tips when asking for help: https://docs.zephyrproject.org/latest/develop/getting_started/index.html#asking-for-help
.. _Zephyr Tech Talks: https://www.zephyrproject.org/tech-talks
