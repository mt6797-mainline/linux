.. SPDX-License-Identifier: GPL-2.0-only

LeEco X620 retained-firmware bring-up
====================================

This experimental Linux 6.12 profile runs on MT6797 through stock preloader,
ATF and LK, followed by the X620 U-Boot relay. Firmware still initializes
DRAM, PMIC, clocks and the panel. This is a boot diagnostic milestone, not
complete SoC or phone support.

The board DT enables eight A53 CPUs and the first 512 MiB of DDR, reserves
firmware and persistent-log areas, and leaves clock, pin and power-domain
providers disabled to retain firmware state. U-Boot loads the native ARM64 Image at
0x40200000 and supplies the mainline board DT and diagnostic initrd. No
kernel entry assembly changes are required.

Automatic ten-core startup left the owner at U-Boot's "Starting kernel"
screen. Single-core boot with CPU hotplug allowed manual A53 startup,
including the second A53 cluster, but bringing CPU8 (the first A72) online
hung the phone. The current DT therefore omits both A72 CPU nodes and their
topology cluster, and boots with maxcpus=8. CPU hotplug remains enabled.
Secondary startup uses the existing mainline PSCI/SMP code and stock ATF.
Board-scoped log messages bracket CPU_ON to distinguish a stalled firmware
call from a failure later in secondary startup. Automatic eight-A53 boot
still needs checking on hardware; A72 investigation is deferred.

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

The initrd source and packaging tools are versioned under ``tools/x620/``.
They expect a companion workspace containing ``out/kernel``, ``dist`` and a
Debian F2FS image mounted at ``rootfs/``. ``X620_WORKSPACE`` overrides the
default workspace directory, which is the parent of the kernel checkout.
The Debian image must contain the diagnostic packages used by the builder.
With the image already mounted, reproduce the payload using::

    export X620_WORKSPACE=/path/to/workspace
    tools/x620/build-kernel.sh
    tools/x620/build-initrd.py
    tools/x620/package-linux.py

The builder assembles newc entries in memory, reads packages only from the
mounted distribution image, and includes compiled terminfo for ncurses
tools. It never unpacks a distribution filesystem into a loose directory.
The BusyBox applet list corresponds to the Debian 1.37.0 binary used here.

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

USB2 CDC ACM now uses the upstream MTU3 gadget controller, a temporary
LK-retained PHY/clock-gate provider and explicit forced VBUS detection.
Linux's MAC resource begins at 0x11271000, while the U-Boot register layout
starts at 0x11270000. Device mode uses SPI 127 from the live stock DT. USB3
and runtime power-management bring-up are excluded from this first profile.
The owner reported an interactive diagnostic shell over USB and successfully
ran commands on 2026-10-13. Transfer stress and reconnect stability remain
unverified. The initrd binds CDC ACM through configfs and starts a shell on
ttyGS0; the device identifies itself as X620-LINUX-ACM.

Captured owner logs confirm CPU1-4 manual online requests, including the
first CPU of the second A53 cluster. CPU_ON returned zero and each CPU
completed GIC redistributor initialization and secondary startup. The owner
then reported a hang when enabling CPU8; its exact stall point was not
captured. The A53-only eight-core configuration is the current checkpoint,
with automatic boot validation still pending. QEMU verified CPU1 online and
offline with the hotplug build; it does not emulate the stock ATF behavior.

Internal storage, touchscreen, networking, automatic A53 SMP validation,
suspend and native panel initialization remain follow-up work. The rich initrd is built from packages
inside a mounted Debian F2FS image, without unpacking a distribution rootfs
into a loose directory. Compiled terminfo entries for linux, vt100, xterm and
xterm-256color are included for ncurses applications such as nano.
