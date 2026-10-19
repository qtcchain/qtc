"""Agent Wallet Service (AWS) — reference implementation of docs/qtc-agent-wallet-spec.md.

A small daemon between an AI agent and a local qtcd: it owns the keys, enforces the owner's policy,
keeps a hash-chained audit log and exposes the spec's tool surface over MCP and REST. The agent
never sees a secret; every spend goes through policy evaluation before the wallet signs.
"""
__version__ = "0.1.0"
