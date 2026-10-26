#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the driver's flow command engine against a consuming-register mock.

Requires a host C compiler; does not emulate forwarding or prove silicon support.
The register definitions, bit helpers and command functions come from the driver.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source, name):
    start = source.index("static inline", source.index(name) - 30)
    end = source.index("\n}", start) + 2
    return source[start:end]


base = Path(__file__).resolve().parent
driver = base.parent / "files/drivers/net/ethernet/qualcomm"
header = (driver / "qca_ppe.h").read_text()
source = (driver / "qca_ppe_flow.c").read_text()
# Preserve continuation lines without copying the kernel's include graph.
defines = []
lines = iter(re.sub(r"/\*.*?\*/", "", header, flags=re.S).splitlines())
for line in lines:
    if not re.match(r"#define\s+PPE_", line):
        continue
    define = line
    while line.endswith("\\"):
        line = next(lines)
        define += "\n" + line
    defines.append(define)
engine = source[source.index("#define PPE_FLOW_OP_RETRIES"):
                source.index("static int ppe_flows_show")]
helpers = "\n".join(function(header, name)
                    for name in ("ppe_entry_set", "ppe_entry_get"))
mock = (base / "ppe-flow-mock.c").read_text()
preamble, tests = mock.split("/* DRIVER_UNDER_TEST */")
with tempfile.TemporaryDirectory(prefix="qualcommax-ppe-test-") as tmp:
    cfile = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    cfile.write_text("\n".join((preamble, *defines, helpers, engine, tests)))
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
