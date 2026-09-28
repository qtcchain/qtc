#!/usr/bin/env python3

import hashlib
import importlib.util
import json
import os
import shutil
import sys
import subprocess
import tempfile
from pathlib import Path


def main():
    """Tests ordered roughly from faster to slower."""
    test_local_bin()

    if not load_verify_module().HOSTS:
        print("- skipping pub tests: no release hosts configured in verify.py")
        return
    test_pub()


def test_pub():
    expect_code(run_verify("", "pub", '0.32'), 4, "Nonexistent version should fail")
    expect_code(run_verify("", "pub", '0.32.awefa.12f9h'), 11, "Malformed version should fail")
    expect_code(run_verify('--min-good-sigs 20', "pub", "22.0"), 9, "--min-good-sigs 20 should fail")

    print("- testing verification (22.0-x86_64-linux-gnu.tar.gz)", flush=True)
    _220_x86_64_linux_gnu = run_verify("--json", "pub", "22.0-x86_64-linux-gnu.tar.gz")
    try:
        result = json.loads(_220_x86_64_linux_gnu.stdout.decode())
    except Exception:
        print("failed on 22.0-x86_64-linux-gnu.tar.gz --json:")
        print_process_failure(_220_x86_64_linux_gnu)
        raise

    expect_code(_220_x86_64_linux_gnu, 0, "22.0-x86_64-linux-gnu.tar.gz should succeed")
    v = result['verified_binaries']
    assert result['good_trusted_sigs']
    assert len(v) == 1
    assert v['bitcoin-22.0-x86_64-linux-gnu.tar.gz'] == '59ebd25dd82a51638b7a6bb914586201e67db67b919b2a1ff08925a7936d1b16'

    print("- testing verification (22.0)", flush=True)
    _220 = run_verify("--json", "pub", "22.0")
    try:
        result = json.loads(_220.stdout.decode())
    except Exception:
        print("failed on 22.0 --json:")
        print_process_failure(_220)
        raise

    expect_code(_220, 0, "22.0 should succeed")
    v = result['verified_binaries']
    assert result['good_trusted_sigs']
    assert v['bitcoin-22.0-aarch64-linux-gnu.tar.gz'] == 'ac718fed08570a81b3587587872ad85a25173afa5f9fbbd0c03ba4d1714cfa3e'
    assert v['bitcoin-22.0-osx64.tar.gz'] == '2744d199c3343b2d94faffdfb2c94d75a630ba27301a70e47b0ad30a7e0155e9'
    assert v['bitcoin-22.0-x86_64-linux-gnu.tar.gz'] == '59ebd25dd82a51638b7a6bb914586201e67db67b919b2a1ff08925a7936d1b16'


RELEASE = "qtc-0.1.0-rc8"
# Release layout: SHA256SUMS lists paths relative to itself, with a directory prefix.
RELEASE_FILES = [
    f"aarch64-linux-gnu/{RELEASE}-aarch64-linux-gnu.tar.gz",
    f"x86_64-linux-gnu/{RELEASE}-x86_64-linux-gnu.tar.gz",
    f"apple-darwin-signed/{RELEASE}-arm64-apple-darwin.zip",
    # Same basename under two directories: only addressable by relative path.
    f"dup-a/{RELEASE}-dup.tar.gz",
    f"dup-b/{RELEASE}-dup.tar.gz",
]


