#!/usr/bin/env python3
"""Switch the shared Fcitx5 classic candidate UI between dark, light and auto."""
import argparse
import json
from pathlib import Path
import dbus

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mode', choices=['dark', 'light', 'auto', 'restore'])
args = parser.parse_args()
controller = dbus.Interface(dbus.SessionBus().get_object('org.fcitx.Fcitx5', '/controller'),
                            'org.fcitx.Fcitx.Controller1')
uri = 'fcitx://config/addon/classicui'
config, _ = controller.GetConfig(uri)
keys = ('Theme', 'DarkTheme', 'UseDarkTheme')
backup = Path.home() / '.local/state/sogo-wayland-fcitx5/appearance-backup.json'
if args.mode == 'restore':
    settings = json.loads(backup.read_text())
else:
    if not backup.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        backup.write_text(json.dumps({k: str(config[k]) for k in keys}, indent=2) + '\n')
    settings = {'Theme': 'default-dark' if args.mode == 'dark' else 'default',
                'DarkTheme': 'default-dark',
                'UseDarkTheme': 'True' if args.mode == 'auto' else 'False'}
for key, value in settings.items():
    config[key] = dbus.String(value, variant_level=1)
controller.SetConfig(uri, dbus.Dictionary(config, signature='sv', variant_level=1))
actual, _ = controller.GetConfig(uri)
if any(str(actual[k]) != v for k, v in settings.items()):
    raise SystemExit('Fcitx5 did not apply the requested appearance')
print('Applied:', args.mode)
