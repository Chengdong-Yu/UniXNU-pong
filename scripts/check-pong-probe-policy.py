#!/usr/bin/env python3
"""Exercise the actual bounded journal on the host without booting or panicking."""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

DEVICE = Path(__file__).resolve().parents[1]
SOURCE = Path('/Volumes/PongDev/build/Pong-iOS16-native/tree/.cache/xnu-work/osfmk/arm64/pong_early_trace.h')


def main():
    source = SOURCE.read_text()
    journal = source[source.index('/* Optional probes only:'):source.index('void pong_early_panic_text(const char *, uint64_t);')]
    probe = (DEVICE / 'drivers/NativeUfs/NativeUfs.cpp').read_text()
    if re.search(r'\bpanic\s*\(', probe) or 'native-ufs-stop' in probe:
        raise SystemExit('Optional UFS probe still has an intentional stop')
    atomic = source[source.index('pong_early_counter_inc(uint64_t *address)'):source.index('void pong_early_enable_normal_vectors(void);')]
    if 'wfe' in atomic or 'for (;;)' in atomic:
        raise SystemExit('Atomic diagnostic still intentionally stops boot')
    counter = (SOURCE.parents[2] / 'bsd/kern/counter_test.c').read_text()
    if 'pong-atomic-probe' not in counter or 'counter_inc(&test_static_scalable_counter)' not in counter:
        raise SystemExit('Atomic diagnostic is not opt-in with the original normal increment')
    transport = (DEVICE / 'drivers/NativeUfs/PongUfsTransport.cpp').read_text()
    commands = (DEVICE / 'drivers/NativeUfs/RootCommands.h').read_text()
    if re.search(r'\bpanic\s*\(', transport) or 'hostWrite4k' in commands:
        raise SystemExit('Read-only transport has a diagnostic panic or persistent-write helper')
    harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static char captured[8192];
static unsigned used;
static int pong_early_console_write(const char *text, int length) {
    assert(length >= 0 && used + (unsigned)length < sizeof(captured));
    memcpy(captured + used, text, length);
    used += length; captured[used] = 0;
    return 1;
}
''' + journal + r'''
int main(void) {
    pong_probe_record(0, "ufs-registers", 1, "read complete", 0x300);
    pong_probe_record(1, "failure-case", 2, "mapping refused; skipped host", 7);
    pong_probe_record(2, "root-media", 3, "backend absent", 0);
    pong_probe_record(3, "pending-case", 4, "observer pending", 0);
    pong_probe_record(99, "bad-slot", 2, "must not overflow", 0);
    assert(strstr(captured, "ufs-registers PASS"));
    assert(strstr(captured, "failure-case FAIL"));
    assert(!strstr(captured, "bad-slot"));
    unsigned snapshot_at = used;
    pong_probe_dump_before_panic();
    pong_early_console_write("PONG PANIC: host-only marker\n", 29);
    const char *snapshot = captured + snapshot_at;
    const char *heading = strstr(snapshot, "PONG PROBE SNAPSHOTS BEFORE PANIC");
    const char *panic_marker = strstr(snapshot, "PONG PANIC:");
    assert(heading && panic_marker && heading < panic_marker);
    assert(strstr(snapshot, "ufs-registers PASS detail=300"));
    assert(strstr(snapshot, "failure-case FAIL detail=7"));
    assert(strstr(snapshot, "root-media SKIP"));
    assert(strstr(snapshot, "pending-case RUNNING"));
    puts("PASS: success/failure return, bounded slots, and snapshots before panic marker");
    return 0;
}
'''
    env = dict(os.environ, DEVELOPER_DIR='/Volumes/PongDev/Applications/Xcode.app/Contents/Developer')
    with tempfile.TemporaryDirectory(prefix='pong-probe-policy-') as scratch:
        path = Path(scratch)
        (path / 'journal.c').write_text(harness)
        subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(path / 'journal.c'), '-o', str(path / 'journal')], check=True, env=env)
        result = subprocess.run([str(path / 'journal')], check=True, capture_output=True, text=True)
    report = {'host_journal_check': 'passed', 'source': str(SOURCE),
              'optional_ufs_has_panic': False, 'hardware_panics_triggered': False,
              'output': result.stdout.strip()}
    (DEVICE.parent / 'evidence/pong-probe-policy-host-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
