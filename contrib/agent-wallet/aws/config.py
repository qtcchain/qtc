"""Service configuration and the local keyring.

The keyring is a 0600 JSON file holding wallet passphrases, the owner's admin secret (signs policy
documents), the decision secret shared with the Tier 2 co-signer, and the backup archive passphrase.
The spec asks for an OS keychain or HSM; this reference implementation keeps the same boundary
(secrets never reach the agent, logs, environment or command lines) with a file keyring.
"""
from __future__ import annotations

import json
import os
import secrets
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class Config:
    data_dir: Path
    rpc_url: str = "http://127.0.0.1:28790"
    cookie_path: str = ""
    rpc_user: str | None = None
    rpc_password: str | None = None
    listen: str = "127.0.0.1:8792"
    cosigner_url: str = "http://127.0.0.1:8793"
    cosigner_listen: str = "127.0.0.1:8793"
    explorer_url: str = ""          # optional public explorer for /fees and cross-checks
    chain: str = "regtest"
    confirmation_depth: int = 6
    approval_webhook: str = ""      # optional URL that receives decoded PSBTs needing approval
    extra: dict[str, Any] = field(default_factory=dict)

    @property
    def keyring_path(self) -> Path:
        return self.data_dir / "keyring.json"

    @property
    def db_path(self) -> Path:
        return self.data_dir / "aws.sqlite3"

    @property
    def policy_dir(self) -> Path:
        return self.data_dir / "policies"

    @property
    def backup_dir(self) -> Path:
        return self.data_dir / "backups"

    @staticmethod
    def load(path: str | Path) -> "Config":
        raw = json.loads(Path(path).read_text())
        data_dir = Path(raw.pop("data_dir")).expanduser()
        known = {k: raw.pop(k) for k in list(raw) if k in Config.__dataclass_fields__}
        cfg = Config(data_dir=data_dir, **known)
        cfg.extra = raw
        return cfg

    def save(self, path: str | Path) -> None:
        d = {k: (str(v) if isinstance(v, Path) else v) for k, v in self.__dict__.items() if k != "extra"}
        d.update(self.extra)
        Path(path).write_text(json.dumps(d, indent=2) + "\n")
        os.chmod(path, 0o600)


class Keyring:
    """0600 JSON file of named secrets. Values are never logged or returned by any tool."""

    def __init__(self, path: Path):
        self.path = path
        self._d: dict[str, str] = {}
        self._mtime = 0.0
        self._reload()

    def _reload(self) -> None:
        """Pick up secrets written by another process (e.g. the owner CLI creating a wallet while the REST service runs)."""
        try:
            st = self.path.stat()
        except FileNotFoundError:
            return
        if st.st_mtime_ns != self._mtime:
            self._d = json.loads(self.path.read_text())
            self._mtime = st.st_mtime_ns

    def _flush(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(self._d, f, indent=2)
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)
        os.chmod(self.path, 0o600)
        self._mtime = self.path.stat().st_mtime_ns

    def get(self, name: str) -> str | None:
        self._reload()
        return self._d.get(name)

    def require(self, name: str) -> str:
        self._reload()
        v = self._d.get(name)
        if v is None:
            raise KeyError(f"keyring has no secret named {name!r}")
        return v

    def set(self, name: str, value: str) -> None:
        self._reload()
        self._d[name] = value
        self._flush()

    def ensure(self, name: str, nbytes: int = 32) -> str:
        self._reload()
        if name not in self._d:
            self.set(name, secrets.token_hex(nbytes))
        return self._d[name]

    def names(self) -> list[str]:
        self._reload()
        return sorted(self._d)
