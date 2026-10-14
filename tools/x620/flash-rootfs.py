#!/usr/bin/env python3
"""Stream the prepared gzip image to X620 userdata from a PC running adb.

Run this explicitly in TWRP. It overwrites userdata, as selected by the owner.
No phone-side staging file is stored on the partition being overwritten.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path, help='debian-arm64-rootfs.img.gz')
    parser.add_argument('--manifest', type=Path, help='defaults to rootfs-build.json beside image')
    parser.add_argument('--serial', help='adb serial, if more than one device is connected')
    args = parser.parse_args()
    manifest = args.manifest or args.image.with_name('rootfs-build.json')
    data = json.loads(manifest.read_text())
    if data['filesystem'] != 'f2fs' or data['architecture'] != 'arm64':
        raise SystemExit('Expected the X620 ARM64 F2FS image')
    if data['image_bytes'] % 1048576:
        raise SystemExit('Image size is not an integral number of MiB')
    digest = hashlib.sha256()
    with args.image.open('rb') as image:
        while block := image.read(1024 * 1024):
            digest.update(block)
    if digest.hexdigest() != data['gzip_sha256']:
        raise SystemExit('Image SHA256 does not match rootfs-build.json')
    adb = ['adb'] + (['-s', args.serial] if args.serial else [])

    def remote(command, check=True):
        return subprocess.run(adb + ['shell', command], check=check,
                              capture_output=True, text=True).stdout.strip()

    if not remote('getprop ro.twrp.version') or remote('id -u') != '0':
        raise SystemExit('Boot X620 into a root TWRP adb shell first')
    device = remote('for p in /dev/block/by-name/userdata /dev/block/platform/*/by-name/userdata /dev/block/platform/*/*/by-name/userdata; do if test -b "$p"; then readlink -f "$p"; break; fi; done')
    if device != '/dev/block/mmcblk0p38':
        raise SystemExit(f'Expected stock X620 userdata to resolve to mmcblk0p38, got {device!r}')
    capacity = int(remote(f'blockdev --getsize64 {device}'))
    if capacity < data['image_bytes']:
        raise SystemExit('Rootfs image is larger than userdata')
    remote('command -v gzip')
    remote('command -v sha256sum')
    remote('twrp unmount /data')
    mounts = remote('cat /proc/mounts')
    if any(line.split()[1] == '/data' or line.split()[0] == device
           for line in mounts.splitlines() if len(line.split()) >= 2):
        raise SystemExit('userdata remains mounted; unmount Data in TWRP before retrying')
    print(f'Writing {args.image.name} to userdata ({device}); existing data is overwritten.', flush=True)
    command = f'gzip -dc | dd of={device} bs=1048576; result=$?; sync; exit "$result"'
    process = subprocess.Popen(adb + ['exec-in', 'sh -c ' + shlex.quote(command)], stdin=subprocess.PIPE)
    sent = 0
    last = time.monotonic()
    try:
        with args.image.open('rb') as image:
            while block := image.read(1024 * 1024):
                process.stdin.write(block)
                sent += len(block)
                if time.monotonic() - last > 5:
                    print(f'Sent {sent // 1048576} MiB compressed', flush=True)
                    last = time.monotonic()
        process.stdin.close()
        if process.wait() != 0:
            raise SystemExit('TWRP write command failed; remain in recovery')
    except BaseException:
        if process.poll() is None:
            process.terminate()
            process.wait()
        raise
    print('Verifying written image SHA256...', flush=True)
    count = data['image_bytes'] // 1048576
    readback = remote(f'dd if={device} bs=1048576 count={count} | sha256sum').split()[0]
    if readback != data['image_sha256']:
        raise SystemExit('userdata readback SHA256 mismatch; remain in TWRP')
    probe = remote(f'blkid {device}')
    found = re.search(r'UUID="([^"]+)"', probe)
    if not found or found.group(1) != data['UUID'] or 'TYPE="f2fs"' not in probe:
        raise SystemExit(f'Written filesystem identity mismatch: {probe}')
    print('F2FS rootfs written. First Linux boot grows it to userdata capacity.')
    print('Use the existing U-Boot relay to load linux-boot.img; this tool does not reboot or flash boot.')


if __name__ == '__main__':
    main()
