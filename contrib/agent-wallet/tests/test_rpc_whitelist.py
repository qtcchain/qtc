"""Spec §9/§10.7: the AWS RPC user is confined by rpcauth + rpcwhitelist and still completes the Tier 1 lifecycle."""
from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from aws.config import Config
from aws.rpc import Rpc, RpcError
from aws.service import AgentWalletService, Auth
from conftest import BIN, Node, policy_for

ATOMS = 100_000_000
WHITELIST = [l.split("=", 1)[1] for l in (Path(__file__).resolve().parents[1] / "rpcwhitelist.conf").read_text().splitlines()
             if l.startswith("rpcwhitelist=")][0]
ALLOWED = set(WHITELIST.split(":", 1)[1].split(","))


def _rpcauth(user: str, password: str) -> str:
    script = BIN.parents[1] / "share" / "rpcauth" / "rpcauth.py"
    if not script.exists():
        script = Path.home() / "Documents" / "qtc" / "qtc" / "share" / "rpcauth" / "rpcauth.py"
    out = subprocess.run(["python3", "-I", str(script), user, password], check=True, capture_output=True, text=True).stdout
    return re.search(r"rpcauth=(\S+)", out).group(1)


@pytest.fixture(scope="module")
def wl_node():
    d = Path(tempfile.mkdtemp(prefix="aws-wl-"))
    pw = "test-only-password"  # regtest, in-process, never reused
    n = Node(d, extra=[f"-rpcauth={_rpcauth('aws', pw)}", "-rpcwhitelistdefault=0", f"-rpcwhitelist={WHITELIST}", "-txindex=1"])
    n.aws_rpc = Rpc(n.url, None, "aws", pw)  # type: ignore[attr-defined]
    yield n
    n.stop(); shutil.rmtree(d, ignore_errors=True)


def test_whitelist_denies_private_material(wl_node):
    for method in ("dumpprivkey", "exportwalletbundle", "gethdkeys", "stop", "generatetoaddress", "sethdseed", "importprivkey", "help"):
        assert method not in ALLOWED
        with pytest.raises(RpcError) as e:
            wl_node.aws_rpc(method)
        assert e.value.code == -403, str(e.value)
    assert wl_node.aws_rpc("getblockcount") >= 0  # whitelisted calls still work for the same user


def test_whitelist_covers_every_method_the_service_calls(wl_node):
    known = set(re.findall(r"^([a-z_]+)", wl_node.rpc("help"), re.M))  # operator identity lists the real RPC names
    src = "".join(p.read_text() for p in (Path(__file__).resolve().parents[1] / "aws").glob("*.py"))
    called = set(re.findall(r'\("([a-z_]+)"', src)) & known
    missing = sorted(called - ALLOWED)
    assert not missing, f"service calls RPCs the whitelist denies: {missing}"
    for private in ("dumpprivkey", "exportwalletbundle", "gethdkeys", "listdescriptors"):
        assert private not in ALLOWED


def test_tier1_lifecycle_under_whitelist(wl_node, tmp_path):
    # funding uses the cookie (operator) identity; the service only ever has the whitelisted user
    wl_node.rpc("createwallet", "funder-wl"); f = wl_node.rpc.wallet("funder-wl"); faddr = f("getnewaddress"); wl_node.mine(110, faddr)
    cfg = Config(data_dir=tmp_path / "aws", rpc_url=wl_node.url, cookie_path="", rpc_user="aws", rpc_password="test-only-password",
                 chain="regtest", cosigner_url="", confirmation_depth=1)
    svc = AgentWalletService(cfg)
    owner = Auth(scopes={"wallet:admin"}, actor="owner"); agent = Auth(scopes={"wallet:read", "wallet:spend", "market:read"}, actor="agent")
    dest = f("getnewaddress")
    w = svc.wallet_create(owner, "wl", 1, policy=policy_for("wl", [dest], per_tx_atoms=2 * ATOMS, per_day_atoms=10 * ATOMS))
    f("sendtoaddress", w["receive_address"], 3.0); wl_node.mine(1, faddr)
    assert svc.wallet_balance(agent, w["wallet_id"])["confirmed_atoms"] == 3 * ATOMS
    r = svc.wallet_send(agent, w["wallet_id"], dest, 1 * ATOMS, "whitelisted spend")
    assert r["status"] == "broadcast"
    wl_node.mine(1, faddr)
    assert svc.wallet_send_status(agent, w["wallet_id"], r["psbt_sha256"])["status"] == "confirmed"
    ok, n = svc.store.audit_verify(); assert ok and n > 0
