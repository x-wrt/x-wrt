#!/usr/bin/env python3
"""Run with python3 scripts/tests/package-metadata.py; build scripts/config/conf
to include Kconfig evaluation checks.
"""

import itertools
import os
from pathlib import Path
import resource
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/package-metadata.pl"
CONF = ROOT / "scripts/config/conf"
VALUES = ("n", "m", "y")


def limit_memory():
    # A recursion regression must fail without exhausting the host's memory.
    resource.setrlimit(resource.RLIMIT_AS, (128 * 1024 * 1024,) * 2)


class PackageMetadataTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="package-metadata-test-")
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)

    def generate(self, condition, depend="C", types=None, extra_config=""):
        if types is None:
            types = {"A": "bool", "B": "bool", "C": "bool"}
        symbols = "".join(
            f'config {name}\n\t{kind} "{name}"\n'
            for name, kind in types.items()
        )
        return self.generate_metadata(f"""Source-Makefile: package/consumer/Makefile
Package: consumer
Title: consumer
Category: Test
Depends: +{condition}:provider
Description: Consumer test package
@@
Config:
{symbols}{extra_config}@@

Source-Makefile: package/provider/Makefile
Package: provider
Title: provider
Category: Test
Depends: @{depend}
Description: Provider test package
@@

Source-Makefile: package/support/Makefile
Package: support
Title: support
Category: Test
Type: ipkg
Description: Tristate package gate
@@
""")

    def generate_metadata(self, content):
        metadata = self.path / "packageinfo"
        metadata.write_text(content)
        result = subprocess.run(
            ["perl", str(SCRIPT), "config", str(metadata)],
            cwd=ROOT, capture_output=True, text=True, timeout=5,
            preexec_fn=limit_memory, check=True,
        )
        self.assertEqual(result.stderr, "")
        return result.stdout[result.stdout.index('menu "Test"'):]

    def consumer(self, generated):
        return generated.split("config PACKAGE_consumer\n", 1)[1].split("\t\thelp", 1)[0]

    def test_opaque_conditions_keep_gate(self):
        for condition in ("(A&&B)||C", "!(A&&B)", "A||B&&C",
                          "A&&B||C", '(A="x&&C&&y")'):
            with self.subTest(condition=condition):
                result = self.consumer(self.generate(condition))
                self.assertIn(f"depends on !({condition}) || C\n", result)

    def test_boolean_conjuncts_drop_gate(self):
        for condition in ("C", "(A&&C)", "A&&(B&&C)",
                          "C&&(A||B)", "C&&!(A&&B)"):
            with self.subTest(condition=condition):
                self.assertNotIn("depends on", self.consumer(self.generate(condition)))

    def test_known_core_boolean(self):
        result = self.consumer(self.generate("A&&IPV6", "IPV6"))
        self.assertNotIn("depends on", result)

    def test_package_boolean_option(self):
        result = self.generate("A&&PACKAGE_feature", "PACKAGE_feature",
                               {"A": "bool", "PACKAGE_feature": "bool"})
        self.assertNotIn("depends on", self.consumer(result))

    def test_def_bool_declaration(self):
        result = self.generate("A&&C", types={"A": "bool"},
                               extra_config="config C\n\tdef_bool y\n")
        self.assertNotIn("depends on", self.consumer(result))

    def test_help_examples_do_not_define_boolean_symbols(self):
        help_text = ('config DOCUMENTATION\n\tbool "Documentation"\n\thelp\n'
                     '\t  config UNKNOWN\n\t    bool "Example only"\n')
        result = self.generate("A&&UNKNOWN", "UNKNOWN", extra_config=help_text)
        self.assertIn("depends on !(A&&UNKNOWN) || UNKNOWN\n", self.consumer(result))

    def test_tristate_and_unknown_symbols_keep_gate(self):
        for depend, types in (("C", {"A": "bool", "C": "tristate"}),
                              ("PACKAGE_support", {"A": "bool"}),
                              ("UNKNOWN", {"A": "bool"})):
            with self.subTest(depend=depend):
                condition = f"A&&{depend}"
                result = self.consumer(self.generate(condition, depend, types))
                self.assertIn(f"depends on !({condition}) || {depend}\n", result)

    def generate_providers(self, conflicts, default_variant=None, selected=True):
        packages = (
            ("consumer", "Depends: +provider\n" if selected else ""),
            ("provider", ""),
            ("alternative", "Provides: provider\n"),
            ("unrelated", ""),
        )
        metadata = ""
        for name, fields in packages:
            metadata += (f"Source-Makefile: package/{name}/Makefile\n"
                         f"Package: {name}\nTitle: {name}\nCategory: Test\n"
                         f"Type: ipkg\n{fields}")
            if name in conflicts:
                metadata += f"Conflicts: {conflicts[name]}\n"
            if name == default_variant:
                metadata += "Default-Variant: 1\n"
            metadata += f"Description: {name}\n@@\n\n"
        return self.generate_metadata(metadata)

    def test_provider_conflict_follows_select_direction(self):
        for default_variant, preferred, other in (
            (None, "provider", "alternative"),
            ("provider", "provider", "alternative"),
            ("alternative", "alternative", "provider"),
        ):
            with self.subTest(default_variant=default_variant):
                generated = self.generate_providers(
                    {other: preferred}, default_variant)
                self.assertIn(f"select PACKAGE_{preferred} if "
                              f"PACKAGE_{other}<PACKAGE_consumer\n", generated)
                preferred_config = generated.split(f"config PACKAGE_{preferred}\n", 1)[1].split("\t\thelp", 1)[0]
                other_config = generated.split(f"config PACKAGE_{other}\n", 1)[1].split("\t\thelp", 1)[0]
                self.assertIn(f"depends on m || (PACKAGE_{other} != y)\n", preferred_config)
                self.assertNotIn("depends on", other_config)
                for selected, expected in (("n", "y"), ("m", "y"), ("y", "n")):
                    with self.subTest(other=selected):
                        result = self.evaluate(generated, {f"PACKAGE_{other}": selected})
                        self.assertEqual(result.get(f"PACKAGE_{preferred}", "n"), expected)
                        self.assertEqual(result.get(f"PACKAGE_{other}", "n"), selected)

    def test_provider_conflicts_forbid_two_builtin_packages(self):
        generated = self.generate_providers({"alternative": "provider"})
        for provider, alternative in itertools.product(VALUES, repeat=2):
            with self.subTest(provider=provider, alternative=alternative):
                result = self.evaluate(generated, {
                    "PACKAGE_consumer": "n", "PACKAGE_provider": provider,
                    "PACKAGE_alternative": alternative,
                })
                self.assertFalse(result.get("PACKAGE_provider") == "y" and
                                 result.get("PACKAGE_alternative") == "y")
                if (provider, alternative) != ("y", "y"):
                    self.assertEqual(result.get("PACKAGE_provider", "n"), provider)
                    self.assertEqual(result.get("PACKAGE_alternative", "n"), alternative)

    def test_provider_conflicts_preserve_unrelated_packages(self):
        generated = self.generate_providers({"alternative": "provider unrelated"})
        alternative_config = generated.split("config PACKAGE_alternative\n", 1)[1].split("\t\thelp", 1)[0]
        self.assertIn("depends on m || (PACKAGE_unrelated != y)\n", alternative_config)
        self.assertNotIn("depends on m || (PACKAGE_provider != y)\n", alternative_config)

    def test_providers_without_declared_conflicts(self):
        generated = self.generate_providers({})
        self.assertNotIn("depends on m ||", generated)
        result = self.evaluate(generated, {
            "PACKAGE_provider": "y", "PACKAGE_alternative": "y",
        })
        self.assertEqual(result["PACKAGE_provider"], "y")
        self.assertEqual(result["PACKAGE_alternative"], "y")

    def test_default_variant_keeps_implicit_conflict(self):
        generated = self.generate_providers({}, "provider")
        self.assertIn("depends on m || (PACKAGE_alternative != y)\n", generated)
        self.evaluate(generated, {"PACKAGE_alternative": "y"})

    def test_bidirectional_provider_conflict_is_emitted_once(self):
        generated = self.generate_providers({
            "provider": "alternative", "alternative": "provider",
        })
        self.assertEqual(generated.count("depends on m ||"), 1)
        self.evaluate(generated, {"PACKAGE_alternative": "y"})

    def test_unselected_provider_conflict_keeps_direction(self):
        generated = self.generate_providers(
            {"alternative": "provider"}, "provider", selected=False)
        self.assertIn("depends on m || (PACKAGE_provider != y)\n", generated)
        self.assertNotIn("depends on m || (PACKAGE_alternative != y)\n", generated)
        self.evaluate(generated, {"PACKAGE_alternative": "y"})

    def evaluate(self, generated, settings):
        if not CONF.is_file():
            self.skipTest("build scripts/config/conf for Kconfig checks")
        kconfig = self.path / "Kconfig"
        kconfig.write_text('mainmenu "test"\nconfig MODULES\n'
                           '\tbool\n\tmodules\n\tdefault y\n' + generated)
        config = self.path / "defconfig"
        config.write_text("".join(
            f"CONFIG_{name}={value}\n"
            for name, value in {"PACKAGE_consumer": "y", **settings}.items()
        ))
        output = self.path / "result"
        result = subprocess.run(
            [str(CONF), "--fatalrecursive", "--defconfig=" + str(config), str(kconfig)],
            cwd=self.path, env=dict(os.environ, KCONFIG_CONFIG=str(output)),
            capture_output=True, text=True, timeout=5, check=True,
        )
        self.assertEqual(result.stderr, "")
        return dict(line[7:].split("=", 1) for line in output.read_text().splitlines()
                    if line.startswith("CONFIG_"))

    def test_precedence_in_kconfig(self):
        result = self.evaluate(self.generate("A||B&&C"), {"A": "y", "B": "n", "C": "n"})
        self.assertNotIn("PACKAGE_consumer", result)
        self.assertNotIn("PACKAGE_provider", result)

    def test_tristate_truth_tables(self):
        conditions = (
            ("A&&C", lambda a, b, c: min(a, c)),
            ("C&&(A||B)", lambda a, b, c: min(c, max(a, b))),
            ("(A&&B)||C", lambda a, b, c: max(min(a, b), c)),
            ("A||B&&C", lambda a, b, c: max(a, min(b, c))),
            ("!(A&&B)", lambda a, b, c: 2 - min(a, b)),
        )
        for expression, evaluate in conditions:
            generated = self.generate(expression, types={"A": "bool", "B": "bool", "C": "tristate"})
            for a, b, c in itertools.product((0, 2), (0, 2), range(3)):
                with self.subTest(expression=expression, a=a, b=b, c=c):
                    result = self.evaluate(generated, dict(zip(("A", "B", "C"),
                                                               (VALUES[a], VALUES[b], VALUES[c]))))
                    gate = max(2 - evaluate(a, b, c), c)
                    self.assertEqual(result.get("PACKAGE_consumer", "n"), VALUES[gate])


if __name__ == "__main__":
    unittest.main()
