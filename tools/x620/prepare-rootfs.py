#!/usr/bin/env python3
"""Configure the mounted Debian ARM64 F2FS image for X620 and MATE."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def install_user_password(root, hash_path):
    """Apply the locally configured password without storing plaintext."""
    if not hash_path.is_file():
        return
    password_hash = hash_path.read_text().strip()
    if not password_hash.startswith('$') or any(c in password_hash for c in '\n:'):
        raise ValueError('Invalid desktop-account password hash')
    subprocess.run(['chroot', str(root), '/usr/sbin/chpasswd', '-e'],
                   input=('dysorder:' + password_hash + '\n').encode(), check=True)


def install_user_ssh_key(root, key_path):
    """Install the workspace's persistent public key for the desktop account."""
    if not key_path.is_file():
        return
    subprocess.run(['ssh-keygen', '-l', '-f', str(key_path)], check=True,
                   stdout=subprocess.DEVNULL)
    key = key_path.read_text().strip()
    uid = int(subprocess.check_output(['chroot', str(root), 'id', '-u', 'dysorder']))
    gid = int(subprocess.check_output(['chroot', str(root), 'id', '-g', 'dysorder']))
    directory = root / 'home/dysorder/.ssh'
    directory.mkdir(parents=True, exist_ok=True)
    directory.chmod(0o700)
    os.chown(directory, uid, gid)
    authorized = directory / 'authorized_keys'
    keys = authorized.read_text().splitlines() if authorized.exists() else []
    identity = ' '.join(key.split()[:2])
    if not any(identity in line for line in keys):
        keys.append(key)
    authorized.write_text('\n'.join(keys) + '\n')
    authorized.chmod(0o600)
    os.chown(authorized, uid, gid)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    if os.geteuid() != 0:
        raise SystemExit('Run with sudo: configuration is written inside the mounted image')
    subprocess.run(['mountpoint', '-q', str(root)], check=True)
    fs = subprocess.check_output(['findmnt', '-n', '-o', 'FSTYPE', '--mountpoint', str(root)], text=True).strip()
    if fs != 'f2fs':
        raise SystemExit('rootfs/ must be the mounted F2FS distribution image')

    def target(*command):
        return subprocess.run(['chroot', str(root), *command], check=True)

    def write(name, content, mode=0o644):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        path.chmod(mode)

    script_dir = Path(__file__).resolve().parent
    workspace = Path(os.environ.get('X620_WORKSPACE', script_dir.parents[2]))
    package_profile = (script_dir / 'rootfs-packages.txt').read_text()
    requested = package_profile.splitlines()
    status = subprocess.run(['chroot', str(root), '/usr/bin/dpkg-query', '-W',
                             '-f=${binary:Package}\t${db:Status-Status}\\n', *requested],
                            capture_output=True, text=True)
    installed = {line.split('\t')[0].split(':')[0] for line in status.stdout.splitlines()
                 if line.endswith('\tinstalled')}
    missing = [name for name in requested if name not in installed]
    if missing:
        target('/usr/bin/env', 'DEBIAN_FRONTEND=noninteractive',
               '/usr/bin/apt-get', 'install', '-y', *missing)
    write('usr/share/mt6797-x620/rootfs-packages.txt', package_profile)
    write('usr/sbin/mt6797-wifi', (script_dir / 'mt6797-wifi.sh').read_text(), 0o755)
    # The production kernel trusts upstream wireless-regdb signers.
    target('/usr/bin/update-alternatives', '--set', 'regulatory.db',
           '/lib/firmware/regulatory.db-upstream')
    firmware_dir = root / 'usr/lib/firmware/mediatek/mt6797'
    firmware_dir.mkdir(parents=True, exist_ok=True)
    for name in ('WIFI_RAM_CODE_6797', 'ROMv3_patch_1_0_hdr.bin',
                 'ROMv3_patch_1_1_hdr.bin', 'WMT_SOC.cfg'):
        source = workspace / 'artifacts/stock-x620/connectivity/flashone' / name
        destination = firmware_dir / name
        destination.write_bytes(source.read_bytes())
        destination.chmod(0o644)
    write('etc/systemd/system/mt6797-wifi.service',
          '[Unit]\nDescription=MT6797 WLAN firmware and function control\n'
          'ConditionPathExists=/sys/firmware/devicetree/base/consys@18070000\n'
          'After=systemd-udevd.service\nBefore=NetworkManager.service\n'
          '[Service]\nType=oneshot\nRemainAfterExit=yes\n'
          'ExecStart=/usr/sbin/mt6797-wifi start\nExecStop=/usr/sbin/mt6797-wifi stop\n'
          'TimeoutStartSec=90\nTimeoutStopSec=30\n'
          '[Install]\nWantedBy=multi-user.target\n')
    write('etc/NetworkManager/conf.d/20-mt6797-x620-wifi.conf',
          '[device-mt6797-x620]\nmatch-device=interface-name:wlan0\n'
          'wifi.scan-rand-mac-address=no\n'
          '[connection-mt6797-x620]\nmatch-device=interface-name:wlan0\n'
          'wifi.cloned-mac-address=preserve\n')

    for group in ('sudo', 'audio', 'video', 'input', 'netdev'):
        target('/usr/sbin/groupadd', '-f', group)
    if subprocess.run(['chroot', str(root), '/usr/bin/id', 'dysorder'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        target('/usr/sbin/useradd', '-m', '-s', '/bin/bash', '-G', 'sudo,audio,video,input,netdev', 'dysorder')
    install_user_ssh_key(root, workspace / 'cfg/mt6797-x620-ssh.pub')
    install_user_password(root, workspace / 'cfg/mt6797-x620-password.hash')

    source = subprocess.check_output(['findmnt', '-n', '-o', 'SOURCE', '--mountpoint', str(root)], text=True).strip()
    uuid = subprocess.check_output(['blkid', '-s', 'UUID', '-o', 'value', source], text=True).strip()
    write('etc/fstab', f'UUID={uuid} / f2fs defaults,noatime 0 0\n')
    write('etc/hostname', 'x620\n')
    write('etc/hosts', '127.0.0.1 localhost\n127.0.1.1 x620\n::1 localhost ip6-localhost ip6-loopback\n')
    write('etc/x620-rootfs-release', 'Debian 13 ARM64 / MATE / X620\n')
    write('etc/machine-id', '')
    machine_id = root / 'var/lib/dbus/machine-id'
    if machine_id.exists() or machine_id.is_symlink():
        machine_id.unlink()
    machine_id.parent.mkdir(parents=True, exist_ok=True)
    machine_id.symlink_to('/etc/machine-id')
    write('etc/apt/sources.list',
          'deb https://mirrors.tuna.tsinghua.edu.cn/debian trixie main\n'
          'deb https://mirrors.tuna.tsinghua.edu.cn/debian trixie-updates main\n'
          'deb https://mirrors.tuna.tsinghua.edu.cn/debian-security trixie-security main\n')
    write('etc/sudoers.d/90-x620-bringup', 'dysorder ALL=(ALL) NOPASSWD: ALL\n', 0o440)
    write('etc/lightdm/lightdm.conf.d/50-x620.conf',
          '[Seat:*]\nautologin-user=dysorder\nautologin-user-timeout=0\n'
          'user-session=mate\ngreeter-session=lightdm-gtk-greeter\n'
          'xserver-command=X -dpi 192 -s 0 -dpms\n')
    write('etc/X11/xorg.conf.d/20-x620-kms.conf',
          'Section "Device"\n    Identifier "X620 native DRM"\n'
          '    Driver "modesetting"\n    Option "AccelMethod" "none"\n'
          '    Option "SWcursor" "true"\nEndSection\n')
    write('etc/dconf/profile/user', 'user-db:user\nsystem-db:local\n')
    write('etc/dconf/db/local.d/00-x620',
          '[org/mate/marco/general]\ncompositing-manager=false\n'
          '[org/mate/desktop/font-rendering]\ndpi=192.0\n'
          '[org/mate/desktop/interface]\naccessibility=true\n'
          '[org/mate/screensaver]\nidle-activation-enabled=false\nlock-enabled=false\n')
    write('etc/systemd/journald.conf.d/50-x620.conf',
          '[Journal]\nStorage=persistent\nSystemMaxUse=32M\nRuntimeMaxUse=16M\n')
    # Reuse the diagnostic initrd's serial setup, without its shell loop.
    serial_setup = Path(__file__).with_name('initrd-usb.sh').read_text()
    serial_setup = serial_setup[serial_setup.index("echo 'Waiting for USB device controller'"):
                                serial_setup.index('test -c /dev/ttyGS0')]
    write('usr/sbin/x620-usb-gadget', '#!/bin/sh\nset -eu\n'
          'fail() { echo "$*" >&2; exit 1; }\n'
          'mkdir -p /run/diag\nstatus=/run/diag/usb-serial-status\n' + serial_setup, 0o755)
    write('etc/systemd/system/x620-usb-gadget.service',
          '[Unit]\nDescription=X620 USB serial console\n'
          'Requires=sys-kernel-config.mount\nAfter=sys-kernel-config.mount systemd-udevd.service\n'
          'Wants=serial-getty@ttyGS0.service\nBefore=serial-getty@ttyGS0.service\n'
          '[Service]\nType=oneshot\nExecStart=/usr/sbin/x620-usb-gadget\nRemainAfterExit=yes\n'
          '[Install]\nWantedBy=multi-user.target\n')
    write('etc/systemd/system/mt6797-x620-ssh-hostkeys.service',
          '[Unit]\nDescription=Generate per-device SSH host keys\nBefore=ssh.service\n'
          '[Service]\nType=oneshot\nExecStart=/usr/bin/ssh-keygen -A\nRemainAfterExit=yes\n'
          '[Install]\nWantedBy=multi-user.target\n')
    write('etc/systemd/system/ssh.service.d/50-mt6797-x620.conf',
          '[Unit]\nRequires=mt6797-x620-ssh-hostkeys.service\nAfter=mt6797-x620-ssh-hostkeys.service\n')
    for host_key in (root / 'etc/ssh').glob('ssh_host_*'):
        host_key.unlink()
    write('etc/xdg/autostart/mt6797-x620-keyboard.desktop',
          '[Desktop Entry]\nType=Application\nName=Screen keyboard\nName[zh_CN]=屏幕键盘\n'
          'Exec=onboard\nIcon=onboard\nOnlyShowIn=MATE;\nX-MATE-Autostart-enabled=true\n')
    write('etc/dconf/db/local.d/10-mt6797-x620-keyboard',
          '[org/onboard]\nlayout=\'Compact\'\nstart-minimized=false\n'
          '[org/onboard/window]\ndocking-enabled=true\n'
          '[org/onboard/window/portrait]\ndock-expand=true\ndock-height=400\n'
          '[org/onboard/window/landscape]\ndock-expand=true\ndock-height=300\n'
          '[org/onboard/auto-show]\nenabled=true\n')
    write('etc/systemd/system/serial-getty@ttyGS0.service.d/50-x620.conf',
          '[Service]\nExecStart=\n'
          'ExecStart=-/sbin/agetty --autologin root --noclear --keep-baud 115200,38400,9600 - vt100\n'
          'ExecStop=-/usr/sbin/mt6797-usb-console-hangup\n'
          'TimeoutStopSec=10s\n')
    write('usr/sbin/mt6797-usb-console-hangup',
          Path(__file__).with_name('mt6797-usb-console-hangup.py').read_text(), 0o755)
    write('etc/systemd/system/session-.scope.d/50-mt6797-x620.conf',
          '[Scope]\nSendSIGHUP=yes\nTimeoutStopSec=10s\n')
    write('etc/systemd/system/x620-zram.service',
          '[Unit]\nDescription=X620 compressed RAM swap\nBefore=display-manager.service\n'
          'ConditionPathExists=/sys/block/zram0/disksize\n'
          '[Service]\nType=oneshot\nExecStart=/usr/sbin/x620-zram\nRemainAfterExit=yes\n'
          '[Install]\nWantedBy=multi-user.target\n')
    write('usr/sbin/x620-zram', '''#!/bin/sh
set -eu
test "$(cat /sys/block/zram0/disksize)" = 0 || exit 0
echo lz4 > /sys/block/zram0/comp_algorithm
size=$(awk '/MemTotal:/ { size=$2*512; if (size>1073741824) size=1073741824; printf "%.0f\\n",size }' /proc/meminfo)
echo "$size" > /sys/block/zram0/disksize
mkswap /dev/zram0
swapon -p 100 /dev/zram0
''', 0o755)
    write('usr/lib/x620/boot-check', '''#!/bin/sh
set -eu
test "$(cat /proc/1/comm)" = systemd
systemctl is-active systemd-udevd.service
test "$(systemctl show -p Result --value systemd-sysusers.service)" = success
flock /run/x620-boot-check.lock /bin/true
findmnt -n -o FSTYPE / | grep -x f2fs
id dysorder
apt --version
apt-get check
test "$(dpkg-query -W -f='${db:Status-Status}\\n' wpasupplicant iw rfkill | grep -c installed)" = 3
systemctl start wpa_supplicant.service
systemctl is-active wpa_supplicant.service
busctl get-property fi.w1.wpa_supplicant1 /fi/w1/wpa_supplicant1 fi.w1.wpa_supplicant1 Capabilities
iw --version
nmcli --version
test "$(dpkg-query -W -f='${db:Status-Status}' mate-session-manager)" = installed
test -r /usr/share/xsessions/mate.desktop
gsettings range org.mate.Marco.general compositing-manager
systemctl start x620-zram.service
test "$(swapon --noheadings --show=NAME)" = /dev/zram0
swapon --show
echo X620_SYSTEMD_ROOTFS_OK
''', 0o755)
    target('/usr/bin/dconf', 'update')
    target('/usr/sbin/visudo', '-cf', '/etc/sudoers')
    # The build-time policy prevents package scripts from starting host services.
    (root / 'usr/sbin/policy-rc.d').unlink(missing_ok=True)
    for service in ('x620-usb-gadget.service', 'mt6797-x620-ssh-hostkeys.service', 'ssh.service', 'x620-zram.service', 'lightdm.service', 'NetworkManager.service', 'wpa_supplicant.service', 'mt6797-wifi.service', 'systemd-timesyncd.service'):
        subprocess.run(['systemctl', '--root', str(root), 'enable', service], check=True)
    subprocess.run(['systemctl', '--root', str(root), 'set-default', 'graphical.target'], check=True)
    for service in ('ksmbd.service', 'wsdd2.service'):
        if (root / 'usr/lib/systemd/system' / service).exists():
            subprocess.run(['systemctl', '--root', str(root), 'disable', service], check=True)
    packages = subprocess.check_output(['chroot', str(root), '/usr/bin/dpkg-query', '-W', '-f=${Package}\t${Version}\t${Architecture}\n'], text=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps({'distro':'Debian 13 trixie', 'architecture':'arm64', 'filesystem':'f2fs', 'UUID':uuid, 'desktop':'MATE', 'display_manager':'LightDM', 'autologin_user':'dysorder', 'USB_console':'ttyGS0 root autologin',
        'interaction':{'screen_keyboard':'Onboard Compact, docked, auto-show', 'font_DPI':192, 'accessibility':True},
        'graphics':'native MediaTek KMS; Xorg modesetting without glamor; Marco composition off', 'phone_target':'userdata /dev/mmcblk0p38', 'hardware_status':'awaiting owner rootfs boot test', 'packages':packages.splitlines()}, indent=2)+'\n')
    os.sync()


if __name__ == '__main__':
    main()
