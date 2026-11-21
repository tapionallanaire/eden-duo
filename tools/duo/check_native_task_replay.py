#!/usr/bin/env python3
"""Replay Thor's stale Recents intent on an idle, rooted Android test emulator (no game or saves needed)."""
import argparse
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--package', default='com.tapionallanaire.duetplus.debug')
    args = parser.parse_args()

    def shell(*cmd):
        return subprocess.check_output(['adb', '-s', args.serial, 'shell', *cmd], text=True, encoding='utf-8', timeout=30).strip()

    if (not args.serial.startswith('emulator-') or shell('getprop', 'ro.kernel.qemu') != '1'
            or shell('id', '-u') != '0' or not re.fullmatch(r'[a-zA-Z0-9_.]+\.debug', args.package)):
        raise SystemExit('Use an adb-rooted disposable emulator with a debug build.')
    for player, process, record in [('nxnative', 'peanuts_game', 'last-exit.txt'), ('nxbanana', 'banana_game', 'banana-last-exit.txt')]:
        package = args.package
        component = f'{package}/dev.{player}.android.GameActivity'
        if f'{package}:{process}' in shell('ps', '-A'):
            raise SystemExit('Refusing to interrupt a native player')
        state = f'/data/user/0/{package}/files/{record}'

        def exit_record():
            return shell(f'if [ -f {state} ]; then sha256sum {state}; fi')

        before = exit_record()
        result = shell('am', 'start', '-W', '-f', '0x10100000', '-n', component)
        if 'Complete' not in result or 'Error' in result or 'Warning' in result:
            raise AssertionError(result)
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if component not in shell('dumpsys', 'activity', 'recents') and f'{package}:{process}' not in shell('ps', '-A'):
                break
            time.sleep(.2)
        else:
            raise AssertionError(f'{component}: dead player task or process survived history replay')
        if exit_record() != before:
            raise AssertionError(f'{component}: history replay started a new native session')
        print(f'{component}: stale task discarded without replacing the exit record')


if __name__ == '__main__':
    main()
