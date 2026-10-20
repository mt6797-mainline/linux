#!/usr/bin/env python3
"""Package native Debian packages and board support for an offline rootfs update."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
from elftools.elf.elffile import ELFFile
from concurrent.futures import ThreadPoolExecutor, as_completed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    if os.geteuid() != 0:
        raise SystemExit('Run as root with the Debian F2FS image mounted at rootfs/')
    subprocess.run(['mountpoint', '-q', str(root)], check=True)
    fs = subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '--mountpoint', str(root)], text=True).strip()
    if fs != 'f2fs':
        raise SystemExit('The package source must be the mounted F2FS image')
    workspace = Path(__file__).resolve().parents[3]
    release = (workspace / 'out/kernel/include/config/kernel.release').read_text().strip()
    image = workspace / 'dist/debian-arm64-rootfs.img'
    if image.stat().st_size < 6 * 1024**3:
        source = subprocess.check_output(['findmnt', '-n', '-o', 'SOURCE', '--mountpoint', str(root)], text=True).strip()
        backing = subprocess.check_output(['losetup', '-n', '-O', 'BACK-FILE', source], text=True).strip()
        if Path(backing).resolve() != image.resolve():
            raise SystemExit('Refusing to resize a different mounted filesystem')
        subprocess.run(['sync', '-f', str(root)], check=True)
        subprocess.run(['umount', str(root)], check=True)
        with image.open('r+b') as stream:
            stream.truncate(6 * 1024**3)
        subprocess.run(['resize.f2fs', str(image)], check=True)
        subprocess.run(['mount', '-t', 'f2fs', '-o', 'loop', str(image), str(root)], check=True)

    def target(*command):
        return subprocess.run(['chroot', str(root), *command], check=True)

    cache = root / 'var/cache/apt/archives'
    cache.mkdir(parents=True, exist_ok=True)
    stage = root / 'var/tmp/mt6797-x620-platform-package'
    if stage.exists():
        raise SystemExit('Unfinished package staging directory: ' + str(stage))
    stage.mkdir()

    def write(path, data, mode=0o644):
        dest = stage / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data.encode() if isinstance(data, str) else data)
        dest.chmod(mode)

    selected = [
        'usr/sbin/mt6797-wmt', 'usr/sbin/mt6797-wifi',
        'usr/sbin/x620-usb-gadget', 'usr/sbin/x620-zram',
        'usr/sbin/mt6797-usb-console-hangup',
        'usr/share/mt6797-x620/rootfs-packages.txt',
        'etc/systemd/system/mt6797-wifi.service',
        'etc/systemd/system/x620-usb-gadget.service',
        'etc/systemd/system/x620-zram.service',
        'etc/systemd/system/mt6797-x620-ssh-hostkeys.service',
        'etc/systemd/system/ssh.service.d/50-mt6797-x620.conf',
        'etc/systemd/system/serial-getty@ttyGS0.service.d/50-x620.conf',
        'etc/systemd/system/session-.scope.d/50-mt6797-x620.conf',
        'etc/NetworkManager/conf.d/20-mt6797-x620-wifi.conf',
    ]
    selected.extend('usr/lib/firmware/mediatek/mt6797/' + name for name in
                    ('WIFI_RAM_CODE_6797', 'ROMv3_patch_1_0_hdr.bin',
                     'ROMv3_patch_1_1_hdr.bin', 'WMT_SOC.cfg'))
    for path in selected:
        src = root / path
        write(path, src.read_bytes(), src.stat().st_mode & 0o777)
    modules = root / 'usr/lib/modules' / release
    for src in sorted(modules.rglob('*')):
        if src.is_file() and not src.is_symlink():
            write(str(src.relative_to(root)), src.read_bytes(), src.stat().st_mode & 0o777)
    with (workspace / 'out/kernel/vmlinux').open('rb') as stream:
        notes = ELFFile(stream).get_section_by_name('.notes').data()
    write('usr/lib/mt6797-x620/kernel.notes', notes)
    write('usr/sbin/mt6797-x620-kernel-check', '''#!/bin/sh
set -eu
# Builtin Wi-Fi uses no external modules; the kernel owns the complete stack.
if test -d /sys/bus/platform/drivers/mt-wifi && ! test -e /sys/bus/platform/drivers/mt-wifi/module; then
    exit 0
fi
cmp -s /sys/kernel/notes /usr/lib/mt6797-x620/kernel.notes || {
    echo 'MT6797: running kernel and installed module build differ' >&2
    exit 1
}
''', 0o755)
    unit = stage / 'etc/systemd/system/mt6797-wifi.service'
    if 'ExecStartPre=/usr/sbin/mt6797-x620-kernel-check' not in unit.read_text():
        unit.write_text(unit.read_text().replace('[Service]\n',
                        '[Service]\nExecStartPre=/usr/sbin/mt6797-x620-kernel-check\n'))
    signature = hashlib.sha256()
    for path in sorted(stage.rglob('*')):
        if path.is_file():
            signature.update(str(path.relative_to(stage)).encode())
            signature.update(path.read_bytes())
    # Git snapshot versions sort after the earlier hexadecimal-only versions;
    # timestamps also keep successive local builds in upgrade order for APT.
    stamp = datetime.now(timezone.utc).strftime('%Y%m%d%H%M%S')
    version = release.split('-')[0] + '+mt6797.git' + stamp + '.' + signature.hexdigest()[:12]
    size = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file()) // 1024
    write('DEBIAN/control',
          f'Package: mt6797-x620-platform\nVersion: {version}\nArchitecture: arm64\n'
          'Maintainer: dysorder <dysorder@users.noreply.github.com>\n'
          f'Installed-Size: {size}\nDepends: kmod, systemd, network-manager, wpasupplicant, wireless-regdb, iw, python3-minimal\n'
          'Description: MT6797 X620 kernel modules, firmware and native system services\n')
    write('DEBIAN/preinst', f'''#!/bin/sh
set -eu
modules=/usr/lib/modules/{release}
if test "$1" = install && test -d "$modules" && ! test -e "$modules.before-mt6797-x620"; then
    mv "$modules" "$modules.before-mt6797-x620"
fi
''', 0o755)
    write('DEBIAN/postinst', f'''#!/bin/sh
set -eu
test "$1" = configure || exit 0
depmod -a '{release}'
update-alternatives --set regulatory.db /lib/firmware/regulatory.db-upstream
systemctl --no-reload enable wpa_supplicant.service mt6797-wifi.service x620-usb-gadget.service
if test "$(cat /proc/1/comm 2>/dev/null || true)" = systemd; then
    systemctl daemon-reload
fi
''', 0o755)
    board_deb = cache / 'mt6797-x620-platform.deb'
    subprocess.run(['dpkg-deb', '--build', '--root-owner-group', str(stage), str(board_deb)], check=True)
    status = subprocess.check_output(['chroot', str(root), '/usr/bin/dpkg-query', '-W',
                                     '-f=${db:Status-Status}\t${binary:Package}=${Version}\n'], text=True)
    packages = [line.split('\t', 1)[1] for line in status.splitlines()
                if line.startswith('installed\t') and not line.split('\t', 1)[1].startswith('mt6797-x620-platform=')]
    uris = subprocess.check_output(['chroot', str(root), '/usr/bin/apt-get',
                                    '--print-uris', 'download', *packages], text=True)
    requests = [shlex.split(line) for line in uris.splitlines() if line.startswith("'")]

    def fetch(request):
        url, filename, size, checksum = request
        if Path(filename).name != filename:
            raise RuntimeError('Unexpected archive filename: ' + filename)
        algorithm, digest = checksum.split(':', 1)
        if algorithm != 'SHA256':
            raise RuntimeError('Package repository did not supply SHA256: ' + filename)
        path = cache / filename

        def valid(candidate):
            if not candidate.exists() or candidate.stat().st_size != int(size):
                return False
            with candidate.open('rb') as stream:
                return hashlib.file_digest(stream, 'sha256').hexdigest() == digest

        if valid(path):
            return path
        temporary = path.with_name(path.name + '.mt6797-download')
        subprocess.run(['curl', '--fail', '--location', '--silent', '--show-error',
                        '--retry', '3', '--retry-all-errors', '--connect-timeout', '15', '--max-time', '180',
                        '--output', str(temporary), url], check=True)
        if not valid(temporary):
            raise RuntimeError('APT SHA256/size verification failed: ' + filename)
        temporary.replace(path)
        return path

    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = [pool.submit(fetch, request) for request in requests]
        for number, future in enumerate(as_completed(futures), 1):
            future.result()
            if number % 100 == 0 or number == len(futures):
                print(f'APT archive SHA256 verified: {number}/{len(futures)}', flush=True)
    policy = root / 'usr/sbin/policy-rc.d'
    existed = policy.exists()
    if not existed:
        policy.write_text('#!/bin/sh\nexit 101\n')
        policy.chmod(0o755)
    try:
        target('/usr/bin/dpkg', '-i', '/var/cache/apt/archives/mt6797-x620-platform.deb')
    finally:
        if not existed:
            policy.unlink()
    import shutil
    shutil.rmtree(stage)
    # The build source did not have old modules; retain the backup only on the phone.
    backup = modules.with_name(release + '.before-mt6797-x620')
    if backup.exists():
        shutil.rmtree(backup)
    # Ignore any superseded archive left by an earlier upgrade in the same build.
    installed = {request.split('=', 1)[0].split(':', 1)[0]: request.split('=', 1)[1]
                 for request in packages}
    installed['mt6797-x620-platform'] = version
    selected_debs = {}
    for path in sorted(cache.glob('*.deb')):
        fields = subprocess.check_output(['dpkg-deb', '-f', str(path),
                                          'Package', 'Version', 'Architecture'], text=True)
        control = dict(line.split(': ', 1) for line in fields.splitlines())
        if installed.get(control['Package']) == control['Version']:
            if control['Architecture'] not in ('arm64', 'all'):
                raise SystemExit('Wrong package architecture: ' + str(path))
            selected_debs[control['Package']] = path
    debs = sorted(selected_debs.values())
    if len(debs) != len(installed):
        raise SystemExit('Offline bundle does not include every installed package')
    checksums = ''.join(hashlib.file_digest(p.open('rb'), 'sha256').hexdigest() +
                        '  packages/' + p.name + '\n' for p in debs)
    installer = '''#!/bin/sh
set -eu
test "$(id -u)" = 0 || { echo 'Run as root' >&2; exit 1; }
bundle=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$bundle"
sha256sum -c SHA256SUMS >/dev/null
policy=/usr/sbin/policy-rc.d
own_policy=0
if ! test -e "$policy"; then
    printf '#!/bin/sh\\nexit 101\\n' > "$policy"
    chmod 755 "$policy"
    own_policy=1
fi
cleanup() { test "$own_policy" != 1 || rm -f "$policy"; }
trap cleanup EXIT
if test "$(cat /proc/1/comm 2>/dev/null || true)" = systemd; then
    systemctl stop NetworkManager.service
fi
apt-get --reinstall -y -o Dir::Etc::sourcelist="$bundle/sources.list" \
    -o Dir::Etc::sourceparts="$bundle/sources.list.d" \
    -o Dpkg::Options::=--force-confold install "$bundle"/packages/*.deb
dpkg --audit
apt-get check
cleanup
own_policy=0
if test "$(cat /proc/1/comm 2>/dev/null || true)" = systemd; then
    systemctl daemon-reload
    systemctl start wpa_supplicant.service
    if cmp -s /sys/kernel/notes /usr/lib/mt6797-x620/kernel.notes; then
        systemctl start mt6797-wifi.service
    else
        echo 'Kernel modules installed; boot the matching linux-boot.img before loading them.'
    fi
    systemctl start NetworkManager.service
fi
echo MT6797_ROOTFS_UPDATE_OK
'''
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # The same native package can update modules without reinstalling the distro.
    platform_output = args.output.parent / 'mt6797-x620-platform.deb'
    platform_temporary = platform_output.with_suffix('.deb.building')
    shutil.copyfile(board_deb, platform_temporary)
    platform_temporary.replace(platform_output)
    temporary = args.output.with_suffix('.building')
    with tarfile.open(temporary, 'w') as archive:
        prefix = 'mt6797-x620-rootfs-update/'
        for name, data, mode in [('install.sh', installer.encode(), 0o755),
                                 ('SHA256SUMS', checksums.encode(), 0o644),
                                 ('sources.list', b'', 0o644)]:
            entry = tarfile.TarInfo(prefix + name)
            entry.mode, entry.size = mode, len(data)
            archive.addfile(entry, io.BytesIO(data))
        directory = tarfile.TarInfo(prefix + 'sources.list.d')
        directory.type, directory.mode = tarfile.DIRTYPE, 0o755
        archive.addfile(directory)
        for path in debs:
            archive.add(path, prefix + 'packages/' + path.name, recursive=False)
    temporary.replace(args.output)
    result = {'packages': len(debs), 'bytes': args.output.stat().st_size,
              'sha256': hashlib.file_digest(args.output.open('rb'), 'sha256').hexdigest(),
              'kernel_release': release, 'platform_package_version': version,
              'scope': 'native ARM64/all Debian packages; preserves userdata; standard module tree'}
    args.output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Offline update:', result, flush=True)


if __name__ == '__main__':
    main()
