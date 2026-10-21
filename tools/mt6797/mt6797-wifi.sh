#!/bin/sh
set -eu
firmware=/lib/firmware/mediatek/mt6797
state=/run/mt6797-wifi-state
mkdir -p "$state"

calibration()
{
    if test -r "$firmware/WIFI" && test "$(stat -c %s "$firmware/WIFI")" -ge 512; then
        echo 'MT6797 Wi-Fi calibration present'
        return 0
    fi
    mkdir -p "$state/nvdata" "$firmware"
    for partition in /dev/mmcblk0p*; do
        test -b "$partition" || continue
        name=$(blkid -p -s PART_ENTRY_NAME -o value "$partition" 2>/dev/null || true)
        case "$name" in nvdata|protect1|protect2|protect_f|protect_s) ;; *) continue ;; esac
        fs=$(blkid -p -s TYPE -o value "$partition" 2>/dev/null || true)
        test "$fs" = ext4 || continue
        if ! mount -t ext4 -o ro,noload "$partition" "$state/nvdata"; then continue; fi
        found=0
        for directory in "$state/nvdata/APCFG/APRDEB" "$state/nvdata/nvram/APCFG/APRDEB"; do
            if test -r "$directory/WIFI" && test "$(stat -c %s "$directory/WIFI")" -ge 512; then
                cp "$directory/WIFI" "$firmware/WIFI"
                chmod 600 "$firmware/WIFI"
                if test -r "$directory/WIFI_CUSTOM"; then
                    cp "$directory/WIFI_CUSTOM" "$firmware/WIFI_CUSTOM"
                    chmod 600 "$firmware/WIFI_CUSTOM"
                fi
                echo "MT6797 calibration recovered from $name ($partition), mounted read-only"
                found=1
                break
            fi
        done
        umount "$state/nvdata"
        test "$found" = 0 || return 0
    done
    echo 'Original WIFI calibration not found in the preserved filesystems.' >&2
    echo 'No factory partition was written. Supply the original 512-byte WIFI record to /lib/firmware/mediatek/mt6797/WIFI.' >&2
    return 1
}

case "${1:-status}" in
    calibration) calibration ;;
    start)
        calibration
        if ! test -d /sys/bus/platform/drivers/mt-wifi; then
            modprobe mt6797_wlan
        fi
        launcher=/usr/sbin/mt6797-wmt
        "$launcher" --inspect "$firmware"
        if test -f "$state/launcher.pid" && kill -0 "$(cat "$state/launcher.pid")" 2>/dev/null; then
            echo 'WMT launcher already running'
        else
            "$launcher" --serve "$firmware" > "$state/launcher.log" 2>&1 &
            echo "$!" > "$state/launcher.pid"
            ready=0
            for attempt in $(seq 1 50); do
                if grep -q '^MT6797_WMT_LAUNCHER_READY' "$state/launcher.log"; then ready=1; break; fi
                kill -0 "$(cat "$state/launcher.pid")" 2>/dev/null || break
                sleep 0.1
            done
            test "$ready" = 1 || { cat "$state/launcher.log" >&2; exit 1; }
        fi
        "$launcher" --on
        ip link show wlan0
        echo 'MT6797 WLAN function on'
        ;;
    stop)
        launcher=/usr/sbin/mt6797-wmt
        "$launcher" --off
        if test -f "$state/launcher.pid"; then
            kill "$(cat "$state/launcher.pid")" 2>/dev/null || true
        fi
        ;;
    status)
        ip -br link
        test ! -d /sys/class/ieee80211 || ls /sys/class/ieee80211
        test ! -f "$state/launcher.log" || tail -n 30 "$state/launcher.log"
        dmesg | grep -Ei 'mt6797|mt6351|wlan|wmt|btif|cfg80211|firmware' | tail -n 60
        ;;
    *) echo 'usage: mt6797-wifi start|stop|status|calibration' >&2; exit 2 ;;
esac
