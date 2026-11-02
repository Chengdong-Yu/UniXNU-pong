#!/usr/bin/env python3
"""Save bounded, read-only Pong CDC console output without rebooting the phone."""
import argparse
import fcntl
import glob
import os
from pathlib import Path
import select
import struct
import termios
import time
from pong_usb_ports import serial_ports

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', help='Pong CDC serial path; automatically selected when unambiguous')
p.add_argument('--seconds', type=float, default=15)
p.add_argument('--max-bytes', type=int, default=1048576)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
if not 0 < a.seconds <= 60 or not 1 <= a.max_bytes <= 16 * 1048576:
    p.error('use 0..60 seconds and 1..16777216 bytes')
ports = [a.port] if a.port else serial_ports()
if len(ports) != 1:
    p.error('expected one Pong CDC port; use --port for an explicit device')
if a.output.exists():
    p.error('preserving existing output; choose a new path')
a.output.parent.mkdir(parents=True, exist_ok=True)
fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    attributes = termios.tcgetattr(fd)
    attributes[0] = attributes[1] = attributes[3] = 0
    attributes[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attributes[4] = attributes[5] = termios.B115200
    attributes[6][termios.VMIN] = attributes[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attributes)
    # Standard CDC host-open handshake; never transmit a command/payload.
    fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('I', termios.TIOCM_DTR))
    deadline = time.monotonic() + a.seconds
    data = bytearray()
    while time.monotonic() < deadline and len(data) < a.max_bytes:
        ready, _, _ = select.select([fd], [], [], min(1, max(0, deadline - time.monotonic())))
        if ready:
            try:
                block = os.read(fd, min(16384, a.max_bytes - len(data)))
            except BlockingIOError:
                continue
            if block:
                data.extend(block)
            else:
                time.sleep(0.01)
    with a.output.open('xb') as output:
        output.write(data)
finally:
    os.close(fd)
print(f'{len(data)} live USB console bytes saved to {a.output}')
