#!/bin/busybox sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export TERM=linux
export HOME=/root
export LANG=C
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /run /tmp /sys/kernel/debug /sys/fs/pstore /sys/kernel/config
mount -t devpts -o mode=620,gid=5 devpts /dev/pts
mount -t tmpfs -o mode=755 tmpfs /run
mount -t tmpfs -o mode=1777 tmpfs /tmp
mount -t debugfs debugfs /sys/kernel/debug
mount -t pstore pstore /sys/fs/pstore
mount -t configfs configfs /sys/kernel/config
hostname x620-initrd
ip link set lo up
printf '<6>X620_INITRD_READY: diagnostic userspace entered\n' > /dev/kmsg
if test -c /dev/pmsg0; then
    printf 'X620_INITRD_READY: %s\n' "$(uname -r)" > /dev/pmsg0 2>/dev/null || true
fi
dmesg > /run/boot-dmesg.txt
cat /proc/cmdline > /run/boot-cmdline.txt
if test -f /sys/firmware/fdt; then
    cat /sys/firmware/fdt > /run/boot.dtb
    dtc -I dtb -O dts /run/boot.dtb > /run/boot.dts 2>/run/dtc.log || true
fi
mkdir -p /run/diag
cat /proc/meminfo > /run/diag/meminfo
cat /proc/interrupts > /run/diag/interrupts
cat /proc/iomem > /run/diag/iomem
/usr/bin/ip -br link > /run/diag/links
ls -l /sys/class/udc > /run/diag/udc 2>/dev/null || true
/usr/bin/x620-usb > /run/diag/usb-serial.log 2>&1 &
if test -r /sys/class/graphics/fb0/name; then
    fb_name=$(cat /sys/class/graphics/fb0/name)
    fb_size=$(cat /sys/class/graphics/fb0/virtual_size)
    fb_bpp=$(cat /sys/class/graphics/fb0/bits_per_pixel)
    fb_stride=$(cat /sys/class/graphics/fb0/stride)
    printf 'fb0: %s %s %sbpp stride=%s\n' "$fb_name" "$fb_size" "$fb_bpp" "$fb_stride" > /run/diag/fb-info
    printf '<6>X620_FB_READY: %s\n' "$(cat /run/diag/fb-info)" > /dev/kmsg
fi
echo 'X620_INITRD_READY: diagnostic shell and tools available' > /dev/console
if test -c /dev/tty1; then
    # BusyBox openvt has no -f option and VT1 is already in use by fbcon.
    # Open the existing VTs directly rather than allocating a fresh VT.
    setsid sh -c 'exec /usr/bin/x620-dashboard < /dev/tty1 > /dev/tty1 2>&1' &
    if test -c /dev/tty2; then
        setsid sh -c 'exec /usr/bin/x620-shell < /dev/tty2 > /dev/tty2 2>&1' &
    fi
    chvt 1 2>/run/diag/vt-switch.log || true
fi
for port in /dev/ttyS0 /dev/ttyS3 /dev/ttyAMA0; do
    if test -c "$port"; then
        setsid sh -c 'exec /usr/bin/x620-shell < "$1" > "$1" 2>&1' sh "$port" &
    fi
done
while true; do
    wait
    sleep 5
done
