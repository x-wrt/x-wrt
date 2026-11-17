#!/usr/bin/env python3
"""Compile AN75xx board trees and validate the AN758x coherent DMA pools.

Usage: python3 npu-dma-dts.py KERNEL_DIR DTC
Use dtc >= 1.7, plus gcc, fdtget and fdtoverlay in PATH.
"""

from pathlib import Path
import subprocess
import sys
import tempfile

kernel = Path(sys.argv[1]).resolve()
dtc = str(Path(sys.argv[2]).resolve())
dts = Path(__file__).resolve().parents[1] / 'dts'


def get(tree, node, prop, fmt='s'):
    return subprocess.check_output(['fdtget', '-t', fmt, str(tree), node, prop], text=True).split()


with tempfile.TemporaryDirectory(prefix='airoha-dma-dts-') as directory:
    directory = Path(directory)
    trees = []
    sources = sorted(dts.glob('an75*.dts')) + sorted(dts.glob('an75*.dtso'))
    for source in sources:
        pre = directory / (source.name + '.pre')
        tree = directory / (source.stem + ('.dtbo' if source.suffix == '.dtso' else '.dtb'))
        with pre.open('w') as output:
            subprocess.run(['gcc', '-E', '-P', '-x', 'assembler-with-cpp', '-nostdinc',
                            '-undef', '-D__DTS__', '-I', str(dts), '-I', str(kernel / 'include'),
                            '-I', str(kernel / 'scripts/dtc/include-prefixes'), str(source)],
                           stdout=output, check=True)
        subprocess.run([dtc, '-@', '-I', 'dts', '-O', 'dtb', '-o', str(tree), str(pre)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if source.suffix == '.dtso':
            merged = directory / (source.stem + '.applied.dtb')
            base = directory / (source.stem.split('-')[0] + '-evb-multievb.dtb')
            subprocess.run(['fdtoverlay', '-i', str(base), '-o', str(merged), str(tree)], check=True)
            tree = merged
        trees.append(tree)

    pools = 0
    for tree in trees:
        if tree.name.startswith('an7563'):
            continue
        pool = '/reserved-memory/npu-dma-pool'
        children = subprocess.check_output(['fdtget', '-l', str(tree), '/reserved-memory'], text=True).split()
        wlan = any(child.startswith('npu-pkt@') for child in children)
        assert get(tree, pool, 'compatible') == ['shared-dma-pool'], tree
        assert get(tree, pool, 'size', 'x') == ['0', '1000000' if wlan else '100000'], tree
        assert get(tree, pool, 'alignment', 'x') == ['0', '100000'], tree
        assert get(tree, pool, 'alloc-ranges', 'x') == ['0', '80000000', '0', '40000000'], tree
        props = subprocess.check_output(['fdtget', '-p', str(tree), pool], text=True).split()
        assert 'no-map' in props and 'reusable' not in props, tree
        names = get(tree, '/soc/npu@1e900000', 'memory-region-names')
        assert names == (['binary', 'pkt', 'tx-pkt', 'tx-bufid', 'ba', 'dma']
                         if wlan else ['binary', 'dma']), tree
        regions = get(tree, '/soc/npu@1e900000', 'memory-region', 'x')
        assert len(regions) == len(names), tree
        assert regions[-1:] == get(tree, pool, 'phandle', 'x'), tree
        pools += 1
    print(f'PASS: {len(trees)} board/overlay trees compiled; {pools} coherent pool configurations checked')
