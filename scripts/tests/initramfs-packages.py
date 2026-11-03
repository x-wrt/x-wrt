#!/usr/bin/env python3
"""Run with python3 scripts/tests/initramfs-packages.py after building host tools."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "staging_dir/host"


class InitramfsPackagesTest(unittest.TestCase):
    def run_command(self, args, **kwargs):
        result = subprocess.run(args, capture_output=True, text=True, timeout=30,
                                **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def prepare(self, path, use_apk):
        for name in ("include", "scripts"):
            (path / name).symlink_to(ROOT / name, target_is_directory=True)
        for name in ("tmp", "repo", "staging/pkginfo", "staging/root-test", "files/etc"):
            (path / name).mkdir(parents=True)
        (path / "tmp/.build").touch()
        (path / "tmp/.packagedeps").write_text(
            "package-$(CONFIG_PACKAGE_builtin-tool) += regular\n"
            "package-$(CONFIG_PACKAGE_recovery-tool) += recovery\n"
            "package-$(CONFIG_PACKAGE_device-tool) += device\n"
            "package-$(CONFIG_PACKAGE_other-device-tool) += other-device\n"
            "package-$(CONFIG_PACKAGE_profile-only-tool) += profile-only\n")
        (path / "staging/pkginfo/regular.default.install").write_text(
            "base-files\nlibc\nkernel\nbuiltin-tool\n")
        (path / "staging/pkginfo/recovery.default.install").write_text(
            "recovery-tool\n")
        (path / "staging/pkginfo/libtest.version").write_text("2026\n")
        for name in ("base-files", "libc", "kernel"):
            (path / f"staging/{name}.version").write_text("1.0-r1\n")
        (path / "files/etc/test-overlay").write_text("shared overlay\n")
        (path / "Makefile").write_text(
            f"include {ROOT}/rules.mk\n"
            f"include {ROOT}/package/Makefile\n")
        if use_apk:
            self.run_command([str(HOST / "bin/openssl"), "ecparam", "-name",
                              "prime256v1", "-genkey", "-noout", "-out",
                              str(path / "private-key.pem")])
            self.run_command([str(HOST / "bin/openssl"), "ec", "-in",
                              str(path / "private-key.pem"), "-pubout", "-out",
                              str(path / "public-key.pem")])

        packages = {
            "libc": "",
            "kernel": "",
            "base-files": "libc",
            "builtin-tool": "",
            "device-tool": "device-lib",
            "device-lib": "",
            "other-device-tool": "",
            "profile-only-tool": "profile-only-lib",
            "profile-only-lib": "",
            "libtest2026": "",
            "recovery-tool": "libtest2026",
        }
        for name, depends in packages.items():
            data = path / f"data-{name}"
            (data / "usr/share").mkdir(parents=True)
            (data / f"usr/share/{name}").write_text(name + "\n")
            script = path / f"{name}.postinst"
            script.write_text('#!/bin/sh\necho installed > '
                              f'"$IPKG_INSTROOT/{name}.configured"\n')
            if use_apk:
                args = [str(HOST / "bin/apk"), "mkpkg", "--info", f"name:{name}",
                        "--info", "version:1.0-r1", "--info", "arch:x86_64",
                        "--files", str(data), "--script", f"post-install:{script}",
                        "--output", str(path / f"repo/{name}-1.0-r1.apk"),
                        "--sign-key", str(path / "private-key.pem")]
                if depends:
                    args += ["--info", f"depends:{depends}"]
                self.run_command(args)
            else:
                control = data / "CONTROL"
                control.mkdir()
                (control / "control").write_text(
                    f"Package: {name}\nVersion: 1.0-r1\nArchitecture: all\n"
                    f"Maintainer: Test\nDescription: Test package\n"
                    + (f"Depends: {depends}\n" if depends else ""))
                (control / "postinst").write_text(script.read_text())
                (control / "postinst").chmod(0o755)
                self.run_command([str(ROOT / "scripts/ipkg-build"), str(data),
                                  str(path / "repo")])

    def build(self, path, use_apk, packages, extra=""):
        (path / ".config").write_text(
            "CONFIG_TARGET_ROOTFS_INITRAMFS=y\n"
            f'CONFIG_TARGET_INITRAMFS_PACKAGES="{packages}"\n'
            "CONFIG_PACKAGE_builtin-tool=y\n"
            "CONFIG_PACKAGE_recovery-tool=m\n"
            "CONFIG_PACKAGE_device-tool=m\n"
            "CONFIG_PACKAGE_other-device-tool=m\n"
            "CONFIG_PACKAGE_profile-only-tool=m\n"
            + ("CONFIG_USE_APK=y\nCONFIG_SIGNED_PACKAGES=y\n" if use_apk else "")
            + extra)
        return self.make(path, "package/install")

    def make(self, path, *args):
        return subprocess.run(
            ["make", "--no-print-directory", "-s", *args,
             f"TOPDIR={path}", "BOARD=test", "ARCH=x86_64",
             "ARCH_PACKAGES=x86_64", f"STAGING_DIR_HOST={HOST}",
             f"STAGING_DIR={path}/staging", f"TARGET_ROOTFS_DIR={path}/roots",
             f"PACKAGE_DIR={path}/repo", f"PACKAGE_DIR_ALL={path}/merged",
             "SOURCE_DATE_EPOCH=1700000000"],
            cwd=path, capture_output=True, text=True, timeout=30,
            env={**os.environ, "PATH": f"{HOST}/bin:{os.environ['PATH']}"})

    def check_device_roots(self, path, extra_packages):
        (path / "kernel").mkdir(exist_ok=True)
        image_makefile = path / "Image.mk"
        image_makefile.write_text(
            f"include {ROOT}/rules.mk\n__target_inc:=1\n"
            f"include {ROOT}/include/image.mk\n"
            "target_conf=$(subst .,_,$(subst -,_,$(subst /,_,$(1))))\n"
            "CONFIG_TARGET_DEVICE_PACKAGES_test_unit_DEVICE_device_a:=profile-only-tool -device-tool\n"
            "DEVICE_CHECK_PROFILE=y\n"
            "DEVICE_PACKAGES:=device-tool -builtin-tool\n"
            "$(eval $(call Device/Check/Common,device_a))\n"
            "DEVICE_PACKAGES:=other-device-tool\n"
            "$(eval $(call Device/Check/Common,device_b))\n"
            "PACKAGES_testid:=device-tool\n"
            "device-roots: initramfs-target-dir-$(INITRAMFS_ID/device_a) "
            "initramfs-target-dir-$(INITRAMFS_ID/device_b) target-dir-testid "
            "$(if $(TARGET_PER_DEVICE_ROOTFS),target-dir-$(ROOTFS_ID/device_a))\n"
            "ids: ; @printf '%s\\n' '$(INITRAMFS_ID/device_a)' "
            "'$(INITRAMFS_ID/device_b)' '$(ROOTFS_ID/device_a)'\n")
        for per_device in ("", "y"):
            settings = ["-f", str(image_makefile),
                        f"KERNEL_BUILD_DIR={path}/kernel",
                        "SUBTARGET=unit",
                        f"CONFIG_TARGET_PER_DEVICE_ROOTFS={per_device}"]
            with self.subTest(per_device=per_device, extras=extra_packages):
                result = self.make(path, *settings, "ids")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                ids = result.stdout.splitlines()
                self.assertEqual(len(ids), 3)
                self.assertTrue(all(len(root_id) == 8 for root_id in ids[:2]))
                self.assertNotEqual(*ids[:2])
                result = self.make(path, *settings, "device-roots")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for index, root_id in enumerate(ids[:2]):
                    device_root = path / f"kernel/initramfs-target-dir-{root_id}"
                    for pkg in ("base-files", "builtin-tool"):
                        self.assertTrue((device_root / f"usr/share/{pkg}").is_file())
                        self.assertTrue((device_root / f"{pkg}.configured").is_file())
                    for pkg in ("device-tool", "device-lib"):
                        self.assertEqual((device_root / f"usr/share/{pkg}").is_file(),
                                         index == 0)
                    self.assertEqual((device_root / "usr/share/other-device-tool").is_file(),
                                     index == 1)
                    for pkg in ("recovery-tool", "libtest2026"):
                        self.assertEqual((device_root / f"usr/share/{pkg}").is_file(),
                                         extra_packages)
                    for pkg in ("profile-only-tool", "profile-only-lib"):
                        self.assertFalse((device_root / f"usr/share/{pkg}").exists())
                if per_device:
                    regular_device = path / f"kernel/target-dir-{ids[2]}"
                    self.assertTrue((regular_device / "usr/share/profile-only-tool").is_file())
                    self.assertTrue((regular_device / "usr/share/profile-only-lib").is_file())
                    self.assertFalse((regular_device / "usr/share/device-tool").exists())
                regular = path / "kernel/target-dir-testid"
                self.assertTrue((regular / "usr/share/device-tool").is_file())
                self.assertFalse((regular / "usr/share/recovery-tool").exists())

    def test_package_selection(self):
        if not any((HOST / f"bin/{name}").is_file() for name in ("apk", "opkg")):
            self.skipTest("build the host apk or opkg tool first")
        for use_apk in (False, True):
            manager = "apk" if use_apk else "opkg"
            if not (HOST / f"bin/{manager}").is_file():
                continue
            with self.subTest(manager=manager), tempfile.TemporaryDirectory(
                    prefix="initramfs-packages-") as tmp:
                path = Path(tmp)
                self.prepare(path, use_apk)
                regular = path / "roots/root-test"
                initramfs = path / "roots/root-initramfs-test"

                result = self.build(path, use_apk, "")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(initramfs.exists())
                self.assertTrue((regular / "usr/share/builtin-tool").is_file())
                self.assertFalse((regular / "usr/share/recovery-tool").exists())
                self.check_device_roots(path, False)

                result = self.build(path, use_apk, "recovery-tool libtest")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((regular / "usr/share/builtin-tool").is_file())
                self.assertFalse((regular / "usr/share/recovery-tool").exists())
                for name in ("base-files", "libc", "kernel", "builtin-tool",
                             "recovery-tool", "libtest2026"):
                    self.assertTrue((initramfs / f"usr/share/{name}").is_file())
                    self.assertTrue((initramfs / f"{name}.configured").is_file())
                self.assertTrue((initramfs / "usr/share/builtin-tool").is_file())
                self.assertEqual((initramfs / "etc/test-overlay").read_text(),
                                 "shared overlay\n")

                self.check_device_roots(path, True)
                self.assertFalse((regular / "usr/share/device-tool").exists())
                self.assertFalse((initramfs / "usr/share/device-tool").exists())

                # Dependency resolution also works without listing the library.
                result = self.build(path, use_apk, "recovery-tool")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((initramfs / "usr/share/libtest2026").is_file())

                # Rebuilding from a smaller list drops previously installed tools.
                result = self.build(path, use_apk, "base-files")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((initramfs / "usr/share/builtin-tool").is_file())
                self.assertFalse((initramfs / "usr/share/recovery-tool").exists())

                result = self.build(path, use_apk, "missing-package")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("missing-package", result.stdout + result.stderr)

    def test_rootfs_routing(self):
        with tempfile.TemporaryDirectory(prefix="initramfs-routing-") as tmp:
            path = Path(tmp)
            harness = path / "Makefile"
            harness.write_text(
                f"TOPDIR:={ROOT}\nDUMP:=1\n__target_inc:=1\n"
                f"include {ROOT}/rules.mk\ninclude {ROOT}/include/image.mk\n"
                "image_prepare: ;\nrecord:\n"
                "\t$(info $(call Kernel/CompileImage/Initramfs))\n"
                "record-fit:\n\t$(info $(call Build/fit-its,gzip with-initrd))\n")
            common = ["make", "--no-print-directory", "-s", "-f", str(harness),
                      "BOARD=test", "SUBTARGET=test",
                      f"TARGET_ROOTFS_DIR={path}/roots", f"KERNEL_BUILD_DIR={path}",
                      f"LINUX_DIR={path}/linux", "CONFIG_TARGET_ROOTFS_INITRAMFS=y",
                      'CONFIG_EXTERNAL_CPIO=""']
            for custom in ("", "recovery-tool"):
                source = str(path / ("roots/root-initramfs-test" if custom
                                     else "roots/root-test"))
                settings = [f"CONFIG_TARGET_INITRAMFS_PACKAGES={custom}"]
                for separate in ("", "y"):
                    output = self.run_command(common + settings + [
                        f"CONFIG_TARGET_ROOTFS_INITRAMFS_SEPARATE={separate}", "record"])
                    expected = (f"cd {source};" if separate
                                else f'CONFIG_INITRAMFS_SOURCE="{source} ')
                    self.assertIn(expected, output)

                for per_device in ("", "y"):
                    output = self.run_command(common + settings + [
                        f"CONFIG_TARGET_PER_DEVICE_ROOTFS={per_device}",
                        "Kernel/CompileImage/Initramfs=echo source=$(1) suffix=$(2)",
                        "-n", "-o", "image_prepare",
                        str(path / ".initramfs-kernel.testid")])
                    expected = str(path / "initramfs-target-dir-testid")
                    self.assertIn(f"source={expected} suffix=.testid", output)
                    self.assertEqual("root.orig-test" in output, not custom)
                    if custom:
                        self.assertIn(f"{source}.orig", output)
                    output = self.run_command(common + settings + [
                        f"CONFIG_TARGET_PER_DEVICE_ROOTFS={per_device}",
                        "CONFIG_TARGET_ROOTFS_INITRAMFS_SEPARATE=y",
                        "DEVICE_NAME=test-device", "INITRAMFS_ID/test-device=initramfs-id",
                        "ROOTFS_ID/test-device=firmware-id", "record-fit"])
                    self.assertIn(f"-i {path}/initrd.initramfs-id.cpio", output)

            for setting in ("CONFIG_EXTERNAL_CPIO=external.cpio",
                            "CONFIG_TARGET_ROOTFS_INITRAMFS="):
                output = self.run_command(common + [
                    "CONFIG_TARGET_INITRAMFS_PACKAGES=recovery-tool", setting,
                    "val.INITRAMFS_PACKAGES", "val.TARGET_DIR_INITRAMFS"])
                self.assertEqual(output.splitlines(),
                                 ["", str(path / "roots/root-test")])


if __name__ == "__main__":
    unittest.main()
