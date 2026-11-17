#!/usr/bin/env python3
"""Reconstruct and test the pinned AN7552 community firmware in a scratch dir.

Usage: python3 an7552-source-check.py SOURCE_PATCHES.tar.gz OUTPUT_DIR
The archive is from the be5000-openwrt-20261109 release; its hash is pinned
below. OUTPUT_DIR must not exist. No files are installed in an OpenWrt image.
The test sources map the documented NPU workspace at fixed virtual addresses.
"""

import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile

archive, output = Path(sys.argv[1]), Path(sys.argv[2]).resolve()
expected = 'e2c5e730f55aa0de62168457043912d3f1354ff3f71a7b55c18ea99d35b8a044'
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('Unexpected source archive SHA-256')
output.mkdir(parents=True, exist_ok=False)
prefix = 'package/firmware/an7552-npu-firmware/'
count = 0
with tarfile.open(archive) as tar:
    members = sorted((member for member in tar.getmembers()
                      if member.isfile() and member.name.endswith('.patch')
                      and not Path(member.name).name.startswith('._')),
                     key=lambda member: Path(member.name).name)
    for member in members:
        data = tar.extractfile(member).read().decode()
        parts = re.split(r'(?=^diff --git )', data, flags=re.M)
        selected = [part for part in parts if part.startswith('diff --git a/' + prefix)]
        if not selected:
            continue
        subprocess.run(['patch', '-p1', '-F0', '--batch', '--forward'],
                       input=''.join(selected), text=True, cwd=output,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        count += 1
print(f'Reconstructed firmware from {count} patches', flush=True)

# Older libc headers omit this Linux UAPI flag although the running kernel
# supports it (Linux >= 4.17). Keep the NOREPLACE semantics of the tests.
env = os.environ.copy()
hostcc = env.get('HOSTCC', 'cc')
macros = subprocess.check_output(shlex.split(hostcc) + ['-dM', '-E', '-include', 'sys/mman.h', '-'],
                                 input='', text=True)
if '#define MAP_FIXED_NOREPLACE ' not in macros:
    hostcc += ' -DMAP_FIXED_NOREPLACE=0x100000'
env['HOSTCC'] = hostcc
source = output / prefix / 'src'
subprocess.run(['make', '-j4', 'check'], cwd=source, env=env, check=True)
subprocess.run(['make', 'mutate'], cwd=source, env=env, check=True)
