#!/usr/bin/python3
"""Export recent adapter/engine journal entries without enabling key tracing."""
import datetime
import os
from pathlib import Path
import subprocess
import sys

os.umask(0o077)
folder = Path('/opt/sogou-ng/logs')
folder.mkdir(parents=True, exist_ok=True, mode=0o700)
since = sys.argv[1] if len(sys.argv) > 1 else '15 minutes ago'
result = subprocess.run(['journalctl', '--user', '--since', since, '--no-pager',
                         '-o', 'short-precise'], capture_output=True, text=True, check=True)
lines = [line for line in result.stdout.splitlines()
         if any(tag in line for tag in ('sogou-native', 'cpis-engine', 'sogou-ng-engine'))]
path = folder / ('diagnostics-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '.log')
with path.open('x') as output:
    output.write('Sogou diagnostics; since: ' + since + '\n')
    output.write('\n'.join(lines) + '\n')
print(path)
