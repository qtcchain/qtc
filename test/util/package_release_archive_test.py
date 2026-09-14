#!/usr/bin/env python3
"""Unit coverage for scripts/release/package_release_archive.py."""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import sys
import tarfile
import tempfile
import unittest
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT_PATH = ROOT / "scripts" / "release" / "package_release_archive.py"


def load_module():
    spec = importlib.util.spec_from_file_location("package_release_archive", SCRIPT_PATH)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class PackageReleaseArchiveTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self._stub_ship_gate()

    def _stub_ship_gate(self):
        # The ship gate needs real ELF/Mach-O binaries; these tests package text stubs.
        self.module.verify_shipped_qtcd = lambda path: None
        self.module.verify_shipped_cli = lambda path: None

    def _build_source_root(self, root: pathlib.Path) -> pathlib.Path:
        source_root = root / "source-root"
        for relative_path in self.module.SUPPORT_FILES:
            path = source_root / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"{relative_path}\n", encoding="utf-8")
        return source_root

    def _write_binaries(self, root: pathlib.Path, *, windows: bool = False) -> tuple[pathlib.Path, pathlib.Path]:
        suffix = ".exe" if windows else ""
        qtcd = root / f"qtcd{suffix}"
        qtc_cli = root / f"qtc-cli{suffix}"
        qtc_util = root / f"qtc-util{suffix}"
        qtcd.write_text("daemon\n", encoding="utf-8")
        qtc_cli.write_text("cli\n", encoding="utf-8")
        qtc_util.write_text("util\n", encoding="utf-8")
        return qtcd, qtc_cli

    def test_linux_archive_includes_binaries_and_helpers(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            source_root = self._build_source_root(root)
            qtcd, qtc_cli = self._write_binaries(root)
            output_dir = root / "out"

            exit_code = self.module.main(
                [
                    "--output-dir",
                    str(output_dir),
                    "--version",
                    "29.2",
                    "--platform-id",
                    "linux-x86_64",
                    "--qtcd",
                    str(qtcd),
                    "--qtc-cli",
                    str(qtc_cli),
                    "--source-root",
                    str(source_root),
                ]
            )

            self.assertEqual(exit_code, 0)
            archive_path = output_dir / "qtc-29.2-x86_64-linux-gnu.tar.gz"
            self.assertTrue(archive_path.is_file())
            with tarfile.open(archive_path, "r:gz") as archive:
                names = set(archive.getnames())
                self.assertIn("qtc-29.2/bin/qtcd", names)
                self.assertIn("qtc-29.2/bin/qtc-cli", names)
                self.assertIn("qtc-29.2/libexec/qtcd.real", names)
                self.assertIn("qtc-29.2/libexec/qtc-cli.real", names)
                self.assertIn("qtc-29.2/contrib/faststart/qtc-faststart.py", names)
                self.assertIn("qtc-29.2/contrib/mining/start-live-mining.sh", names)
                self.assertIn("qtc-29.2/doc/qtc-download-and-go.md", names)

                wrapper = archive.extractfile("qtc-29.2/bin/qtcd")
                assert wrapper is not None
                self.assertIn("missing runtime libraries", wrapper.read().decode("utf-8"))

    def test_cuda_archive_names_match_release_platform_ids(self):
        self.assertEqual(
            self.module.archive_filename("0.33.0", "linux-x86_64-cuda12", None),
            "qtc-0.33.0-x86_64-linux-gnu-cuda12.tar.gz",
        )
        self.assertEqual(
            self.module.archive_filename("0.33.0", "linux-x86_64-cuda13", None),
            "qtc-0.33.0-x86_64-linux-gnu-cuda13.tar.gz",
        )

    def test_windows_archive_uses_zip_and_exe_names(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            source_root = self._build_source_root(root)
            qtcd, qtc_cli = self._write_binaries(root, windows=True)
            output_dir = root / "out"

            exit_code = self.module.main(
                [
                    "--output-dir",
                    str(output_dir),
                    "--version",
                    "29.2",
                    "--platform-id",
                    "windows-x86_64",
                    "--qtcd",
                    str(qtcd),
                    "--qtc-cli",
                    str(qtc_cli),
                    "--source-root",
                    str(source_root),
                ]
            )

            self.assertEqual(exit_code, 0)
            archive_path = output_dir / "qtc-29.2-x86_64-w64-mingw32.zip"
            self.assertTrue(archive_path.is_file())
            with zipfile.ZipFile(archive_path) as archive:
                names = set(archive.namelist())
            self.assertIn("qtc-29.2/bin/qtcd.exe", names)
            self.assertIn("qtc-29.2/bin/qtc-cli.exe", names)
            self.assertIn("qtc-29.2/contrib/faststart/qtc-agent-setup.py", names)

    def test_stage_release_tree_rejects_missing_support_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            source_root = root / "source-root"
            source_root.mkdir()
            qtcd, qtc_cli = self._write_binaries(root)

            with self.assertRaises(FileNotFoundError):
                self.module.stage_release_tree(
                    version="29.2",
                    platform_id="linux-x86_64",
                    qtcd_path=qtcd,
                    qtc_cli_path=qtc_cli,
                    qtc_util_path=None,
                    matmul_metallib_path=None,
                    oracle_metallib_path=None,
                    source_root=source_root,
                    temp_root=root / "temp",
                )

    def test_tarball_is_reproducible_with_source_date_epoch(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            source_root = self._build_source_root(root)
            qtcd, qtc_cli = self._write_binaries(root)
            output_a = root / "out-a"
            output_b = root / "out-b"

            original_epoch = os.environ.get("SOURCE_DATE_EPOCH")
            os.environ["SOURCE_DATE_EPOCH"] = "1712534400"
            try:
                self.module.main(
                    [
                        "--output-dir",
                        str(output_a),
                        "--version",
                        "29.2",
                        "--platform-id",
                        "linux-x86_64",
                        "--qtcd",
                        str(qtcd),
                        "--qtc-cli",
                        str(qtc_cli),
                        "--source-root",
                        str(source_root),
                    ]
                )
                self.module.main(
                    [
                        "--output-dir",
                        str(output_b),
                        "--version",
                        "29.2",
                        "--platform-id",
                        "linux-x86_64",
                        "--qtcd",
                        str(qtcd),
                        "--qtc-cli",
                        str(qtc_cli),
                        "--source-root",
                        str(source_root),
                    ]
                )
            finally:
                if original_epoch is None:
                    os.environ.pop("SOURCE_DATE_EPOCH", None)
                else:
                    os.environ["SOURCE_DATE_EPOCH"] = original_epoch

            archive_a = output_a / "qtc-29.2-x86_64-linux-gnu.tar.gz"
            archive_b = output_b / "qtc-29.2-x86_64-linux-gnu.tar.gz"
            self.assertEqual(archive_a.read_bytes(), archive_b.read_bytes())



class ShipGateTest(unittest.TestCase):
    """The ship gate itself, unstubbed: text stubs must be refused, never skipped."""

    def setUp(self):
        self.module = load_module()

    def test_verify_shipped_qtcd_refuses_unrecognized_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            qtcd.write_text("daemon\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "unrecognized file"):
                self.module.verify_shipped_qtcd(qtcd)

    def test_verify_shipped_cli_refuses_unrecognized_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            cli = pathlib.Path(tmpdir) / "qtc-cli"
            cli.write_text("cli\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "unrecognized file"):
                self.module.verify_shipped_cli(cli)

    def test_verify_shipped_qtcd_refuses_shell_wrapper(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            qtcd.write_text("#!/bin/sh\nexec libexec/qtcd.real\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "wrapper"):
                self.module.verify_shipped_qtcd(qtcd)


if __name__ == "__main__":
    unittest.main()
