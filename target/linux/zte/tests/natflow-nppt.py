#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the NPPT worker with mocked rules, stats and netdevices."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

base = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--natflow", type=Path, required=True, help="Use the client from this natflow source tree")
parser.add_argument("--require-macvlan-pppoe", action="store_true",
                    help="Require the client to admit PPPoE over a direct macvlan")
args = parser.parse_args()
patches = base.parent / "patches-7.3"
base_patch = (patches / "490-net-zx279133-natflow.patch").read_text()


def added_lines(diff):
    return "\n".join(line[1:] for line in diff.splitlines()
                     if line.startswith("+") and not line.startswith("+++")) + "\n"


def added_file(patch, name):
    for diff in patch.split("\n+++ b/")[1:]:
        if diff.splitlines()[0].endswith(name):
            return added_lines(diff.split("\n--- ", 1)[0])
    raise ValueError(name)


def function(source, signature):
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


def without_includes(source):
    return "\n".join(line for line in source.splitlines()
                     if not line.startswith("#include"))


api = "include/linux/zte_nppt_natflow.h"
worker = "drivers/net/ethernet/zte/zx279133-offload-natflow.h"
policy = without_includes(added_file(base_patch, "/zx279133-policy.h"))
driver = added_file(base_patch, "/zx279133-offload.c")
encoding = "\n".join(function(driver, signature) for signature in (
    "static void zx279133_put_ipv4",
    "static void zx279133_fast_response_build",
    "static u64 zx279133_flow_ip_bytes",
))
async_header = args.natflow / "natflow_async.h"
if async_header.is_file():
    frontend_source = (args.natflow / "natflow_zte.h").read_text()
    decode_source = async_header.read_text()
    bind, unbind = "natflow_async_backend_bind", "natflow_async_backend_unbind"
    path_valid = "natflow_async_backend_path_valid"
else:
    frontend_source = decode_source = (args.natflow / "natflow_qca.h").read_text()
    bind, unbind = "natflow_zte_bind", "natflow_zte_unbind"
    path_valid = "natflow_zte_path_valid"

binding = "static const struct zte_nppt_natflow_api *natflow_zte_api;\n"
binding += function(frontend_source, f"static void {bind}(").replace(bind, "natflow_async_backend_bind") + "\n"
binding += function(frontend_source, f"static void {unbind}(").replace(unbind, "natflow_async_backend_unbind")
frontend = function(decode_source, "static bool natflow_async_decode_l2") + "\n"
frontend += "\n".join(function(frontend_source, signature) for signature in (
    "static bool natflow_zte_pppoe(",
    "static bool natflow_zte_path(",
    f"static bool {path_valid}(",
))
frontend = frontend.replace(path_valid, "natflow_async_backend_path_valid")
frontend = frontend.replace("struct natflow_async_ticket", "struct natflow_async_backend_ticket")
macvlan_pppoe = args.require_macvlan_pppoe or "get_macvlan_real_dev(" in frontend
frontend = f"#define TEST_NATFLOW_MACVLAN_PPPOE {int(macvlan_pppoe)}\n" + frontend
mock = (base / "natflow-nppt-mock.c").read_text()

with tempfile.TemporaryDirectory(prefix="zte-natflow-test-") as temp:
    tmp = Path(temp)
    source = mock.replace("/* NPPT API */", without_includes(added_file(base_patch, api)))
    source = source.replace("/* NPPT VLAN POLICY */", policy)
    source = source.replace("/* NPPT ENCODING */", encoding)
    source = source.replace("/* NPPT WORKER */", without_includes(added_file(base_patch, worker)))
    source = source.replace("/* NATFLOW BINDING */", binding)
    source = source.replace("/* NATFLOW PATH */", frontend)
    cfile = tmp / "test.c"
    executable = tmp / "test"
    cfile.write_text(source)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        str(cfile), "-o", str(executable),
    ], check=True)
    subprocess.run([executable], check=True)
