#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root TERM=linux LANG=C
export PS1='x620-initrd:\w# '
echo 'X620 diagnostic shell'
echo 'Tools: ip ss ethtool lsusb i2cdetect dtc fdtget strace'
echo '       lsblk blkid fdisk fsck.f2fs bash nano jq wget nc'
echo '       dropbear dbclient mount top hexdump gzip tar'
echo 'Boot logs: /run/boot-dmesg.txt /run/boot.dtb /run/diag/'
echo 'Use reboot -f to restart; storage is not mounted automatically.'
exec /usr/bin/bash --noprofile --norc -i
