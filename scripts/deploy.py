#!/usr/bin/env python3
"""Install the pinned engine without running Debian maintainer scripts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import pwd
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
PREFIX = Path('/opt/sogou-ng')


def verify_packages(repo=REPO):
    manifest = json.loads((repo / 'vendor/manifest.json').read_text())
    files = []
    for item in manifest:
        name = item['file']
        if Path(name).name != name or not name.endswith('.deb'):
            raise ValueError('Invalid package filename')
        package = repo / 'vendor/debs' / name
        if hashlib.sha256(package.read_bytes()).hexdigest() != item['sha256']:
            raise ValueError(f'Checksum mismatch: {name}')
        files.append(package)
    return files


def link_plan(root, prefix=PREFIX):
    paths = ['etc/cpis', 'usr/share/cpis', 'var/lib/cpis']
    paths += [str(p.relative_to(root)) for p in
              sorted((root / 'usr/lib/x86_64-linux-gnu').iterdir())]
    return {str(Path('/') / p): str(prefix / 'root' / p) for p in paths}


def conflicts(links):
    return [p for p in links if os.path.lexists(p)]


def install(args):
    packages = verify_packages()
    if args.check:
        print(f'OK: {len(packages)} pinned packages verified; no system changes.')
        return
    if os.geteuid() != 0:
        raise SystemExit('Run with sudo and --user YOUR_LOGIN')
    if platform.machine() != 'x86_64':
        raise SystemExit('Only x86_64 is supported')
    user = pwd.getpwnam(args.user)
    if user.pw_uid == 0:
        raise SystemExit('Choose a non-root desktop user')
    if PREFIX.exists() or PREFIX.is_symlink():
        raise SystemExit(f'{PREFIX} already exists; refusing to overwrite it.')
    library = REPO / 'build/libsogou-native.so'
    if not library.is_file():
        raise SystemExit('Run make first')
    created = []
    try:
        with tempfile.TemporaryDirectory(prefix='sogou-stage-') as temporary:
            root = Path(temporary) / 'root'
            root.mkdir()
            for package in packages:
                subprocess.run(['dpkg-deb', '-x', str(package), str(root)], check=True)
            for relative in ['usr/bin/cpis-engine-service', 'etc/cpis/ise.ini',
                             'usr/share/cpis', 'var/lib/cpis']:
                if not (root / relative).exists():
                    raise RuntimeError(f'Missing engine file: {relative}')
            links = link_plan(root)
            occupied = conflicts(links)
            if occupied:
                raise RuntimeError('Existing CPIS files; refusing to overwrite:\n' + '\n'.join(occupied))
            PREFIX.mkdir()
            shutil.copytree(root, PREFIX / 'root', symlinks=True)
        shutil.copy2(library, PREFIX / library.name)
        shutil.copytree(REPO / 'data', PREFIX / 'integration')
        userdata = PREFIX / 'userdata'
        userdata.mkdir(mode=0o700)
        os.chown(userdata, user.pw_uid, user.pw_gid)
        for name, target in links.items():
            Path(name).symlink_to(target)
            created.append(name)
        (PREFIX / 'deployment.json').write_text(json.dumps(
            {'project': 'sogo-wayland-fcitx5', 'user': args.user, 'links': links}, indent=2) + '\n')
        print('Installed. As the desktop user, run: python3 scripts/user-setup.py install')
    except Exception:
        for name in reversed(created):
            Path(name).unlink(missing_ok=True)
        # PREFIX was absent before this install attempt.
        if PREFIX.exists():
            shutil.rmtree(PREFIX)
        raise


def uninstall(args):
    if os.geteuid() != 0:
        raise SystemExit('Run uninstall with sudo after scripts/user-setup.py uninstall')
    state = json.loads((PREFIX / 'deployment.json').read_text())
    if state.get('project') != 'sogo-wayland-fcitx5':
        raise SystemExit('Unrecognized installation')
    for name, target in state['links'].items():
        p = Path(name)
        if p.is_symlink() and os.readlink(p) == target:
            p.unlink()
    for name in ['root', 'integration']:
        shutil.rmtree(PREFIX / name)
    (PREFIX / 'libsogou-native.so').unlink()
    (PREFIX / 'deployment.json').unlink()
    print(f'Engine removed. User dictionaries preserved in {PREFIX}/userdata; back up or remove separately.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    install_parser = sub.add_parser('install')
    install_parser.add_argument('--user', default=os.environ.get('SUDO_USER', ''))
    install_parser.add_argument('--check', action='store_true', help='Verify package checksums only')
    sub.add_parser('uninstall')
    args = parser.parse_args()
    if args.command == 'install':
        install(args)
    else:
        uninstall(args)


if __name__ == '__main__':
    main()
