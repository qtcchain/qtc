"""Spec §11 Phase 3: Tier 2 script-guarded wallet with the policy co-signer and the owner recovery leaf."""
from __future__ import annotations

import pytest

from aws.spend import SpendError
from conftest import fund, policy_for

ATOMS = 100_000_000


@pytest.fixture(scope="module")
def owner_key(node):
    node.rpc("createwallet", "owner-cold")
    w = node.rpc.wallet("owner-cold")
    a1, a2 = w("getnewaddress"), w("getnewaddress")
    return {"rpc": w, "address": a1, "pubkey": [w("exportpqkey", a1)["key"], w("exportpqkey", a2)["key"]]}


def test_agent_tier2_cosign(node, funder, aws, owner, agent, owner_key):
    dest = funder["rpc"]("getnewaddress")
    pol = policy_for("t2", [dest], per_tx_atoms=5 * ATOMS, per_day_atoms=50 * ATOMS); pol["tier"] = 2
    pol["approval"]["require_above_atoms"] = 100 * ATOMS
    w = aws.wallet_create(owner, "t2", 2, owner_mldsa=owner_key["pubkey"], policy=pol, cooldown_blocks=5)
    wid = w["wallet_id"]
    assert w["tier"] == 2 and w["descriptor"].startswith("mr(multi_pq(2,") and "{" not in w["descriptor"]
    fund(node, funder, w["receive_address"], 4.0)
    assert aws.wallet_balance(agent, wid)["confirmed_atoms"] == 4 * ATOMS
    # the agent key alone cannot finalize: no single-signature leaf exists (primary leaf is the 2-of-2)
    aws.spend.cosigner = None
    with pytest.raises(SpendError) as e:
        aws.wallet_send(agent, wid, dest, 1 * ATOMS, "no cosigner")
    assert e.value.code == "cosigner_unavailable"
    from aws.cosigner import CosignerClient
    aws.spend.cosigner = CosignerClient(aws.cfg.cosigner_url, aws.keyring)
    r = aws.wallet_send(agent, wid, dest, 1 * ATOMS, "with cosigner")
    assert r["status"] == "broadcast"
    node.mine(1, funder["address"])
    assert aws.wallet_balance(agent, wid)["confirmed_atoms"] < 3 * ATOMS  # 4 - 1 - fee (+ change back to the tree)


def test_cosigner_rejects_bad_envelope(node, funder, aws, owner_key):
    from aws.cosigner import CosignerClient, make_envelope
    w = aws.store.wallet("agent-t2-tier2")
    cli = CosignerClient(aws.cfg.cosigner_url, aws.keyring)
    wr = aws.rpc.wallet(w["node_wallet"])
    dest = funder["rpc"]("getnewaddress")
    psbt = wr("walletcreatedfundedpsbt" if False else "walletcreatefundedpsbt", [], [{dest: 0.5}], 0,
              {"fee_rate": 2, "include_watching": True, "changeAddress": w["receive_address"]})["psbt"]
    # wrong secret -> rejected; right secret but destination not on the allowlist -> rejected by the cosigner's own policy copy
    bad = make_envelope("not-the-secret", "00" * 32, 1, 50_000_000, [dest])
    with pytest.raises(RuntimeError):
        cli.sign("t2", psbt, bad)
    from aws.spend import psbt_sha256
    env = make_envelope(aws.keyring.require("decision_secret"), psbt_sha256(psbt), 1, 50_000_000, [dest])
    with pytest.raises(RuntimeError):
        cli.sign("t2", psbt, env)


def test_tier2_backups_cover_both_key_wallets(aws):
    """Until the wallet can spend multi-leaf trees, owner recovery is through the encrypted archives of both wallets."""
    w = aws.store.wallet("agent-t2-tier2")
    names = [p.name for p in aws.cfg.backup_dir.iterdir()]
    assert any(n.startswith(w["node_wallet"]) for n in names) and any(n.startswith(w["key_wallet"]) for n in names)
    create = [e for e in aws.store.audit_export() if e["kind"] == "wallet.create" and e["wallet_id"] == w["wallet_id"]][0]
    assert create["payload"]["planned_recovery_tree"].startswith("mr(multi_pq(2,")
