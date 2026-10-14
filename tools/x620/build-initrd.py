#!/usr/bin/env python3
"""Build a rich newc initramfs directly from the mounted Debian image."""
import gzip
import hashlib
import json
import os
import posixpath
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess

SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE = Path(os.environ.get('X620_WORKSPACE', SCRIPT_DIR.parents[2]))
ROOT = WORKSPACE / 'rootfs'
OUTPUT = WORKSPACE / 'dist'
entries = {}
dependencies = {}
processed = set()


def directory(path):
    if path and path not in entries:
        parent = str(PurePosixPath(path).parent)
        if parent != '.':
            directory(parent)
        entries[path] = (stat.S_IFDIR | (0o1777 if path == 'tmp' else 0o755), b'', 0, 0)


def canonical(path):
    path = posixpath.normpath('/' + path.lstrip('/')).lstrip('/')
    for prefix in ('bin', 'sbin', 'lib', 'lib64'):
        if path == prefix or path.startswith(prefix + '/'):
            return 'usr/' + path
    return path


def root_target(path):
    """Resolve symlinks inside the target filesystem, never inside the host root."""
    parts = list(PurePosixPath('/' + path.lstrip('/')).parts[1:])
    done = []
    hops = 0
    while parts:
        part = parts.pop(0)
        if part in ('', '.'):
            continue
        if part == '..':
            if done:
                done.pop()
            continue
        candidate = ROOT.joinpath(*done, part)
        if candidate.is_symlink():
            hops += 1
            if hops > 40:
                raise RuntimeError('Symlink loop: ' + path)
            target = os.readlink(candidate)
            target_parts = list(PurePosixPath(target).parts)
            if target.startswith('/'):
                done = []
                target_parts = target_parts[1:]
            parts = target_parts + parts
        else:
            done.append(part)
    return '/'.join(done)


lib_dirs = ('usr/lib/aarch64-linux-gnu', 'usr/lib', 'usr/lib/systemd',
            'usr/lib/aarch64-linux-gnu/systemd')


def add(path):
    path = canonical(path)
    if path in processed:
        return
    processed.add(path)
    source = ROOT / path
    info = source.lstat()
    directory(str(PurePosixPath(path).parent))
    if stat.S_ISLNK(info.st_mode):
        target = os.readlink(source)
        entries[path] = (stat.S_IFLNK | 0o777, target.encode(), 0, 0)
        if target.startswith('/'):
            add(target)
        else:
            add(str(PurePosixPath(path).parent / target))
        return
    if not stat.S_ISREG(info.st_mode):
        raise RuntimeError('Unsupported source entry: ' + path)
    content = source.read_bytes()
    entries[path] = (stat.S_IFREG | stat.S_IMODE(info.st_mode), content, 0, 0)
    if not content.startswith(b'\x7fELF'):
        return
    if len(content) < 20 or int.from_bytes(content[18:20], 'little') != 183:
        raise RuntimeError('Non-AArch64 ELF: ' + path)
    dynamic = subprocess.run(['readelf', '-d', str(source)], capture_output=True,
                             text=True, check=True).stdout
    needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)
    headers = subprocess.run(['readelf', '-l', str(source)], capture_output=True,
                             text=True, check=True).stdout
    interpreter = re.search(r'Requesting program interpreter: (.*?)\]', headers)
    resolved = []
    if interpreter:
        add(interpreter.group(1))
        resolved.append(canonical(interpreter.group(1)))
    for name in needed:
        candidate = next((d + '/' + name for d in lib_dirs if (ROOT / d / name).exists()), None)
        if candidate is None:
            raise RuntimeError(f'Missing dependency of {path}: {name}')
        add(candidate)
        resolved.append(candidate)
    dependencies[path] = resolved


def generated(path, content, mode=0o644):
    directory(str(PurePosixPath(path).parent))
    entries[path] = (stat.S_IFREG | mode, content.encode() if isinstance(content, str) else content, 0, 0)


def newc():
    result = bytearray()
    for inode, (path, (mode, data, major, minor)) in enumerate(sorted(entries.items()), 1):
        name = path.encode() + b'\0'
        fields = (inode, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, 0, len(data),
                  0, 0, major, minor, len(name), 0)
        result.extend(b'070701' + ''.join(f'{v:08x}' for v in fields).encode())
        result.extend(name)
        result.extend(b'\0' * (-len(result) % 4))
        result.extend(data)
        result.extend(b'\0' * (-len(result) % 4))
    name = b'TRAILER!!!\0'
    fields = (len(entries) + 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0)
    result.extend(b'070701' + ''.join(f'{v:08x}' for v in fields).encode() + name)
    result.extend(b'\0' * (-len(result) % 512))
    return bytes(result)


