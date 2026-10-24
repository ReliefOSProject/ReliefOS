#!/usr/bin/env python3
"""Keep private kernel test implementations in the ReliefNT repository."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TestOwnership(unittest.TestCase):
    def test_os_fixtures_do_not_include_private_kernel_implementations(self):
        violations = []
        for directory in ("tests", "tools/tests"):
            for path in sorted((ROOT / directory).rglob("*")):
                if path.suffix not in {".c", ".h", ".cpp"}:
                    continue
                for line, text in enumerate(path.read_text().splitlines(), 1):
                    if (re.match(r'\s*#\s*include\s*["<].*kernel/reliefnt/', text)
                            and '/tools/tests/' not in text):
                        violations.append(f"{path.relative_to(ROOT)}:{line}")
        self.assertEqual(violations, [], "private fixtures belong in ReliefNT")

    def test_os_tests_do_not_own_kernel_module_compositions(self):
        self.assertFalse((ROOT / "tools/kernel_test_sources.py").exists())
        self.assertFalse((ROOT / "tools/tests/abi_layout_golden.json").exists())
        for path in sorted((ROOT / "tools").glob("test_*.py")):
            if path == Path(__file__).resolve():
                continue
            self.assertNotRegex(path.read_text(), r"(?m)^from kernel_test_sources import")

    def test_os_scripts_do_not_compile_kernel_translation_units(self):
        import ast
        for path in sorted((ROOT / "tools").glob("test_*.py")):
            if path == Path(__file__).resolve():
                continue
            for node in ast.walk(ast.parse(path.read_text())):
                if isinstance(node, ast.Constant) and isinstance(node.value, str):
                    self.assertNotRegex(node.value,
                        r"^kernel/reliefnt/(?:arch|block|drivers|firmware|fs|kernel|lib|mm|net|sound)/[^\n]*\.c$",
                        str(path.relative_to(ROOT)))

    def test_os_shell_tests_do_not_inspect_private_kernel_sources(self):
        violations = []
        paths = set((ROOT / "tests").rglob("*.sh"))
        paths.update((ROOT / "tools/tests").rglob("*.sh"))
        paths.update((ROOT / "tools").glob("test*.sh"))
        private_source = re.compile(
            r"kernel/reliefnt/(?:arch|block|drivers|firmware|fs|kernel|lib|mm|net|sound|include/reliefnt)/"
            r"[\w./+-]+\.(?:c|h|cpp|S|s|inc)\b")
        for path in sorted(paths):
            for line, text in enumerate(path.read_text().splitlines(), 1):
                if not text.lstrip().startswith("#") and private_source.search(text):
                    violations.append(f"{path.relative_to(ROOT)}:{line}")
        self.assertEqual(violations, [], "private source assertions belong in ReliefNT")


if __name__ == "__main__":
    unittest.main()
