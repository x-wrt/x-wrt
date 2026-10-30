#!/usr/bin/env python3
"""Exercise deployment refusal paths inside an unprivileged mount namespace.

No host configuration or real data directory is writable in these tests.
Requires bubblewrap; the end-to-end deployment is verified on an X-Wrt host.
"""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "deploy-ubuntu-schroot.sh"


class DeploymentGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("bwrap"):
            raise unittest.SkipTest("bubblewrap is not installed")
        probe = subprocess.run(
            ["bwrap", "--ro-bind", "/", "/", "--unshare-user", "--uid", "0",
             "--gid", "0", "/bin/true"], capture_output=True
        )
        if probe.returncode:
            raise unittest.SkipTest("unprivileged user namespaces are unavailable")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="schroot-deploy-test-")
        self.addCleanup(self.temp.cleanup)
        self.base = pathlib.Path(self.temp.name)
        self.etc = self.base / "etc"
        self.data = self.base / "data"
        self.etc.mkdir()
        self.data.mkdir()
        (self.etc / "openwrt_release").write_text("DISTRIB_ID='X-WRT'\n")

    def invoke(self, *args, volatile=False):
        command = [
            "bwrap", "--ro-bind", "/", "/", "--unshare-user", "--uid", "0",
            "--gid", "0", "--bind", str(self.etc), "/etc", "--tmpfs", "/tmp",
            "--proc", "/proc", "--dev", "/dev",
            "--ro-bind", str(SCRIPT.parent), "/opt",
        ]
        if volatile:
            command += ["--tmpfs", "/mnt"]
        else:
            command += ["--bind", str(self.data), "/mnt"]
        command += ["/bin/busybox", "ash", "/opt/deploy-ubuntu-schroot.sh",
                    "--root", "/mnt/ubuntu", *args]
        return subprocess.run(command, capture_output=True, universal_newlines=True)

    def assert_refused(self, result, message):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(message, result.stderr)

    def test_help_does_not_create_files(self):
        result = self.invoke("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--repair", result.stdout)
        self.assertEqual(list(self.data.iterdir()), [])

    def test_invalid_swap_size_is_rejected(self):
        for size in ["-1", "four", "131073", "99999999999999999999"]:
            with self.subTest(size=size):
                self.assert_refused(self.invoke("--swap-mb", size), "Swap" if size.isdigit() else "--swap-mb")
        self.assertEqual(list(self.data.iterdir()), [])

    def test_volatile_storage_is_rejected(self):
        self.assert_refused(self.invoke("--repair", volatile=True), "持久磁盘")

    def test_existing_unrelated_files_are_preserved(self):
        root = self.data / "ubuntu"
        root.mkdir()
        sentinel = root / "important.txt"
        sentinel.write_bytes(b"Do not replace existing data.\n")
        self.assert_refused(self.invoke(), "不会覆盖")
        self.assertEqual(sentinel.read_bytes(), b"Do not replace existing data.\n")
        self.assertEqual(list(root.iterdir()), [sentinel])

    def test_missing_environment_cannot_be_repaired(self):
        self.assert_refused(self.invoke("--repair"), "--repair 需要已有完整 Ubuntu")
        self.assertFalse((self.data / "ubuntu").exists())

    def test_symlink_cannot_be_used_as_root(self):
        target = self.data / "real-data"
        target.mkdir()
        (self.data / "ubuntu").symlink_to("real-data")
        self.assert_refused(self.invoke("--repair"), "不能是软链接")
        self.assertEqual(list(target.iterdir()), [])

    def test_other_schroot_is_not_reconfigured(self):
        root = self.data / "ubuntu"
        for subdir in ["etc", "usr/bin", "bin"]:
            (root / subdir).mkdir(parents=True)
        (root / "etc/os-release").write_text('ID=ubuntu\nVERSION_ID="24.04"\n')
        for filename in ["usr/bin/dpkg", "bin/bash"]:
            executable = root / filename
            executable.write_text("#!/bin/sh\nexit 0\n")
            executable.chmod(0o755)
        confdir = self.etc / "schroot/chroot.d"
        confdir.mkdir(parents=True)
        config = confdir / "ubuntu.conf"
        content = "[ubuntu]\ndirectory=/data/someone-elses-ubuntu\n"
        config.write_text(content)
        self.assert_refused(self.invoke("--repair"), "拒绝替换")
        self.assertEqual(config.read_text(), content)
        self.assertFalse((self.etc / "sysupgrade.conf").exists())


if __name__ == "__main__":
    unittest.main()
