#!/usr/bin/env bash
# Build only inside a distro-specific F2FS image mounted at rootfs/.
set -euo pipefail
WORKSPACE=${MT6797_WORKSPACE:-${X620_WORKSPACE:-$(cd "$(dirname "$0")/../../.." && pwd)}}
task_image="$WORKSPACE/dist/debian-arm64-rootfs.img"
task_mount="$WORKSPACE/rootfs"
task_seed="$WORKSPACE/artifacts/rootfs/debian-arm64.img"
task_tools="$WORKSPACE/linux-6.12/tools/mt6797"
mkdir -p "$WORKSPACE/dist" "$WORKSPACE/artifacts" "$task_mount"
if mountpoint -q "$task_mount"; then
    echo 'rootfs/ is already mounted; finish that operation first' >&2
    exit 1
fi
if test -n "$(ls -A "$task_mount")"; then
    echo 'rootfs/ must be empty before mounting the image' >&2
    exit 1
fi
if ! test -f "$task_image"; then
    if test -f "$task_seed"; then
        cp --reflink=auto --sparse=always "$task_seed" "$task_image"
    else
        truncate -s 6G "$task_image"
        sudo mkfs.f2fs -f -U f40f7c77-a817-4b13-97c4-24281d26d4e1 -l X620DEBIAN "$task_image"
    fi
fi
if test "$(stat -c %s "$task_image")" -lt 6442450944; then
    truncate -s 6G "$task_image"
    sudo resize.f2fs "$task_image"
fi
sudo mount -t f2fs -o loop "$task_image" "$task_mount"
trap 'sudo umount "$task_mount"' EXIT
if ! test -f "$task_mount/etc/debian_version"; then
    sudo debootstrap --foreign --arch=arm64 --variant=minbase \
        --include=systemd-sysv,udev,busybox-static,ca-certificates,f2fs-tools \
        trixie "$task_mount" https://mirrors.tuna.tsinghua.edu.cn/debian
    printf '#!/bin/sh\nexit 101\n' | sudo tee "$task_mount/usr/sbin/policy-rc.d" >/dev/null
    sudo chmod 755 "$task_mount/usr/sbin/policy-rc.d"
    sudo chroot "$task_mount" /debootstrap/debootstrap --second-stage
fi
printf '#!/bin/sh\nexit 101\n' | sudo tee "$task_mount/usr/sbin/policy-rc.d" >/dev/null
sudo chmod 755 "$task_mount/usr/sbin/policy-rc.d"
printf 'deb https://mirrors.tuna.tsinghua.edu.cn/debian trixie main\ndeb https://mirrors.tuna.tsinghua.edu.cn/debian trixie-updates main\ndeb https://mirrors.tuna.tsinghua.edu.cn/debian-security trixie-security main\n' \
    | sudo tee "$task_mount/etc/apt/sources.list" >/dev/null
sudo install -D -m 644 "$task_tools/rootfs-packages.txt" \
    "$task_mount/usr/share/mt6797-x620/rootfs-packages.txt"
sudo unshare --mount --propagation private bash -s -- "$task_mount" <<'CHROOT'
set -euo pipefail
task_root=$1
mount -t proc proc "$task_root/proc"
mount -t sysfs sysfs "$task_root/sys"
mount --rbind /dev "$task_root/dev"
chroot "$task_root" /usr/bin/env DEBIAN_FRONTEND=noninteractive apt-get update
mapfile -t task_packages < "$task_root/usr/share/mt6797-x620/rootfs-packages.txt"
chroot "$task_root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
    apt-get install -y "${task_packages[@]}"
chroot "$task_root" /usr/bin/env DEBIAN_FRONTEND=noninteractive apt-get upgrade -y
CHROOT
aarch64-linux-gnu-gcc --sysroot="$task_mount" \
    -isystem "$task_mount/usr/include/aarch64-linux-gnu" \
    -B"$task_mount/usr/lib/aarch64-linux-gnu/" -L"$task_mount/usr/lib/aarch64-linux-gnu" \
    -O2 -Wall -Wextra -Werror -fno-link-libatomic -static \
    -o "$WORKSPACE/out/mt6797-wmt" "$task_tools/mt6797-wmt.c"
sudo install -m 755 "$WORKSPACE/out/mt6797-wmt" "$task_mount/usr/sbin/mt6797-wmt"
sudo python3 "$task_tools/prepare-rootfs.py" --root "$task_mount" \
    --manifest "$WORKSPACE/artifacts/rootfs-mate-manifest.json"
if ! test -f "$WORKSPACE/out/kernel/modules.order"; then
    echo 'Build the production Image and modules in out/kernel before packaging rootfs' >&2
    exit 1
fi
sudo make -C "$WORKSPACE/linux-6.12" O="$WORKSPACE/out/kernel" \
    ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH="$task_mount" \
    INSTALL_MOD_STRIP=1 modules_install
sudo chroot "$task_mount" /usr/bin/dpkg --audit
sudo chroot "$task_mount" /usr/bin/systemd-analyze verify --man=no \
    x620-usb-gadget.service mt6797-x620-ssh-hostkeys.service x620-zram.service \
    lightdm.service NetworkManager.service wpa_supplicant.service mt6797-wifi.service
sudo python3 "$task_tools/package-rootfs-update.py" --root "$task_mount" \
    --output "$WORKSPACE/dist/mt6797-x620-rootfs-update.tar"
sudo python3 "$task_tools/audit-rootfs.py" --root "$task_mount" \
    --output "$WORKSPACE/artifacts/mt6797-rootfs-native-audit.json"
sudo chroot "$task_mount" /usr/bin/apt-get clean
sudo sync -f "$task_mount"
sudo umount "$task_mount"
trap - EXIT
sudo fsck.f2fs -f "$task_image"
task_sparse="$WORKSPACE/out/mt6797-x620-sparse"
mkdir -p "$task_sparse"
(cd "$task_sparse" && sudo dump.f2fs -s '0~-1' "$task_image" > dump.log 2>&1)
python3 "$task_tools/make-f2fs-sparse.py" "$task_image" "$task_sparse/dump_sit" \
    "$WORKSPACE/dist/mt6797-x620-debian-mate.simg.building" \
    --dirty-check "$task_sparse/check.img"
sudo fsck.f2fs --dry-run -f "$task_sparse/check.img"
mv "$WORKSPACE/dist/mt6797-x620-debian-mate.simg.building" \
    "$WORKSPACE/dist/mt6797-x620-debian-mate.simg"
echo "Rootfs ready: $WORKSPACE/dist/mt6797-x620-debian-mate.simg"
