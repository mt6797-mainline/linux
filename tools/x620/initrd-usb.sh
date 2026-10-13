#!/bin/busybox sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
status=/run/diag/usb-serial-status
log=/run/diag/usb-serial.log
mkdir -p /run/diag
exec 2>>"$log"

fail()
{
    echo "FAILED: $*" > "$status"
    printf '<3>X620_USB_SERIAL_FAILED: %s\n' "$*" > /dev/kmsg
    exit 1
}

echo 'Waiting for USB device controller' > "$status"
count=0
udc=
while test "$count" -lt 20; do
    for path in /sys/class/udc/*; do
        test -e "$path" || continue
        udc=${path##*/}
        break
    done
    test -n "$udc" && break
    count=$((count + 1))
    sleep 1
done
test -n "$udc" || fail 'no UDC; inspect MTU3/PHY probe messages'

g=/sys/kernel/config/usb_gadget/x620
mkdir -p "$g" || fail 'cannot create configfs gadget'
echo 0x1d6b > "$g/idVendor" || fail 'idVendor'
echo 0x0104 > "$g/idProduct" || fail 'idProduct'
echo 0x0200 > "$g/bcdUSB"
echo 0x0100 > "$g/bcdDevice"
echo 0xef > "$g/bDeviceClass"
echo 0x02 > "$g/bDeviceSubClass"
echo 0x01 > "$g/bDeviceProtocol"
mkdir -p "$g/strings/0x409" "$g/configs/c.1/strings/0x409" || fail 'gadget strings'
echo X620-LINUX-ACM > "$g/strings/0x409/serialnumber"
echo 'LeEco X620 Linux' > "$g/strings/0x409/manufacturer"
echo 'X620 Linux USB serial' > "$g/strings/0x409/product"
echo 'CDC ACM diagnostic shell' > "$g/configs/c.1/strings/0x409/configuration"
echo 250 > "$g/configs/c.1/MaxPower"
mkdir "$g/functions/acm.usb0" || fail 'CDC ACM function unavailable'
ln -s "$g/functions/acm.usb0" "$g/configs/c.1/acm.usb0" || fail 'ACM configuration link'
echo "$udc" > "$g/UDC" || fail "cannot bind to $udc"

test -c /dev/ttyGS0 || fail 'ttyGS0 missing after gadget bind'
echo "Ready: CDC ACM /dev/ttyGS0 ($udc)" > "$status"
printf '%s\n' "$udc" > /run/diag/usb-udc-name
printf '<6>X620_USB_SERIAL_READY: %s ttyGS0\n' "$udc" > /dev/kmsg
# Opening the TTY can wait for the host; keep setup/status independent of it.
# Reopen after disconnect so a newly connected terminal gets a fresh shell.
while true; do
    setsid sh -c 'exec /usr/bin/x620-shell < /dev/ttyGS0 > /dev/ttyGS0 2>&1'
    sleep 1
done
