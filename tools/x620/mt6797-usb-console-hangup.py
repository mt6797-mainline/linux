#!/usr/bin/python3
"""Hang up the USB console before systemd stops its login process."""
import errno
import fcntl
import os

# Linux asm-generic/ioctls.h: TIOCVHANGUP. This also wakes blocked tty ioctls.
TIOCVHANGUP = 0x5437

try:
    fd = os.open('/dev/ttyGS0', os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
except OSError as error:
    if error.errno not in (errno.ENOENT, errno.ENODEV, errno.ENXIO):
        raise
else:
    try:
        fcntl.ioctl(fd, TIOCVHANGUP)
    finally:
        os.close(fd)
