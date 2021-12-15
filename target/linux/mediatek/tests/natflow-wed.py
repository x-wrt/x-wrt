#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production natflow WED ownership/flush/idle paths with mocked hardware."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

base = Path(__file__).resolve().parent
linux = base.parent.parent


def added_source(platform, name):
    patch = (linux / platform / 'patches-6.18' /
             '995-0001-hwnat-add-natflow-flow-offload-support.patch').read_text()
    marker = '+++ b/drivers/net/ethernet/mediatek/' + name + '\n'
    body = patch.split(marker, 1)[1].split('\n--- ', 1)[0]
    return '\n'.join(line[1:] for line in body.splitlines()
                     if line.startswith('+'))


def function(source, signature):
    start = source.index(signature)
    end = source.index('\n}', start) + 2
    return source[start:end]


functions = []
for name, signature in (
    ('mtk_ppe1.c', 'int mtk_ppe_wed_flow_update('),
    ('mtk_ppe_offload1.c', 'void mtk_flow_offload_unbind_wcid('),
    ('mtk_ppe_offload1.c', 'int mtk_flow_offload_idle_wcid('),
):
    production = function(added_source('mediatek', name), signature)
    assert production == function(added_source('ramips', name), signature)
    functions.append(production)

mock = (base / 'natflow-wed-mock.c').read_text()
with tempfile.TemporaryDirectory(prefix='natflow-wed-test-') as tmp:
    source, binary = Path(tmp) / 'test.c', Path(tmp) / 'test'
    source.write_text(mock.replace('/* PRODUCTION */', '\n'.join(functions)))
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
