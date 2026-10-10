#!/usr/bin/env python3
"""Register this installation for the current desktop user; preserve global hotkeys."""
import argparse
import json
import os
from pathlib import Path
import subprocess

PREFIX = Path('/opt/sogou-ng')
HOME = Path.home()
STATE = HOME / '.local/state/sogo-wayland-fcitx5/registration.json'


def plan():
    data = HOME / '.local/share'
    links = {
        data / 'fcitx5/addon/sogou-native.conf': PREFIX / 'integration/sogou-native-addon.conf',
        data / 'fcitx5/inputmethod/sogou-native.conf': PREFIX / 'integration/sogou-native-inputmethod.conf',
        HOME / '.config/systemd/user/sogou-ng-engine.service': PREFIX / 'integration/sogou-ng-engine.service',
    }
    icons = PREFIX / 'root/usr/share/icons/hicolor'
    for p in icons.glob('*/apps/fcitx-sogou-ime-ng.png'):
        links[data / 'icons/hicolor' / p.relative_to(icons)] = p
    return links


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['install', 'uninstall'])
    args = parser.parse_args()
    if os.geteuid() == 0:
        raise SystemExit('Run as your desktop user, without sudo')
    if args.command == 'install':
        if STATE.exists():
            raise SystemExit('Already registered; uninstall user registration first')
        links = plan()
        for name, target in links.items():
            if os.path.lexists(name):
                raise SystemExit(f'Existing file, refusing to overwrite: {name}')
            if not target.exists():
                raise SystemExit(f'Missing installation file: {target}')
        STATE.parent.mkdir(parents=True, exist_ok=True)
        created = {}
        try:
            for name, target in links.items():
                name.parent.mkdir(parents=True, exist_ok=True)
                name.symlink_to(target)
                created[str(name)] = str(target)
        finally:
            STATE.write_text(json.dumps(created, indent=2) + '\n')
        subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
        variables = [v for v in ('WAYLAND_DISPLAY', 'DISPLAY') if v in os.environ]
        if variables:
            subprocess.run(['systemctl', '--user', 'import-environment', *variables], check=True)
        subprocess.run(['systemctl', '--user', 'enable', '--now', 'sogou-ng-engine.service'], check=True)
        print('Registered. Restart Fcitx5 (or log out/in), then add Sogou Native in fcitx5-configtool.')
    else:
        if not STATE.exists():
            raise SystemExit('No registration created by this script')
        subprocess.run(['systemctl', '--user', 'disable', '--now', 'sogou-ng-engine.service'], check=True)
        for name, target in json.loads(STATE.read_text()).items():
            p = Path(name)
            if p.is_symlink() and os.readlink(p) == target:
                p.unlink()
        STATE.unlink()
        subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
        print('Registration removed. Remove Sogou Native from fcitx5-configtool and restart Fcitx5.')


if __name__ == '__main__':
    main()
