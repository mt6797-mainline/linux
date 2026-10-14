#!/bin/busybox sh
# Sourced by PID 1, so successful handoff execs switch_root rather than a child.
x620_rootfs_boot()
{
    root_uuid=f40f7c77-a817-4b13-97c4-24281d26d4e1
    rescue=0
    for arg in $(cat /proc/cmdline); do
        case "$arg" in
            x620.rescue=1) rescue=1 ;;
            x620.rootfs=UUID=*) root_uuid=${arg#x620.rootfs=UUID=} ;;
        esac
    done
    test "$rescue" = 0 || return 0
    root_device=
    for attempt in $(seq 1 30); do
        root_device=$(blkid -U "$root_uuid" 2>/dev/null) || root_device=
        test -n "$root_device" && break
        sleep 0.1
    done
    if test -z "$root_device"; then
        echo 'X620: Debian rootfs UUID absent; starting diagnostic initrd' > /dev/console
        return 0
    fi
    mkdir -p /newroot
    fs_bytes=$(blkid -p -s FSSIZE -o value "$root_device" 2>/dev/null) || fs_bytes=
    device_bytes=$(blockdev --getsize64 "$root_device" 2>/dev/null) || device_bytes=
    if test -n "$fs_bytes" && test -n "$device_bytes" && test "$device_bytes" -gt "$fs_bytes"; then
        echo "X620: growing F2FS to $device_bytes bytes" > /dev/console
        if ! fsck.f2fs -f "$root_device" || ! resize.f2fs "$root_device"; then
            echo 'X620: F2FS growth failed; starting diagnostic initrd' > /dev/console
            return 0
        fi
    fi
    if ! mount -t f2fs -o rw,noatime "$root_device" /newroot; then
        echo 'X620: rootfs mount failed; starting diagnostic initrd' > /dev/console
        return 0
    fi
    if ! test -f /newroot/etc/x620-rootfs-release || ! test -x /newroot/lib/systemd/systemd; then
        umount /newroot
        echo 'X620: rootfs identity/init check failed; starting diagnostic initrd' > /dev/console
        return 0
    fi
    echo "X620_ROOTFS_READY: $root_device" > /dev/console
    mkdir -p /newroot/dev /newroot/proc /newroot/sys /newroot/run
    moved=
    # Keep /proc available until the last move. util-linux mount additionally
    # updates userspace mount state; BusyBox is sufficient for this PID 1 handoff.
    for point in dev sys run proc; do
        if ! /bin/busybox mount --move "/$point" "/newroot/$point"; then
            for restore in proc run sys dev; do
                case " $moved " in
                    *" $restore "*) /bin/busybox mount --move "/newroot/$restore" "/$restore" ;;
                esac
            done
            echo "X620: could not move /$point; starting diagnostic initrd" > /dev/console
            umount /newroot
            return 0
        fi
        moved="$point $moved"
    done
    exec /bin/busybox switch_root /newroot /sbin/init
}
x620_rootfs_boot
