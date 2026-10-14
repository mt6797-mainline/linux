#!/usr/bin/env python3
"""Check native userland dependencies and kernel-module CRCs in the mounted image."""
import argparse
import importlib.util
from io import BytesIO
import json
import lzma
from pathlib import Path
import struct
import subprocess
from elftools.elf.elffile import ELFFile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    subprocess.run(['mountpoint', '-q', str(root)], check=True)
    tools = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('native_dependencies', tools / 'build-initrd.py')
    resolver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(resolver)
    resolver.ROOT = root
    programs = [
        'usr/sbin/NetworkManager', 'usr/sbin/wpa_supplicant', 'usr/sbin/wpa_cli',
        'usr/sbin/iw', 'usr/sbin/rfkill', 'usr/bin/nmcli', 'usr/bin/nm-applet',
        'usr/bin/dbus-daemon', 'usr/lib/systemd/systemd', 'usr/bin/apt-get',
        'usr/bin/dpkg', 'usr/sbin/sshd', 'usr/bin/ssh', 'usr/bin/curl',
        'usr/bin/wget', 'usr/bin/nano', 'usr/bin/mate-session',
        'usr/lib/xorg/Xorg', 'usr/sbin/lightdm', 'usr/sbin/mt6797-wmt',
        'usr/bin/evtest', 'usr/bin/libinput',
        'usr/libexec/libinput/libinput-debug-events',
    ]
    for program in programs:
        actual = resolver.canonical(resolver.root_target(program))
        if not (root / actual).exists():
            alternate = program.replace('usr/sbin/', 'usr/bin/') if 'usr/sbin/' in program else program.replace('usr/bin/', 'usr/sbin/')
            actual = resolver.canonical(resolver.root_target(alternate))
        resolver.add(actual)
    workspace = tools.parents[2]
    release = (workspace / 'out/kernel/include/config/kernel.release').read_text().strip()
    symbols = {}
    for line in (workspace / 'out/kernel/Module.symvers').read_text().splitlines():
        crc, name, *_ = line.split()
        symbols[name] = int(crc, 16)
    modules = list((root / 'usr/lib/modules' / release / 'kernel').rglob('*.ko.xz'))
    imported = 0
    for path in modules:
        elf = ELFFile(BytesIO(lzma.decompress(path.read_bytes())))
        if elf.header['e_machine'] != 'EM_AARCH64':
            raise RuntimeError('Non-ARM64 module: ' + str(path))
        versions = elf.get_section_by_name('__versions')
        if not versions:
            continue
        data = versions.data()
        if len(data) % 64:
            raise RuntimeError('Unexpected modversion record layout: ' + str(path))
        for offset in range(0, len(data), 64):
            crc = struct.unpack_from('<Q', data, offset)[0]
            name = data[offset + 8:offset + 64].split(b'\0', 1)[0].decode()
            if symbols.get(name) != crc:
                raise RuntimeError(f'Module CRC mismatch: {path.name}: {name}')
            imported += 1
    if not modules:
        raise RuntimeError('No kernel modules installed')
    result = {'ARM64_programs': programs, 'ELF_dependency_records': len(resolver.dependencies),
              'kernel_release': release, 'modules': len(modules),
              'verified_symbol_CRCs': imported, 'result': 'passed'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('Native dependency/module audit:', len(programs), 'programs,',
          len(modules), 'modules,', imported, 'symbol CRCs passed')


if __name__ == '__main__':
    main()
