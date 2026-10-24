#!/usr/bin/env python3
"""Check concrete Darwin 22 storage dispatch and bind to audited 20G81 functions."""
import argparse, hashlib, json, os, re, struct, subprocess, sys
from pathlib import Path
DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE / 'kits/NativePrelink'))
import native_prelink as np
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--object',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args(); obj_path=args.object; out=args.output
component='PongUfsBlock'; mangled=str(len(component))+component
# Both the genuine input and reference object were checked by the audit.
audit=json.loads((DEVICE/'mac-build/storage-abi/interface-audit.json').read_text())
if audit['method_slots'] != 190 or audit['compiled_public_class_size'] != 0x90:
    raise SystemExit('Missing Darwin 22 storage ABI qualification')
obj = obj_path.read_bytes()
image = np.macho(obj)
offset = next(at for cmd, at, size in image['commands'] if cmd == 2)
sy, count, st, _ = struct.unpack_from('<4I', obj, offset + 8)
symbols = []
for i in range(count):
    ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, sy + i * 16)
    symbols.append((obj[st + ix:obj.index(b'\0', st + ix)].decode(), value, kind))
vtable = next(v for name, v, kind in symbols if name == '__ZTV' + mangled)
relocations = {}
for segment in image['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            ro, rc = struct.unpack_from('<II', obj, section['command_offset'] + 56)
            for i in range(rc):
                address, word = struct.unpack_from('<II', obj, ro + i * 8)
                if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                    relocations[section['va'] + address] = symbols[word & 0xffffff][0]
compiled = [relocations.get(vtable + 16 + i * 8) for i in range(190)]
if not all(compiled) or vtable + 16 + 190 * 8 in relocations or '___cxa_pure_virtual' in compiled:
    raise SystemExit('Concrete block class has missing/additional/pure virtual methods')
all_names = list(dict.fromkeys(compiled + [r['reference_name'] for r in audit['methods']]))
# Apple's c++filt strips the Mach-O underscore itself.
demangled = subprocess.check_output(['xcrun', 'c++filt', *all_names], text=True).splitlines()
readable = dict(zip(all_names, demangled))
def signature(name):
    value = readable[name].split('::', 1)
    if len(value) != 2:
        raise SystemExit('Undecoded method: ' + name)
    return value[1]
overrides = 0
for row, actual in zip(audit['methods'], compiled):
    expected = row['reference_name']
    own = actual.startswith(('__ZN' + mangled, '__ZNK' + mangled))
    if expected == '___cxa_pure_virtual':
        if not own:
            raise SystemExit('A required transport operation is not implemented')
    elif actual != expected:
        if not own or (row['slot'] not in (0, 1) and signature(actual) != signature(expected)):
            raise SystemExit('Concrete subclass dispatch mismatch at slot ' + str(row['slot']))
    overrides += own

possible={k:int(v,16) for k,v in audit['bindings'].items()}
undefined = [name for name,value,kind in symbols if kind == 1 and 'IOBlockStorageDevice' in name]
if any(name not in possible for name in undefined):
    raise SystemExit('Unqualified storage import: '+str([n for n in undefined if n not in possible]))
if any(re.search(r'IOBlockStorageDeviceD[01]Ev$',n) for n in undefined):
    raise SystemExit('Do not call abstract complete/deleting destructor traps')
bindings={name:possible[name] for name in undefined}
assembly=out/'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n' for name,value in sorted(bindings.items())))
subprocess.run(['xcrun','--sdk','iphoneos','clang','-target','arm64-apple-ios16.0','-Werror','-c',str(assembly),'-o',str(out/'original-bindings.o')],check=True)
(out/'bindings.json').write_text(json.dumps(dict(fixed_prelink_base=audit['fixed_prelink_base'],
    original_candidate_sha256=audit['candidate_sha256'],
    compiled_object_sha256=hashlib.sha256(obj).hexdigest(),virtual_methods_checked=190,
    concrete_overrides=overrides,bindings={name:hex(value) for name,value in bindings.items()},actual_io_qualified=False),indent=2)+'\n')
print(f'PASS: 190 storage slots, {overrides} concrete overrides; {len(bindings)} original imports')
