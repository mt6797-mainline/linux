#!/usr/bin/env python3
"""Independent Android v2 kernel payload for the U-Boot relay."""
from pathlib import Path
import gzip
import hashlib
import json
import os
import struct
import subprocess

ROOT = Path(os.environ.get('X620_WORKSPACE', Path(__file__).resolve().parents[3]))
KERNEL_SOURCE = Path(__file__).resolve().parents[2]
DIST = ROOT / "dist"


def package(dtb, target, cmdline):
    temporary = target.with_suffix(".building")
    subprocess.run(["mkbootimg", "--header_version", "2", "--pagesize", "2048",
        "--base", "0x40000000", "--kernel_offset", "0x200000",
        "--ramdisk_offset", "0x2800000", "--dtb_offset", "0x7000000",
        "--kernel", str(DIST / "Image.gz"), "--ramdisk", str(DIST / "initrd.cpio.gz"),
        "--dtb", str(dtb), "--cmdline", cmdline, "--output", str(temporary)], check=True)
    data = temporary.read_bytes()
    assert data[:8] == b"ANDROID!"
    ks, ka, rs, ra, ss, sa, tags, page, version = struct.unpack_from("<9I", data, 8)
    ds, da = struct.unpack_from("<IQ", data, 1648)
    assert (ka, ra, da, page, version, ss) == (0x40200000, 0x42800000, 0x47000000, 2048, 2, 0)
    k = data[page:page + ks]
    assert gzip.decompress(k) == (DIST / "Image").read_bytes()
    ro = page + ((ks + page - 1) // page) * page
    do = ro + ((rs + page - 1) // page) * page
    assert data[ro:ro + rs] == (DIST / "initrd.cpio.gz").read_bytes()
    assert data[do:do + ds] == dtb.read_bytes()
    assert len(data) < 0x4000000  # Measured relay fastboot buffer, not partition size.
    offset, runtime, flags = struct.unpack_from("<QQQ", gzip.decompress(k), 8)
    assert offset == 0 and flags & 8 and 0x40200000 + runtime < 0x44000000
    # Fit between the expanded kernel and the live LK DT reservation at 0x44000000.
    assert 0x40200000 + runtime <= ra and ra + rs <= 0x44000000
    temporary.replace(target)
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "kernel_addr": hex(ka), "initrd_addr": hex(ra), "dtb_addr": hex(da)}


def main():
    cmdline = subprocess.check_output(["fdtget", "-t", "s", str(DIST / "x620.dtb"),
        "/chosen", "bootargs"], text=True).strip()
    result = package(DIST / "x620.dtb", DIST / "linux-boot.img", cmdline)
    persistent = result['bytes'] <= 0xf00000
    result.update({"format": "Android boot v2", "usage": (
        "U-Boot fastboot boot or logical flash boot" if persistent else
        "U-Boot fastboot RAM download only; exceeds logical boot capacity"),
        "persistent_slot_compatible": persistent,
        "logical_boot_offset": "0x100000", "logical_boot_capacity": 0xf00000,
        "stock_LK_fastboot": "avoid: boot/reboot handlers actively relock",
        "kernel_version": (ROOT / "out/kernel/include/config/kernel.release").read_text().strip(),
        "hardware_boot": "awaiting owner test",
        "Linux_USB_gadget": ("CDC ACM /dev/ttyGS0 shell configured" if
            "CONFIG_USB_CONFIGFS_ACM=y" in (DIST / "kernel.config").read_text() and
            "CONFIG_USB_MTU3=y" in (DIST / "kernel.config").read_text() else
            "not enabled on this board yet"),
        "persistent_console": "0x444bf000+64KiB, vendor PRZ layout",
        "kernel_entry_source_modified": False,
        "source_base": subprocess.check_output(["git", "-C", str(KERNEL_SOURCE),
            "rev-parse", "HEAD"], text=True).strip()})
    (DIST / "linux-build.json").write_text(json.dumps(result, indent=2) + "\n")
    print("U-Boot RAM payload:", result)


if __name__ == "__main__":
    main()