def test_local_bin():
    """Exercise `bin` offline against a throwaway signing key and release tree."""
    if shutil.which("gpg") is None:
        print("- skipping bin tests: gpg not found")
        return

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        signer_home, verifier_home = tmp / "signer-gnupg", tmp / "verifier-gnupg"
        for home in (signer_home, verifier_home):
            home.mkdir(mode=0o700)
        release, downloads = tmp / "release", tmp / "downloads"
        downloads.mkdir()

        hashes = {}
        for i, rel in enumerate(RELEASE_FILES):
            f = release / rel
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(f"{rel} payload {i}\n".encode())
            hashes[rel] = hashlib.sha256(f.read_bytes()).hexdigest()
        sums = release / "SHA256SUMS"
        sums.write_text("".join(f"{hashes[rel]}  {rel}\n" for rel in RELEASE_FILES))

        # Sign with a key that is ultimately trusted only in the signer's keyring; the
        # verifier imports the public key without trust, so trust must come from
        # --trusted-keys.
        gpg(signer_home, "--quick-gen-key", "QTC Test Signer <signer@example.invalid>", "ed25519", "sign", "never")
        fpr = next(line.split(":")[9] for line in gpg(signer_home, "--with-colons", "--list-keys").splitlines()
                   if line.startswith("fpr:"))
        gpg(signer_home, "--detach-sign", "--armor", "--output", str(sums) + ".asc", str(sums))
        pubkey = tmp / "signer.asc"
        gpg(signer_home, "--export", "--armor", "--output", str(pubkey))
        gpg(verifier_home, "--import", str(pubkey))
        env = dict(os.environ, GNUPGHOME=str(verifier_home))

        def run_bin(global_args, *binaries):
            files = " ".join(f"'{b}'" for b in binaries)
            return run_verify(f"--min-good-sigs 1 {global_args}", "bin",
                              f"--sums-sig-file '{sums}.asc' '{sums}' {files}", env=env)

        print("- testing bin (relative mode, directory-prefixed SHA256SUMS)", flush=True)
        result = expect_json(run_bin("--json"), 0, "relative mode should verify every listed file")
        assert result['verified_binaries'] == {str(release / rel): hashes[rel] for rel in RELEASE_FILES}
        assert result['missing_binaries'] == []
        assert not result['good_trusted_sigs'] and len(result['good_untrusted_sigs']) == 1

        print("- testing bin (explicit binaries matched by basename)", flush=True)
        flat = []
        for rel in RELEASE_FILES[:3]:
            shutil.copy(release / rel, downloads / Path(rel).name)
            flat.append(downloads / Path(rel).name)
        result = expect_json(run_bin("--json", *flat), 0,
                             "explicit binaries should match directory-prefixed entries by basename")
        assert result['verified_binaries'] == {str(f): hashes[rel] for f, rel in zip(flat, RELEASE_FILES)}

        print("- testing bin (explicit binaries matched by relative path)", flush=True)
        dup_a, dup_b = (release / rel for rel in RELEASE_FILES[3:5])
        result = expect_json(run_bin("--json", dup_a, dup_b), 0,
                             "a shared basename should resolve by path relative to SHA256SUMS")
        assert result['verified_binaries'] == {str(dup_a): hashes[RELEASE_FILES[3]],
                                               str(dup_b): hashes[RELEASE_FILES[4]]}
        rel_run = subprocess.run(
            [sys.executable, str(verify_path()), "--min-good-sigs", "1", "--json", "bin",
             "SHA256SUMS", RELEASE_FILES[3]],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=release, env=env)
        result = expect_json(rel_run, 0, "a cwd-relative path should match its SHA256SUMS entry")
        assert result['verified_binaries'] == {RELEASE_FILES[3]: hashes[RELEASE_FILES[3]]}

        shutil.copy(dup_a, downloads / dup_a.name)
        expect_code(run_bin("", downloads / dup_a.name), 7,
                    "a basename matching several SHA256SUMS entries should be rejected as ambiguous")
        expect_code(run_bin("", flat[0], release / RELEASE_FILES[0]), 7,
                    "two binaries matching the same SHA256SUMS entry should be rejected")
        unlisted = downloads / f"{RELEASE}-riscv64-linux-gnu.tar.gz"
        unlisted.write_bytes(b"not in SHA256SUMS\n")
        expect_code(run_bin("", flat[0], unlisted), 7, "a binary not in SHA256SUMS should be rejected")
        flat[1].write_bytes(b"tampered\n")
        expect_code(run_bin("", flat[1]), 1, "a binary with the wrong hash should fail integrity")

        print("- testing bin --trusted-keys", flush=True)
        for spec, msg in [
            (fpr, "a full fingerprint"),
            (fpr.lower(), "a lowercase fingerprint"),
            (f"0x{fpr[-16:]}", "a 0x-prefixed long key id"),
            (" ".join(fpr[i:i + 4] for i in range(0, 40, 4)), "a space-grouped fingerprint"),
            (f"{'0' * 40},{fpr}", "a list containing the fingerprint"),
        ]:
            result = expect_json(run_bin(f"--json --trusted-keys '{spec}'", flat[0]), 0,
                                 f"--trusted-keys with {msg} should succeed")
            assert len(result['good_trusted_sigs']) == 1 and not result['good_untrusted_sigs'], msg
        for spec, msg in [
            ("0" * 40, "an unrelated fingerprint"),
            (fpr[-8:], "a short key id"),
        ]:
            result = expect_json(run_bin(f"--json --trusted-keys '{spec}'", flat[0]), 0,
                                 f"--trusted-keys with {msg} should succeed")
            assert not result['good_trusted_sigs'] and len(result['good_untrusted_sigs']) == 1, msg

        print("- testing bin --trusted-keys (signature by a signing subkey)", flush=True)
        gpg(signer_home, "--quick-add-key", fpr, "ed25519", "sign", "never")
        subkey_fpr = [line.split(":")[9] for line in gpg(signer_home, "--with-colons", "--list-keys").splitlines()
                      if line.startswith("fpr:")][-1]
        assert subkey_fpr != fpr
        gpg(signer_home, "--local-user", f"{subkey_fpr}!", "--detach-sign", "--armor",
            "--output", str(sums) + ".asc", str(sums))
        gpg(signer_home, "--export", "--armor", "--output", str(pubkey))
        gpg(verifier_home, "--import", str(pubkey))
        for spec, msg in [
            (fpr, "the primary fingerprint"),
            (subkey_fpr, "the subkey fingerprint"),
            (subkey_fpr[-16:], "the subkey long key id"),
        ]:
            result = expect_json(run_bin(f"--json --trusted-keys {spec}", flat[0]), 0,
                                 f"--trusted-keys with {msg} should trust a subkey signature")
            assert len(result['good_trusted_sigs']) == 1 and not result['good_untrusted_sigs'], msg


