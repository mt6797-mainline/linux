#!/bin/sh
export TERM=linux
frame=0
while true; do
    frame=$((frame + 1))
    printf '\033[2J\033[H'
    printf '\033[1;36mLINUX 6.12 / SIMPLEFB\033[0m\n'
    echo 'LeEco X620 -- diagnostic initrd'
    echo '=============================================='
    echo 'X620_INITRD_READY -- PID 1 has entered userspace'
    uname -r
    printf 'Frame: %s  Uptime: ' "$frame"; cut -d ' ' -f 1 /proc/uptime
    printf 'Online CPUs: '; cat /sys/devices/system/cpu/online
    awk '/MemTotal|MemAvailable/ {print}' /proc/meminfo
    if test -r /run/diag/fb-info; then
        cat /run/diag/fb-info
    else
        echo 'fb0: not registered'
    fi
    printf 'Foreground VT: '; cat /sys/class/tty/tty0/active 2>/dev/null
    echo
    echo 'Network:'
    /usr/bin/ip -br link
    echo 'USB gadget controllers:'
    ls /sys/class/udc 2>/dev/null
    test ! -r /run/diag/usb-serial-status || cat /run/diag/usb-serial-status
    if test -r /run/diag/usb-udc-name; then
        udc=$(cat /run/diag/usb-udc-name)
        printf 'USB state: '; cat "/sys/class/udc/$udc/state"
    fi
    echo
    echo 'Recent kernel messages:'
    dmesg | tail -n 10
    echo
    echo 'Logs: /run/boot-dmesg.txt /run/boot.dtb /run/diag/'
    echo 'Console: VT1 status / VT2 shell; serial shells enabled'
    echo 'No internal partitions are mounted automatically.'
    if test "$frame" -le 3; then
        printf '<6>X620_DASHBOARD_FRAME: %s\n' "$frame" > /dev/kmsg
    fi
    sleep 2
done
