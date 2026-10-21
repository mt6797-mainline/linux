#!/usr/bin/env python3
"""Boot the real F2FS image through initrd into ARM64 systemd.

Uses the existing test kernel output with test-only virtio transport. This
does not emulate MT6797 USB/display hardware or establish a phone GUI boot.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    root = Path(os.environ.get('MT6797_WORKSPACE') or os.environ.get('X620_WORKSPACE', Path(__file__).resolve().parents[3]))
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', type=Path, default=root / 'dist/debian-arm64-rootfs.img')
    args = parser.parse_args()
    test = root / 'artifacts/rootfs-qemu-test'
    test.mkdir(exist_ok=True)
    disk = test / 'rootfs.img'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always',
                    str(args.image), str(disk)], check=True)
    # Exercise first-boot offline growth without modifying the delivered image.
    with disk.open('r+b') as stream:
        stream.truncate(max(args.image.stat().st_size + 2 * 1024**3, 6 * 1024**3))
    log = test / 'boot.log'
    command = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a53',
               '-m', '3G', '-smp', '4', '-nographic', '-monitor', 'none',
               '-kernel', str(test / 'Image'),
               '-initrd', str(root / 'dist/initrd.cpio.gz'), '-append',
               'console=ttyAMA0,115200 loglevel=6 rdinit=/init cpuidle.off=1 '
               'systemd.unit=kernel-command-line.target '
               'systemd.mask=x620-usb-gadget.service systemd.run=/usr/lib/x620/boot-check '
               'systemd.run_success_action=poweroff systemd.run_failure_action=poweroff',
               '-drive', f'if=none,id=rootfs,file={disk},format=raw',
               '-device', 'virtio-blk-device,drive=rootfs']
    with log.open('wb') as output:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=180)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)
            raise
    text = log.read_text(errors='replace')
    marker = 'X620_SYSTEMD_ROOTFS_OK'
    result = {'kernel':'production build reused with test-only virtio transport',
              'PID1_systemd':marker in text,
              'initrd_switch_root':'X620_ROOTFS_READY:' in text,
              'F2FS_growth':'X620: growing F2FS' in text,
              'userspace_check':marker in text,
              'exit_code':process.returncode,
              'scope':'ARM64 systemd/udev/F2FS/zram/packages; phone USB and MATE GUI require hardware testing'}
    (test / 'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    if not result['userspace_check']:
        print(text[-6500:])
        raise SystemExit(1)


if __name__ == '__main__':
    main()
