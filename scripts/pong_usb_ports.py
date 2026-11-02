"""Locate Pong's retained CDC interface even when USB uses the iOS UDID."""
import glob
import plistlib
import subprocess

def _nodes(value):
    if isinstance(value,dict):
        yield value
        for child in value.get('IORegistryEntryChildren',[]):yield from _nodes(child)
    elif isinstance(value,list):
        for child in value:yield from _nodes(child)

def phone_usb_nodes():
    try:
        root=plistlib.loads(subprocess.check_output(
            ['/usr/sbin/ioreg','-a','-r','-c','IOUSBHostDevice','-l','-w','0'],timeout=5))
    except (OSError,ValueError,subprocess.SubprocessError,plistlib.InvalidFileException):return []
    found=[];seen=set()
    for node in _nodes(root):
        # Interface objects inherit VID/PID too; select the physical device
        # once rather than counting its CDC child as another phone.
        if 'bDeviceClass' not in node or node.get('idVendor')!=0x05ac or node.get('idProduct')!=0x12a8:continue
        key=(node.get('IORegistryEntryID'),node.get('locationID'),node.get('USB Serial Number'))
        if key in seen:continue
        if any(any(isinstance(v,str) and 'Pong XNU console' in v for v in item.values()) or
               (item.get('bInterfaceClass')==2 and item.get('bInterfaceSubClass')==2)
               for item in _nodes(node)):
            found.append(node);seen.add(key)
    return found

def serial_ports():
    ports=glob.glob('/dev/cu.usbmodemPONG_SM8475*')
    for node in phone_usb_nodes():
        for item in _nodes(node):
            port=item.get('IOCalloutDevice')
            if isinstance(port,str) and port.startswith('/dev/cu.usbmodem'):ports.append(port)
    return sorted(set(ports))

def native_serial():
    nodes=phone_usb_nodes()
    if len(nodes)==1:return nodes[0].get('USB Serial Number') or nodes[0].get('kUSBSerialNumberString')
    return None
