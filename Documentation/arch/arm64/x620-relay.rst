.. SPDX-License-Identifier: GPL-2.0-only

LeEco X620 retained-firmware bring-up
====================================

This experimental Linux 6.12 profile runs on MT6797 through stock preloader,
ATF and LK, followed by the X620 U-Boot relay. Firmware still initializes
DRAM, PMIC, clocks and the panel. This is a boot diagnostic milestone, not
complete SoC or phone support.

The board DT uses one CPU and the first 512 MiB of DDR, reserves firmware
and persistent-log areas, and leaves clock, pin and power-domain providers
disabled to retain firmware state. U-Boot loads the native ARM64 Image at
0x40200000 and supplies the mainline board DT and diagnostic initrd. No
kernel entry assembly changes are required.

``CONFIG_X620_RELAY_DIAGNOSTICS`` enables two temporary adapters:

* ``earlycon=x620ram,mmio32,0x444bf000 keep_bootcon`` records console output
  using the vendor persistent-RAM signature/start/size layout. The initial
  page retains the first boot messages. An early initcall expands the mapping
  to the reserved 64 KiB zone for subsequent messages. This zone must not be
  assigned to ramoops simultaneously. Recovery or power loss can clear it.
* The retained display refresh driver checks the measured OVL framebuffer,
  pitch, DSI command mode and command queue before touching registers. It
  makes layer 0 opaque, removes the logo overlay and triggers mutex/DSI at
  100 ms intervals when DSI is idle. It stops refreshing if the framebuffer
  address changes. ``simplefb`` provides fbcon; the adapter does not initialize
  clocks, power domains or the panel.

Reproducing the kernel build
----------------------------

The independent ``arch/arm64/configs/x620_relay.config`` profile includes
F2FS and diagnostic userspace support without editing a source defconfig.
Use an out-of-tree build and build only the X620 DT target::

    export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
    make O=../out/kernel KCONFIG_ALLCONFIG="$PWD/arch/arm64/configs/x620_relay.config" allnoconfig
    make O=../out/kernel -j10 Image mediatek/mt6797-leeco-x620.dtb

Reuse the same output directory for subsequent builds. Merge configuration
updates and run ``olddefconfig`` instead of cleaning or starting again with
``allnoconfig``. The companion workspace keeps equivalent files in ``cfg/``
and packages ``dist/linux-boot.img`` with gzip Image, initrd and board DT in an
Android v2 container for U-Boot's fastboot RAM-download path. This payload is
distinct from the LK-facing ``dist/boot.img`` that contains the U-Boot relay.

Stock LK's fastboot boot/reboot handlers actively relock the device in the
analyzed backup. Execute download boot only against the U-Boot relay, whose
USB serial is ``X620-UBOOT-RELAY``. Its loader runs after USB gadget teardown
and returns to fastboot on loading errors.

Validation
----------

The ARM64 build and QEMU U-Boot Android-v2/gzip loader reached diagnostic
``/init``. Persistent early and userspace console messages were read back,
and the actual refresh worker passed a reserved-RAM register fixture check.
QEMU does not emulate MT6797 display, interrupt routing or USB hardware.

On 2026-10-13 the owner replied that the requested startup output was visible
after testing the images. This records a hardware boot observation; device
logs and shell access were not collected. The tested Linux payload SHA256 is
``eebd93471a1141db59cc99808e3347f83f7aa7bb1b360c1faec02091f895f223``.
The tested relay SHA256 is
``2ce568d940a595908d40f8569460a2d139790cd51cda679a8202079857772d85``.

Linux USB gadget and internal storage are not connected in this first board
profile. The initrd supplies diagnostic tools, but USB disappearance after
handoff is expected. Touchscreen, networking, SMP, suspend and native panel
initialization remain follow-up work. The rich initrd is built from packages
inside a mounted Debian F2FS image, without unpacking a distribution rootfs
into a loose directory.
