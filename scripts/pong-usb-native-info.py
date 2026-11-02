#!/usr/bin/env python3
"""Read the real native lockdownd identity through the computer's usbmuxd."""
import argparse
import json
import plistlib
import socket
import struct
from pong_usb_ports import native_serial

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial',help='defaults to the iOS identity on Pong CDC/Mux USB')
p.add_argument('--socket',default='/var/run/usbmuxd')
p.add_argument('--identity',action='store_true',help='read the actual native device identity keys')
a=p.parse_args()
a.serial=a.serial or native_serial() or 'PONG-SM8475'

def receive(s,n):
    data=bytearray()
    while len(data)<n:
        block=s.recv(n-len(data))
        if not block:raise ConnectionError('native USB connection closed')
        data.extend(block)
    return bytes(data)

def mux(s,request):
    body=plistlib.dumps(dict(request,ProgName='Pong native USB info',ClientVersionString='Pong-1.0'))
    s.sendall(struct.pack('<IIII',16+len(body),1,8,1)+body)
    size,version,kind,tag=struct.unpack('<IIII',receive(s,16))
    if not 16<=size<=1048576 or version!=1 or kind!=8 or tag!=1:
        raise ValueError('invalid host usbmuxd response')
    return plistlib.loads(receive(s,size-16))

def lockdown(s,request):
    body=plistlib.dumps(dict(request,Label='Pong native USB info'))
    s.sendall(struct.pack('>I',len(body))+body)
    length=struct.unpack('>I',receive(s,4))[0]
    if not 0<length<=1048576:raise ValueError('native lockdownd response too large')
    return plistlib.loads(receive(s,length))

with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
    s.settimeout(5);s.connect(a.socket)
    devices=mux(s,{'MessageType':'ListDevices'}).get('DeviceList',[])
matches=[d for d in devices if d.get('Properties',{}).get('SerialNumber','').lower()==a.serial.lower()
         and d['Properties'].get('ConnectionType')=='USB']
if len(matches)!=1:
    raise SystemExit('Pong native USB is not registered with host usbmuxd: '+json.dumps(devices,default=str))
device=matches[0]
with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
    s.settimeout(5);s.connect(a.socket)
    result=mux(s,{'MessageType':'Connect','DeviceID':device['DeviceID'],'PortNumber':socket.htons(62078)})
    if result.get('Number')!=0:raise ConnectionError('native lockdownd connect: '+str(result))
    answer=lockdown(s,{'Request':'QueryType'})
    identity={}
    if a.identity:
        for key in ('ProductType','DeviceClass','ProductVersion','BuildVersion','ModelNumber','SerialNumber','UniqueDeviceID'):
            reply=lockdown(s,{'Request':'GetValue','Key':key})
            identity[key]=reply.get('Value',{'native_error':reply.get('Error','no value')})
result={'physical_usb':device,'native_lockdownd':answer}
if a.identity:
    result['native_identity']=identity
    uid=identity.get('UniqueDeviceID')
    if isinstance(uid,str):result['usb_serial_matches_native_uid']=uid.lower()==a.serial.lower()
print(json.dumps(result,indent=2,default=str))