def main():
    if subprocess.run(['mountpoint', '-q', str(ROOT)]).returncode:
        raise SystemExit('rootfs/ must be a mounted distribution image')
    OUTPUT.mkdir(exist_ok=True)
    for path in ('usr/bin', 'usr/sbin', 'usr/lib', 'dev', 'proc', 'sys', 'run', 'tmp',
                 'root', 'etc', 'var/lib/dpkg', 'var/lib/apt/lists/partial',
                 'var/cache/apt/archives/partial', 'var/log', 'mnt', 'media'):
        directory(path)
    for name in ('bin', 'sbin', 'lib'):
        entries[name] = (stat.S_IFLNK | 0o777, ('usr/' + name).encode(), 0, 0)
    entries['dev/console'] = (stat.S_IFCHR | 0o600, b'', 5, 1)
    entries['dev/null'] = (stat.S_IFCHR | 0o666, b'', 1, 3)
    add('usr/bin/busybox')
    applets = (SCRIPT_DIR / 'busybox-applets.txt').read_text().splitlines()
    for applet in applets:
        path = canonical(applet)
        if path == 'usr/bin/busybox':
            continue
        directory(str(PurePosixPath(path).parent))
        entries[path] = (stat.S_IFLNK | 0o777, b'/usr/bin/busybox', 0, 0)
    binaries = [
        'usr/bin/bash', 'usr/bin/strace', 'usr/bin/dtc', 'usr/bin/fdtget', 'usr/bin/fdtput',
        'usr/bin/lsusb', 'usr/bin/lsblk', 'usr/bin/less', 'usr/bin/nano',
        'usr/bin/jq', 'usr/bin/nc.openbsd',
        'usr/bin/ps', 'usr/bin/top', 'usr/bin/free', 'usr/bin/uptime',
        'usr/sbin/ip', 'usr/sbin/ss', 'usr/sbin/ethtool',
        'usr/sbin/i2cdetect', 'usr/sbin/i2cget', 'usr/sbin/i2cset', 'usr/sbin/i2cdump',
        'usr/sbin/blkid', 'usr/sbin/fdisk', 'usr/bin/mount', 'usr/bin/umount',
        'usr/bin/dmesg', 'usr/sbin/dropbear', 'usr/bin/dbclient',
        'usr/bin/dropbearkey', 'usr/sbin/mkfs.f2fs', 'usr/sbin/fsck.f2fs',
        'usr/sbin/dump.f2fs', 'usr/sbin/resize.f2fs', 'usr/sbin/mke2fs', 'usr/sbin/e2fsck',
        'usr/bin/modetest', 'usr/sbin/insmod',
    ]
    # Some Debian tools are installed in /usr/bin rather than /usr/sbin.
    for path in binaries:
        if not (ROOT / path).exists():
            alternative = path.replace('usr/sbin/', 'usr/bin/') if 'usr/sbin/' in path else path.replace('usr/bin/', 'usr/sbin/')
            if not (ROOT / alternative).exists():
                raise RuntimeError('Required diagnostic tool missing: ' + path)
            path = alternative
        add(path)
    for path in ('etc/passwd', 'etc/group', 'etc/nsswitch.conf', 'etc/hosts',
                 'etc/ssl/certs/ca-certificates.crt',
                 'usr/share/keyrings/debian-archive-keyring.pgp',
                 'usr/share/keyrings/debian-archive-keyring.gpg'):
        if (ROOT / path).exists():
            add(path)
    # ncurses tools such as nano need compiled terminal descriptions too.
    for terminal in ('l/linux', 'v/vt100', 'x/xterm', 'x/xterm-256color'):
        add('usr/share/terminfo/' + terminal)
    for source, dest in [('initrd-init.sh', 'init'),
                         ('initrd-shell.sh', 'usr/bin/x620-shell'),
                         ('initrd-rootfs.sh', 'usr/bin/x620-rootfs'),
                         ('initrd-usb.sh', 'usr/bin/x620-usb')]:
        generated(dest, (SCRIPT_DIR / source).read_bytes(), 0o755)
    generated('etc/hostname', 'x620-initrd\n')
    generated('etc/resolv.conf', 'nameserver 1.1.1.1\n')
    generated('etc/profile', 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin\nexport TERM=linux\n')
    generated('etc/apt/sources.list', 'deb [signed-by=/usr/share/keyrings/debian-archive-keyring.pgp] https://mirrors.tuna.tsinghua.edu.cn/debian trixie main\ndeb [signed-by=/usr/share/keyrings/debian-archive-keyring.pgp] https://mirrors.tuna.tsinghua.edu.cn/debian-security trixie-security main\n')
    # The initrd is a curated rescue filesystem, not the full image's dpkg state.
    generated('var/lib/dpkg/status', '')
    generated('etc/motd', 'X620 first-boot diagnostic initrd\n')
    for alias, target in [('usr/bin/nc', '/usr/bin/nc.openbsd')]:
        entries[alias] = (stat.S_IFLNK | 0o777, target.encode(), 0, 0)
    archive = newc()
    compressed = gzip.compress(archive, compresslevel=9, mtime=0)
    (OUTPUT / 'initrd.cpio.gz').write_bytes(compressed)
    manifest = {'format': 'newc+gzip', 'architecture': 'aarch64',
                'entry_count': len(entries), 'uncompressed_bytes': len(archive),
                'compressed_bytes': len(compressed),
                'sha256': hashlib.sha256(compressed).hexdigest(),
                'elf_dependencies': dependencies,
                'entries': {name: {'mode': oct(mode), 'bytes': len(data),
                                  'symlink': data.decode() if stat.S_ISLNK(mode) else None}
                            for name, (mode, data, major, minor) in sorted(entries.items())}}
    (OUTPUT / 'initrd-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Initrd entries:', len(entries), 'compressed bytes:', len(compressed))


if __name__ == '__main__':
    main()
