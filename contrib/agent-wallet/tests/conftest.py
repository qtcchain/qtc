"""Regtest harness: a fresh qtcd per test session, mined funds, the AWS and the Tier 2 co-signer in-process."""
from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from aws.config import Config, Keyring  # noqa: E402
from aws.cosigner import CosignerService, serve as serve_cosigner  # noqa: E402
from aws.policy import PolicyEngine  # noqa: E402
from aws.rpc import Rpc  # noqa: E402
from aws.service import AgentWalletService, Auth  # noqa: E402
from aws.store import Store  # noqa: E402

def _default_bin() -> Path:
    for cand in (ROOT.parents[2] / "build" / "bin", Path.home() / "Documents" / "qtc" / "qtc" / "build" / "bin"):
        if (cand / "qtcd").exists():
            return cand
    return ROOT.parents[2] / "build" / "bin"


BIN = Path(os.environ.get("QTC_BIN", str(_default_bin())))


def free_port() -> int:
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


class Node:
    def __init__(self, datadir: Path, extra: list[str] | None = None):
        self.datadir = datadir; self.rpcport = free_port(); self.port = free_port()
        self.proc = subprocess.Popen([str(BIN / "qtcd"), "-regtest", f"-datadir={datadir}", f"-rpcport={self.rpcport}", f"-port={self.port}",
                                      "-fallbackfee=0.0001", "-listen=0", "-printtoconsole=0", "-server=1"] + (extra or []),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.cookie = datadir / "regtest" / ".cookie"
        self.url = f"http://127.0.0.1:{self.rpcport}"
        for _ in range(120):
            if self.cookie.exists():
                try:
                    Rpc(self.url, str(self.cookie))("getblockcount"); break
                except Exception:
                    pass
            time.sleep(0.25)
        else:
            raise RuntimeError("qtcd did not start")
        self.rpc = Rpc(self.url, str(self.cookie))

    def mine(self, n: int, addr: str) -> None:
        self.rpc("generatetoaddress", n, addr)

    def stop(self) -> None:
        try:
            self.rpc("stop")
        except Exception:
            pass
        try:
            self.proc.wait(timeout=60)
        except Exception:
            self.proc.kill()


@pytest.fixture(scope="session")
def node():
    d = Path(tempfile.mkdtemp(prefix="aws-regtest-"))
    n = Node(d)
    yield n
    n.stop(); shutil.rmtree(d, ignore_errors=True)


@pytest.fixture(scope="session")
def funder(node):
    """A miner wallet that funds agent wallets."""
    node.rpc("createwallet", "funder")
    w = node.rpc.wallet("funder")
    addr = w("getnewaddress")
    node.mine(120, addr)  # coinbase maturity + spendable balance
    return {"rpc": w, "address": addr}


@pytest.fixture(scope="session")
def aws(node, tmp_path_factory):
    data = tmp_path_factory.mktemp("aws-data")
    cs_port = free_port()
    cfg = Config(data_dir=data, rpc_url=node.url, cookie_path=str(node.cookie), chain="regtest",
                 cosigner_url=f"http://127.0.0.1:{cs_port}", cosigner_listen=f"127.0.0.1:{cs_port}", confirmation_depth=2)
    svc = AgentWalletService(cfg)
    # co-signer in a thread with its own keyring/store, sharing only the two secrets it needs
    cs_dir = data / "cosigner"; cs_dir.mkdir()
    kr = Keyring(cs_dir / "keyring.json")
    for name in ("decision_secret", "admin_secret"):
        kr.set(name, svc.keyring.require(name))
    cs_store = Store(cs_dir / "cosigner.sqlite3")
    cs = CosignerService(cfg, Rpc(node.url, str(node.cookie)), kr, cs_store, PolicyEngine(cfg.policy_dir, kr.require("admin_secret"), cs_store))
    cs.store.wallet = svc.store.wallet  # type: ignore[method-assign]
    srv = serve_cosigner(cs, cfg.cosigner_listen)
    t = threading.Thread(target=srv.serve_forever, daemon=True); t.start()
    yield svc
    srv.shutdown()


@pytest.fixture
def owner():
    return Auth(scopes={"wallet:admin"}, actor="owner")


@pytest.fixture
def agent():
    return Auth(scopes={"wallet:read", "wallet:spend", "market:read", "trade:quote", "trade:read"}, actor="agent")


def fund(node, funder, address: str, qtc: float, confs: int = 1) -> str:
    txid = funder["rpc"]("sendtoaddress", address, qtc)
    node.mine(confs, funder["address"])
    return txid


def policy_for(agent_id: str, allow: list[str], **limits) -> dict:
    from aws.policy import DEFAULT_POLICY
    doc = json.loads(json.dumps(DEFAULT_POLICY))
    doc["agent_id"] = agent_id
    doc["destinations"]["allow"] = allow
    doc["limits"].update(limits)
    return doc
