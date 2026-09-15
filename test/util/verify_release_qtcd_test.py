#!/usr/bin/env python3
"""Fail-closed tests for scripts/release/verify_release_qtcd.py."""

from __future__ import annotations

import importlib.util
import io
import pathlib
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "release" / "verify_release_qtcd.py"
LEGACY_CPU = pathlib.Path.home() / ".cache" / "qtc-legacy" / (
    "qtc-0.1.0-linux-x86_64-cpu.tar.gz"
)


def load_module():
    spec = importlib.util.spec_from_file_location("verify_release_qtcd", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class VerifyReleaseQtcdTest(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_text_stub_is_other(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            path = pathlib.Path(tmpdir) / "qtcd"
            path.write_text("daemon\n", encoding="utf-8")
            self.assertEqual(self.mod.classify(path), "other")

    def test_legacy_03342_cpu_fails_closed(self) -> None:
        if not LEGACY_CPU.is_file():
            self.skipTest("0.1.0 cpu tarball not on this host")
        import tarfile

        with tarfile.open(LEGACY_CPU) as archive, tempfile.TemporaryDirectory() as tmpdir:
            archive.extract("qtcd", tmpdir)
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            self.assertEqual(self.mod.classify(qtcd), "elf")
            self.assertFalse(self.mod.has_enable_zmq_help(qtcd))
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.verify_qtcd(qtcd)
            self.assertIn("ENABLE_ZMQ help text", str(caught.exception))

    def test_macho_libzmq_dylib_is_rejected(self) -> None:
        name = b"/opt/homebrew/lib/libzmq.5.dylib\x00"
        name_off = 24
        cmdsize = (name_off + len(name) + 7) & ~7
        lc = struct.pack("<IIIIII", 0xC, cmdsize, name_off, 0, 0, 0) + name
        lc += b"\x00" * (cmdsize - len(lc))
        # 64-bit Mach-O header is 32 bytes (includes reserved).
        header = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 1, len(lc), 0, 0)
        macho = header + lc
        with tempfile.TemporaryDirectory() as tmpdir:
            path = pathlib.Path(tmpdir) / "qtcd"
            path.write_bytes(macho + b"Enable publish hash block")
            self.assertEqual(self.mod.classify(path), "macho")
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.verify_macos(path, require_zmq=True)
            self.assertIn("libzmq", str(caught.exception))

    def test_macho_homebrew_libevent_is_rejected(self) -> None:
        name = b"/opt/homebrew/opt/libevent/lib/libevent_core-2.1.7.dylib\x00"
        name_off = 24
        cmdsize = (name_off + len(name) + 7) & ~7
        lc = struct.pack("<IIIIII", 0xC, cmdsize, name_off, 0, 0, 0) + name
        lc += b"\x00" * (cmdsize - len(lc))
        header = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 1, len(lc), 0, 0)
        macho = header + lc
        with tempfile.TemporaryDirectory() as tmpdir:
            path = pathlib.Path(tmpdir) / "qtcd"
            path.write_bytes(macho + b"Enable publish hash block")
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.verify_macos(path, require_zmq=True)
            self.assertIn("Homebrew", str(caught.exception))
            self.assertIn("libevent", str(caught.exception))

    def test_cli_skips_zmq_help_but_rejects_homebrew(self) -> None:
        name = b"/opt/homebrew/opt/libomp/lib/libomp.dylib\x00"
        name_off = 24
        cmdsize = (name_off + len(name) + 7) & ~7
        lc = struct.pack("<IIIIII", 0xC, cmdsize, name_off, 0, 0, 0) + name
        lc += b"\x00" * (cmdsize - len(lc))
        header = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 1, len(lc), 0, 0)
        macho = header + lc
        with tempfile.TemporaryDirectory() as tmpdir:
            path = pathlib.Path(tmpdir) / "qtc-cli"
            path.write_bytes(macho)
            self.assertFalse(self.mod.requires_zmq(path))
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.verify_binary(path)
            self.assertIn("Homebrew", str(caught.exception))

    def _write_03342_shaped_elf(self, path: pathlib.Path) -> None:
        # ELF magic so classify() is linux; hidden-arg string without the
        # ENABLE_ZMQ help text — the v0.1.0 / v0.1.0 CPU tarball shape.
        path.write_bytes(b"\x7fELF" + b"\x00" * 12 + b"-zmqpubhashblock\x00")

    def test_cli_exits_nonzero_on_03342_shaped_elf(self) -> None:
        """The gate is decorative if FAIL prints and the process still exits 0."""
        with tempfile.TemporaryDirectory() as tmpdir:
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            self._write_03342_shaped_elf(qtcd)
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), str(qtcd)],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(
                proc.returncode, 0,
                f"FAIL must be a non-zero exit; stdout={proc.stdout!r} stderr={proc.stderr!r}",
            )
            self.assertIn(b"FAIL", proc.stderr)
            self.assertIn(b"ENABLE_ZMQ help text", proc.stderr)
            self.assertEqual(self.mod.main([str(qtcd)]), 1)

    def test_cli_mixed_qtcd_fail_cli_pass_still_exits_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            cli = pathlib.Path(tmpdir) / "qtc-cli"
            self._write_03342_shaped_elf(qtcd)
            # Linux qtc-cli is portability-only: an ELF with no ZMQ help is OK.
            cli.write_bytes(b"\x7fELF" + b"\x00" * 16)
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), str(qtcd), str(cli)],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stderr.decode())
            combined = proc.stdout + proc.stderr
            last = [ln for ln in combined.splitlines() if ln.strip()][-1]
            self.assertIn(b"FAIL", last)
            self.assertEqual(self.mod.main([str(qtcd), str(cli)]), 1)

    def test_cli_legacy_03342_cpu_tarball_exits_nonzero(self) -> None:
        if not LEGACY_CPU.is_file():
            self.skipTest("0.1.0 cpu tarball not on this host")
        import tarfile

        with tarfile.open(LEGACY_CPU) as archive, tempfile.TemporaryDirectory() as tmpdir:
            archive.extract("qtcd", tmpdir)
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), str(qtcd)],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertIn(b"FAIL", proc.stderr)
            self.assertIn(b"ENABLE_ZMQ help text", proc.stderr)

    def test_launch_requires_exit_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            failing = pathlib.Path(tmpdir) / "qtcd"
            failing.write_text("#!/bin/sh\necho missing libcublasLt.so.13 >&2\nexit 127\n", encoding="utf-8")
            failing.chmod(0o755)
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.verify_launch(failing)
            self.assertIn("exited 127", str(caught.exception))
            self.assertIn("libcublasLt", str(caught.exception))

            ok = pathlib.Path(tmpdir) / "qtcd.ok"
            ok.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            ok.chmod(0o755)
            self.mod.verify_launch(ok)


    def test_shell_wrapper_without_real_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            bindir = pathlib.Path(tmpdir) / "bin"
            bindir.mkdir()
            wrapper = bindir / "qtcd"
            wrapper.write_text("#!/bin/sh\nexec ../libexec/qtcd.real \"$@\"\n", encoding="utf-8")
            wrapper.chmod(0o755)
            self.assertTrue(self.mod.is_shell_wrapper(wrapper))
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.resolve_verify_target(wrapper)
            self.assertIn("libexec", str(caught.exception))
            self.assertIn("wrapper", str(caught.exception).lower())
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), str(wrapper)],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertIn(b"FAIL", proc.stderr)
            self.assertIn(b"wrapper", proc.stderr.lower())

    def test_shell_wrapper_resolves_to_libexec_real(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            bindir = root / "bin"
            libexec = root / "libexec"
            bindir.mkdir()
            libexec.mkdir()
            wrapper = bindir / "qtcd"
            wrapper.write_text("#!/bin/sh\nexec ../libexec/qtcd.real \"$@\"\n", encoding="utf-8")
            real = libexec / "qtcd.real"
            real.write_bytes(b"\x7fELF" + b"\x00" * 16)
            self.assertEqual(self.mod.resolve_verify_target(wrapper), real)
            self.assertTrue(self.mod.is_daemon(real))
            self.assertTrue(self.mod.requires_zmq(real))

    def test_is_daemon_recognizes_qtcd_real(self) -> None:
        self.assertTrue(self.mod.is_daemon(pathlib.Path("libexec/qtcd.real")))
        self.assertFalse(self.mod.is_daemon(pathlib.Path("libexec/qtc-cli.real")))
        self.assertFalse(self.mod.requires_zmq(pathlib.Path("libexec/qtc-cli.real")))

    def test_cli_refuses_unrecognized_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            path = pathlib.Path(tmpdir) / "qtcd"
            path.write_text("daemon\n", encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), str(path)],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertIn(b"FAIL", proc.stderr)
            self.assertIn(b"not an ELF", proc.stderr)
            self.assertEqual(self.mod.main([str(path)]), 1)

    def test_gate_fails_when_handed_qtcd_built_without_zmq(self) -> None:
        """A gate nobody has seen fail is not a gate. This is the 111/122 shape."""
        with tempfile.TemporaryDirectory() as tmpdir:
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            self._write_03342_shaped_elf(qtcd)
            self.assertEqual(self.mod.classify(qtcd), "elf")
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.verify_path_for_ship(qtcd)
            self.assertIn("ENABLE_ZMQ help text", str(caught.exception))
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), str(qtcd)],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertIn(b"FAIL", proc.stderr)
            self.assertEqual(self.mod.main([str(qtcd)]), 1)

    def test_gate_passes_with_statically_linked_libzmq(self) -> None:
        # Release binaries from the depends set carry libzmq inside the executable: ldd shows no
        # libzmq, but the library's internal symbol strings are present after stripping.
        with tempfile.TemporaryDirectory() as tmpdir:
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            qtcd.write_bytes(b"\x7fELF" + b"\x00" * 16 + self.mod.ENABLE_ZMQ_HELP + b"\x00int zmq::msg_t::init_buffer(const void*, size_t)\x00")
            with mock.patch.object(self.mod, "elf_needed_ldd", return_value=["libc.so.6"]):
                self.mod.verify_linux(qtcd)  # must not raise

    def test_gate_fails_when_help_present_but_libzmq_not_linked(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            qtcd = pathlib.Path(tmpdir) / "qtcd"
            qtcd.write_bytes(b"\x7fELF" + b"\x00" * 8 + b"Enable publish hash block")
            with mock.patch.object(self.mod, "elf_needed_ldd", return_value=["libc.so.6"]):
                with self.assertRaises(self.mod.VerifyError) as caught:
                    self.mod.verify_linux(qtcd)
                self.assertIn("libzmq", str(caught.exception))
                self.assertEqual(self.mod.main([str(qtcd)]), 1)

    def test_pe_without_zmq_help_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            qtcd = pathlib.Path(tmpdir) / "qtcd.exe"
            qtcd.write_bytes(b"MZ" + b"\x00" * 32 + b"-zmqpubhashblock\x00")
            self.assertEqual(self.mod.classify(qtcd), "pe")
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.verify_binary(qtcd)
            self.assertIn("ENABLE_ZMQ help text", str(caught.exception))

    def test_macho_without_zmq_help_fails(self) -> None:
        header = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 0, 0, 0, 0)
        with tempfile.TemporaryDirectory() as tmpdir:
            path = pathlib.Path(tmpdir) / "qtcd"
            path.write_bytes(header)
            self.assertEqual(self.mod.classify(path), "macho")
            with self.assertRaises(self.mod.VerifyError) as caught:
                self.mod.verify_macos(path, require_zmq=True)
            self.assertIn("ENABLE_ZMQ help text", str(caught.exception))

    def _write_tar_with_member(self, archive: pathlib.Path, member: str, payload: bytes) -> None:
        with tarfile.open(archive, "w:gz") as handle:
            info = tarfile.TarInfo(member)
            info.size = len(payload)
            handle.addfile(info, io.BytesIO(payload))

    def test_archive_gate_fails_on_qtcd_built_without_zmq(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            archive = pathlib.Path(tmpdir) / "qtc-0.1.0-x86_64-linux-gnu.tar.gz"
            payload = b"\x7fELF" + b"\x00" * 12 + b"-zmqpubhashblock\x00"
            self._write_tar_with_member(archive, "qtc-0.1.0/libexec/qtcd.real", payload)
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--archive", str(archive)],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertIn(b"FAIL", proc.stderr)
            self.assertIn(b"ENABLE_ZMQ help text", proc.stderr)
            with self.assertRaises(self.mod.VerifyError):
                self.mod.verify_archive(archive)
            self.assertEqual(self.mod.main(["--archive", str(archive)]), 1)

    def test_archive_gate_fails_on_unreadable_tarball(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            archive = pathlib.Path(tmpdir) / "qtc-0.1.0-linux-x86_64-cpu.tar.gz"
            archive.write_bytes(b"not a tarball")
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--archive", str(archive)],
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertIn(b"FAIL", proc.stderr)

    def test_cli_requires_binary_or_archive(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(SCRIPT)],
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"binary or --archive", proc.stderr)


if __name__ == "__main__":
    unittest.main()
