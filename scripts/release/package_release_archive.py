#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Create a canonical QTC binary archive for one release platform.

The generated archive contains the release binaries plus the fast-start and
mining helper scripts needed for a download-and-go operator flow.
"""

from __future__ import annotations

import argparse
import gzip
import importlib.util
import json
import os
import shutil
import sys
import tarfile
import tempfile
from pathlib import Path
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SUPPORT_FILES_MANIFEST = Path(__file__).with_name("support_files.txt")


def load_support_files(manifest_path: Path = SUPPORT_FILES_MANIFEST) -> list[str]:
    support_files: list[str] = []
    for raw_line in manifest_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        support_files.append(line)
    return support_files


PLATFORM_CONFIGS = {
    "linux-x86_64": {
        "triple": "x86_64-linux-gnu",
        "archive_format": "tar.gz",
        "exe_suffix": "",
    },
    "linux-x86_64-cuda12": {
        "triple": "x86_64-linux-gnu-cuda12",
        "archive_format": "tar.gz",
        "exe_suffix": "",
    },
    "linux-x86_64-cuda13": {
        "triple": "x86_64-linux-gnu-cuda13",
        "archive_format": "tar.gz",
        "exe_suffix": "",
    },
    "linux-arm64": {
        "triple": "aarch64-linux-gnu",
        "archive_format": "tar.gz",
        "exe_suffix": "",
    },
    "windows-x86_64": {
        "triple": "x86_64-w64-mingw32",
        "archive_format": "zip",
        "exe_suffix": ".exe",
    },
    "macos-x86_64": {
        "triple": "x86_64-apple-darwin",
        "archive_format": "tar.gz",
        "exe_suffix": "",
    },
    "macos-arm64": {
        "triple": "arm64-apple-darwin",
        "archive_format": "tar.gz",
        "exe_suffix": "",
    },
}
SUPPORT_FILES = load_support_files()


def source_date_epoch() -> int:
    raw = os.environ.get("SOURCE_DATE_EPOCH")
    if raw is None or not raw.strip():
        return 0
    return int(raw.strip())


def wrapper_payload(binary_name: str, platform_id: str) -> str | None:
    if platform_id.startswith("linux-"):
        extra_hint = ""
        if binary_name == "qtcd":
            extra_hint = " libsqlite3-0 libzmq5"
        return f"""#!/bin/sh
set -eu
SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REAL="$SELF_DIR/../libexec/{binary_name}.real"
if [ ! -x "$REAL" ]; then
  echo "QTC packaged binary is missing: $REAL" >&2
  exit 127
fi
if command -v ldd >/dev/null 2>&1; then
  missing="$(ldd "$REAL" 2>/dev/null | awk '/=> not found/ {{print $1}}' | tr '\\n' ' ')"
  if [ -n "$missing" ]; then
    echo "QTC {binary_name} is missing runtime libraries: $missing" >&2
    echo "Ubuntu/Debian hint: sudo apt-get install libevent-2.1-7t64 libevent-core-2.1-7t64 libevent-extra-2.1-7t64 libevent-pthreads-2.1-7t64{extra_hint}" >&2
    echo "General hint: install the equivalent libevent, sqlite3, and zeromq runtime packages for your distribution." >&2
    echo "The packaged binary is located at: $REAL" >&2
    exit 127
  fi
fi
exec "$REAL" "$@"
"""
    if platform_id.startswith("macos-"):
        return f"""#!/bin/sh
set -eu
SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REAL="$SELF_DIR/../libexec/{binary_name}.real"
if [ ! -x "$REAL" ]; then
  echo "QTC packaged binary is missing: $REAL" >&2
  exit 127