def gpg(home: Path, *args: str) -> str:
    completed = subprocess.run(
        ["gpg", "--homedir", str(home), "--batch", "--yes", "--pinentry-mode", "loopback",
         "--passphrase", "", *args],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode != 0:
        print_process_failure(completed)
        raise RuntimeError(f"gpg {' '.join(args)} failed")
    return completed.stdout.decode()


def verify_path() -> Path:
    maybe_here = Path.cwd() / 'verify.py'
    return maybe_here if maybe_here.exists() else Path.cwd() / 'contrib' / 'verify-binaries' / 'verify.py'


def load_verify_module():
    spec = importlib.util.spec_from_file_location("verify", verify_path())
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_verify(global_args: str, command: str, command_args: str, env=None) -> subprocess.CompletedProcess:
    path = verify_path()

    if command == "pub":
        command += " --cleanup"

    return subprocess.run(
        f"{path} {global_args} {command} {command_args}",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True, env=env)


def expect_json(completed: subprocess.CompletedProcess, expected_code: int, msg: str):
    expect_code(completed, expected_code, msg)
    try:
        return json.loads(completed.stdout.decode())
    except Exception:
        print(f"{msg!r}: output is not JSON")
        print_process_failure(completed)
        raise


def expect_code(completed: subprocess.CompletedProcess, expected_code: int, msg: str):
    if completed.returncode != expected_code:
        print(f"{msg!r} failed: got code {completed.returncode}, expected {expected_code}")
        print_process_failure(completed)
        sys.exit(1)
    else:
        print(f"✓ {msg!r} passed")


def print_process_failure(completed: subprocess.CompletedProcess):
    print(f"stdout:\n{completed.stdout.decode()}")
    print(f"stderr:\n{completed.stderr.decode()}")


if __name__ == '__main__':
    main()
