#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production PPE worker and natflow client with mocked hardware.

Usage: python3 ppe-natflow.py --natflow /path/to/natflow
No device traffic, concurrency or silicon behavior is emulated.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--natflow", type=Path, required=True)
args = parser.parse_args()
base = Path(__file__).resolve().parent
files = base.parent / "files"
driver = files / "drivers/net/ethernet/qualcomm"


def no_includes(source):
    return re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)


def function(source, signature):
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


api = no_includes((files / "include/linux/qca_ppe_natflow.h").read_text())
data_header = (driver / "qca_ppe_flow.h").read_text()
data = data_header[data_header.index("struct ppe_flow_data {"):
                   data_header.index("struct ppe_flow_entry {")]
backend = no_includes((driver / "qca_ppe_natflow.c").read_text())
core = (driver / "qca_ppe_flow_offload.c").read_text()
free = function(core, "void ppe_flow_free(struct qca_ppe_priv *priv,")
client = (args.natflow / "natflow_qca.h").read_text()
mock = (base / "ppe-natflow-mock.c").read_text()
pre, middle, tests = mock.split("/* PRODUCTION */")
with tempfile.TemporaryDirectory(prefix="ppe-natflow-test-") as tmp:
    cfile = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    cfile.write_text("\n".join((pre, api, data, middle, backend, free, client, tests)))
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