fi
if command -v otool >/dev/null 2>&1; then
  missing=""
  while IFS= read -r dep; do
    case "$dep" in
      ""|@*|/System/*|/usr/lib/*) continue ;;
    esac
    if [ ! -e "$dep" ]; then
      missing="$missing $dep"
    fi
  done <<EOF
$(otool -L "$REAL" | awk 'NR>1 {{print $1}}')
EOF
  if [ -n "$missing" ]; then
    echo "QTC {binary_name} is missing runtime libraries:$missing" >&2
    echo "Install the matching Homebrew runtime packages with: brew install libevent sqlite zeromq" >&2
    echo "Apple Silicon default prefixes: /opt/homebrew/opt/libevent/lib /opt/homebrew/opt/sqlite/lib /opt/homebrew/opt/zeromq/lib" >&2
    echo "Intel default prefixes: /usr/local/opt/libevent/lib /usr/local/opt/sqlite/lib /usr/local/opt/zeromq/lib" >&2
    echo "The packaged binary is located at: $REAL" >&2
    exit 127
  fi
fi
exec "$REAL" "$@"
"""
    return None


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, help="Directory where the archive will be written.")
    parser.add_argument("--version", required=True, help="Release version string, for example 29.2.")
    parser.add_argument(
        "--platform-id",
        required=True,
        choices=sorted(PLATFORM_CONFIGS.keys()),
        help="Canonical release platform id.",
    )
    parser.add_argument("--qtcd", required=True, help="Path to the qtcd binary for this platform.")
    parser.add_argument("--qtc-cli", required=True, help="Path to the qtc-cli binary for this platform.")
    parser.add_argument(
        "--qtc-util",
        help="Path to the qtc-util binary for this platform. Defaults to a sibling of qtc-cli or qtcd.",
    )
    parser.add_argument("--matmul-metallib", help="Optional precompiled MatMul Metal library for macOS archives.")
    parser.add_argument("--oracle-metallib", help="Optional precompiled oracle Metal library for macOS archives.")
    parser.add_argument(
        "--source-root",
        default=str(ROOT),
        help="Repository root used to source helper scripts and docs (default: repo root).",
    )
    parser.add_argument(
        "--archive-name",
        help="Optional output filename override. Defaults to qtc-<version>-<target>.<ext>.",
    )
    return parser.parse_args(argv)


def ensure_input_file(path: Path, label: str) -> Path:
    if not path.is_file():
        raise FileNotFoundError(f"Missing {label}: {path}")
    return path


def resolve_qtc_util_path(explicit_path: Path | None, qtcd_path: Path, qtc_cli_path: Path, exe_suffix: str) -> Path:
    if explicit_path is not None:
        return ensure_input_file(explicit_path, "qtc-util binary")

    util_name = f"qtc-util{exe_suffix}"
    candidates = [
        qtc_cli_path.parent / util_name,
        qtcd_path.parent / util_name,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        "Missing qtc-util binary; release archives must ship qtc-util so PQ-signed auto-updates can verify. "
        "Pass --qtc-util or place it next to qtc-cli/qtcd."
    )



def _load_verify_module():
    script = Path(__file__).with_name("verify_release_qtcd.py")
    spec = importlib.util.spec_from_file_location("verify_release_qtcd", script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load {script}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_shipped_qtcd(qtcd_path: Path) -> None:
    """Refuse to package a qtcd that advertises ZMQ without linking it, loads
    Homebrew dylibs on macOS, or does not launch. The path must be the real
    ELF/Mach-O/PE (build-tree bin/qtcd or an already-staged libexec/qtcd.real);
    a packaged #!/bin/sh wrapper is not a binary and ldd/otool on it pass
    vacuously. An unrecognized file is FAIL, never a skip."""
    module = _load_verify_module()
    if module.is_shell_wrapper(qtcd_path):
        raise RuntimeError(f"{qtcd_path}: pass the real ELF/Mach-O, not the packaged bin/qtcd wrapper")
    if module.classify(qtcd_path) == "other":
        raise RuntimeError(
            f"{qtcd_path}: not an ELF, Mach-O, or PE binary; refusing to package an unrecognized file "
            "(a skipped gate is how a ZMQ-less daemon ships)"
        )
    module.verify_path_for_ship(qtcd_path)


def verify_shipped_cli(qtc_cli_path: Path) -> None:
    """Same portability bar for qtc-cli (no ZMQ or launch requirement)."""
    module = _load_verify_module()
    if module.is_shell_wrapper(qtc_cli_path):
        raise RuntimeError(f"{qtc_cli_path}: pass the real ELF/Mach-O, not the packaged bin/qtc-cli wrapper")
    if module.classify(qtc_cli_path) == "other":
        raise RuntimeError(f"{qtc_cli_path}: not an ELF, Mach-O, or PE binary; refusing to package an unrecognized file")
    module.verify_binary(qtc_cli_path)


def archive_filename(version: str, platform_id: str, override: str | None) -> str:
    if override:
        return override
    config = PLATFORM_CONFIGS[platform_id]
    suffix = ".zip" if config["archive_format"] == "zip" else ".tar.gz"
    return f"qtc-{version}-{config['triple']}{suffix}"


def stage_release_tree(
    *,
    version: str,
    platform_id: str,
    qtcd_path: Path,
    qtc_cli_path: Path,
    qtc_util_path: Path | None,
    matmul_metallib_path: Path | None,
    oracle_metallib_path: Path | None,
    source_root: Path,
    temp_root: Path,
) -> tuple[Path, list[str]]:
    config = PLATFORM_CONFIGS[platform_id]
    release_root = temp_root / f"qtc-{version}"
    included: list[str] = []

    bin_dir = release_root / "bin"
    libexec_dir = release_root / "libexec"
    bin_dir.mkdir(parents=True, exist_ok=True)

    verify_shipped_qtcd(ensure_input_file(qtcd_path, "qtcd binary"))
    verify_shipped_cli(ensure_input_file(qtc_cli_path, "qtc-cli binary"))
    binary_pairs = [
        (qtcd_path, f"qtcd{config['exe_suffix']}"),
        (qtc_cli_path, f"qtc-cli{config['exe_suffix']}"),
        (
            resolve_qtc_util_path(qtc_util_path, qtcd_path, qtc_cli_path, config["exe_suffix"]),
            f"qtc-util{config['exe_suffix']}",
        ),
    ]
    for source, dest_name in binary_pairs:
        wrapper = wrapper_payload(dest_name.removesuffix(config["exe_suffix"]), platform_id)
        if wrapper is None:
            destination = bin_dir / dest_name
            shutil.copy2(source, destination)
            included.append(str(destination.relative_to(release_root)))
            continue

        libexec_dir.mkdir(parents=True, exist_ok=True)
        real_destination = libexec_dir / f"{dest_name.removesuffix(config['exe_suffix'])}.real"
        shutil.copy2(source, real_destination)
        real_destination.chmod(0o755)
        included.append(str(real_destination.relative_to(release_root)))

        destination = bin_dir / dest_name
        destination.write_text(wrapper, encoding="utf-8")
        destination.chmod(0o755)
        included.append(str(destination.relative_to(release_root)))

    if platform_id.startswith("macos-"):
        metallib_inputs = [
            (matmul_metallib_path, "matmul_accel_kernels.metallib"),
            (oracle_metallib_path, "oracle_accel_kernels.metallib"),
        ]
        metal_dir = libexec_dir / "metal"
        for source_path, dest_name in metallib_inputs:
            if source_path is None:
                continue
            source = ensure_input_file(source_path, dest_name)
            metal_dir.mkdir(parents=True, exist_ok=True)
            destination = metal_dir / dest_name
            shutil.copy2(source, destination)
            destination.chmod(0o644)
            included.append(str(destination.relative_to(release_root)))

    for relative_path in SUPPORT_FILES:
        source = ensure_input_file(source_root / relative_path, relative_path)
        destination = release_root / relative_path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        included.append(str(destination.relative_to(release_root)))

    return release_root, sorted(included)


def normalized_tarinfo(tarinfo: tarfile.TarInfo, epoch: int) -> tarfile.TarInfo:
    tarinfo.uid = 0
    tarinfo.gid = 0
    tarinfo.uname = "root"
    tarinfo.gname = "root"
    tarinfo.mtime = epoch
    if tarinfo.isdir():
        tarinfo.mode = 0o755
    elif tarinfo.isfile():
        tarinfo.mode = 0o755 if (tarinfo.mode & 0o111) else 0o644
    return tarinfo


def write_tar_gz(archive_path: Path, release_root: Path) -> None:
    epoch = source_date_epoch()
    with archive_path.open("wb") as raw_handle:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw_handle, mtime=epoch, compresslevel=9) as gzip_handle:
            with tarfile.open(fileobj=gzip_handle, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in [release_root, *sorted(release_root.rglob("*"))]:
                    arcname = str(path.relative_to(release_root.parent))
                    if path.is_dir():
                        tarinfo = normalized_tarinfo(archive.gettarinfo(str(path), arcname), epoch)
                        archive.addfile(tarinfo)
                        continue
                    tarinfo = normalized_tarinfo(archive.gettarinfo(str(path), arcname), epoch)
                    with path.open("rb") as handle:
                        archive.addfile(tarinfo, handle)


def zip_info_for(path: Path, arcname: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(arcname)
    info.date_time = (1980, 1, 1, 0, 0, 0)
    info.compress_type = zipfile.ZIP_DEFLATED
    mode = 0o755 if (path.stat().st_mode & 0o111) else 0o644
    info.external_attr = mode << 16
    return info


def write_zip(archive_path: Path, release_root: Path) -> None:
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(release_root.rglob("*")):
            if not path.is_file():
                continue
            archive.writestr(zip_info_for(path, str(path.relative_to(release_root.parent))), path.read_bytes())


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    output_dir = Path(args.output_dir).expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    source_root = Path(args.source_root).expanduser().resolve()
    archive_path = output_dir / archive_filename(args.version, args.platform_id, args.archive_name)

    with tempfile.TemporaryDirectory(prefix="qtc-release-archive-") as temp_dir:
        temp_root = Path(temp_dir)
        release_root, included_paths = stage_release_tree(
            version=args.version,
            platform_id=args.platform_id,
            qtcd_path=Path(args.qtcd).expanduser().resolve(),
            qtc_cli_path=Path(args.qtc_cli).expanduser().resolve(),
            qtc_util_path=Path(args.qtc_util).expanduser().resolve() if args.qtc_util else None,
            matmul_metallib_path=Path(args.matmul_metallib).expanduser().resolve() if args.matmul_metallib else None,
            oracle_metallib_path=Path(args.oracle_metallib).expanduser().resolve() if args.oracle_metallib else None,
            source_root=source_root,
            temp_root=temp_root,
        )
        if PLATFORM_CONFIGS[args.platform_id]["archive_format"] == "zip":
            write_zip(archive_path, release_root)
        else:
            write_tar_gz(archive_path, release_root)

    json.dump(
        {
            "archive_path": str(archive_path),
            "archive_name": archive_path.name,
            "platform_id": args.platform_id,
            "archive_format": PLATFORM_CONFIGS[args.platform_id]["archive_format"],
            "included_paths": included_paths,
        },
        sys.stdout,
        indent=2,
    )
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
