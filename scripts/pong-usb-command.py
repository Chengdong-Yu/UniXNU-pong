#!/usr/bin/env python3
"""Send an explicit Pong CDC command (never an automatic reboot timer)."""
import argparse
import fcntl
import glob
import os
import select
import struct
import termios
import time
from pong_usb_ports import serial_ports

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('command',choices=['rec','fastboot','help'])
p.add_argument('--port')
a=p.parse_args()
ports=[a.port] if a.port else serial_ports()
if len(ports)!=1:p.error('expected one Pong CDC port; use --port')
fd=os.open(ports[0],os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
try:
    attributes=termios.tcgetattr(fd)
    attributes[0]=attributes[1]=attributes[3]=0
    attributes[2]=termios.CLOCAL|termios.CREAD|termios.CS8
    attributes[4]=attributes[5]=termios.B115200
    attributes[6][termios.VMIN]=attributes[6][termios.VTIME]=0
    termios.tcsetattr(fd,termios.TCSANOW,attributes)
    fcntl.ioctl(fd,termios.TIOCMBIS,struct.pack('I',termios.TIOCM_DTR))
    payload=('help\n' if a.command=='help' else 'reboot '+a.command+'\n').encode('ascii')
    written=0;deadline=time.monotonic()+3
    while written<len(payload):
        if time.monotonic()>deadline:raise TimeoutError('CDC command write timeout')
        _,ready,_=select.select([], [fd], [], .1)
        if ready:written+=os.write(fd,payload[written:])
    print('sent '+payload.decode().strip())
    deadline=time.monotonic()+3
    while time.monotonic()<deadline:
        ready,_,_=select.select([fd],[],[],.2)
        if ready:
            try:block=os.read(fd,16384)
            except OSError:break
            if block:print(block.decode('utf-8',errors='replace'),end='')
finally:os.close(fd)
