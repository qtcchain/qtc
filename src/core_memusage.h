// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CORE_MEMUSAGE_H
#define BITCOIN_CORE_MEMUSAGE_H

#include <primitives/transaction.h>
#include <primitives/block.h>
#include <memusage.h>
#include <shielded/bundle.h>

#include <variant>

static inline size_t RecursiveDynamicUsage(const CScript& script) {
    return memusage::DynamicUsage(script);
}

static inline size_t RecursiveDynamicUsage(const COutPoint& out) {
    return 0;
}

static inline size_t RecursiveDynamicUsage(const CTxIn& in) {
    size_t mem = RecursiveDynamicUsage(in.scriptSig) + RecursiveDynamicUsage(in.prevout) + memusage::DynamicUsage(in.scriptWitness.stack);
    for (std::vector<std::vector<unsigned char> >::const_iterator it = in.scriptWitness.stack.begin(); it != in.scriptWitness.stack.end(); it++) {
         mem += memusage::DynamicUsage(*it);
    }
    return mem;
}

static inline size_t RecursiveDynamicUsage(const CTxOut& out) {
    return RecursiveDynamicUsage(out.scriptPubKey);
}

// Shielded bundle accounting. Nested structs are approximated by the sum of
// their dynamically allocated (vector) members; fixed-size arrays and scalars
// live inline and are covered by the owner's allocation.

static inline size_t RecursiveDynamicUsage(const smile2::CompactPublicAccount& account) {
    return memusage::DynamicUsage(account.public_key) +
           memusage::DynamicUsage(account.public_coin.t0) +
           memusage::DynamicUsage(account.public_coin.t_msg);
}

static inline size_t RecursiveDynamicUsage(const smile2::CompactPublicKeyData& key) {
    return memusage::DynamicUsage(key.public_key);
}

static inline size_t RecursiveDynamicUsage(const shielded::registry::ShieldedAccountRegistrySpendWitness& witness) {
    return memusage::DynamicUsage(witness.sibling_path);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::SpendDescription& spend) {
    return RecursiveDynamicUsage(spend.account_registry_proof);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::ConsumedAccountLeafSpend& spend) {
    return RecursiveDynamicUsage(spend.account_registry_proof);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::GenericOpaqueSpendRecord& spend) {
    return RecursiveDynamicUsage(spend.account_registry_proof);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::OutputDescription& output) {
    size_t mem = memusage::DynamicUsage(output.encrypted_note.ciphertext);
    if (output.smile_account) mem += RecursiveDynamicUsage(*output.smile_account);
    if (output.smile_public_key) mem += RecursiveDynamicUsage(*output.smile_public_key);
    return mem;
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::GenericOpaqueOutputRecord& output) {
    return memusage::DynamicUsage(output.encrypted_note.ciphertext) +
           RecursiveDynamicUsage(output.smile_account) +
           RecursiveDynamicUsage(output.smile_public_key);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::AddressLifecycleControl& control) {
    return memusage::DynamicUsage(control.subject_spending_pubkey) + memusage::DynamicUsage(control.signature);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::ProofShardDescriptor& shard) {
    return memusage::DynamicUsage(shard.proof_metadata);
}

static inline size_t RecursiveDynamicUsage(const CShieldedInput& input) {
    return memusage::DynamicUsage(input.ring_positions);
}

static inline size_t RecursiveDynamicUsage(const CShieldedOutput& output) {
    return memusage::DynamicUsage(output.encrypted_note.aead_ciphertext) + memusage::DynamicUsage(output.range_proof);
}

static inline size_t RecursiveDynamicUsage(const CViewGrant& grant) {
    return memusage::DynamicUsage(grant.encrypted_data);
}

/** Vector of elements that themselves own dynamic allocations (element overloads must be declared above). */
template <typename T>
static inline size_t RecursiveDynamicUsageOfVector(const std::vector<T>& v) {
    size_t mem = memusage::DynamicUsage(v);
    for (const T& item : v) {
        mem += RecursiveDynamicUsage(item);
    }
    return mem;
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::SendPayload& payload) {
    return RecursiveDynamicUsageOfVector(payload.spends) +
           RecursiveDynamicUsageOfVector(payload.outputs) +
           RecursiveDynamicUsageOfVector(payload.lifecycle_controls);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::SpendPathRecoveryPayload& payload) {
    return RecursiveDynamicUsageOfVector(payload.spends) + RecursiveDynamicUsageOfVector(payload.outputs);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::RecoveryExitPayload& payload) {
    return memusage::DynamicUsage(payload.spend_pubkey) +
           memusage::DynamicUsage(payload.ownership_sig) +
           memusage::DynamicUsage(payload.membership_proof);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::LifecyclePayload& payload) {
    return RecursiveDynamicUsageOfVector(payload.lifecycle_controls);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::IngressBatchPayload& payload) {
    return RecursiveDynamicUsageOfVector(payload.consumed_spends) +
           memusage::DynamicUsage(payload.ingress_leaves) +
           RecursiveDynamicUsageOfVector(payload.reserve_outputs);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::EgressBatchPayload& payload) {
    return RecursiveDynamicUsageOfVector(payload.outputs);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::RebalancePayload& payload) {
    return memusage::DynamicUsage(payload.reserve_deltas) +
           RecursiveDynamicUsageOfVector(payload.reserve_outputs) +
           memusage::DynamicUsage(payload.netting_manifest.domains);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::SettlementAnchorPayload& payload) {
    return memusage::DynamicUsage(payload.imported_claim_ids) +
           memusage::DynamicUsage(payload.imported_adapter_ids) +
           memusage::DynamicUsage(payload.proof_receipt_ids) +
           memusage::DynamicUsage(payload.batch_statement_digests) +
           memusage::DynamicUsage(payload.reserve_deltas);
}

static inline size_t RecursiveDynamicUsage(const shielded::v2::TransactionBundle& bundle) {
    return std::visit([](const auto& payload) { return RecursiveDynamicUsage(payload); }, bundle.payload) +
           RecursiveDynamicUsageOfVector(bundle.proof_shards) +
           memusage::DynamicUsage(bundle.output_chunks) +
           memusage::DynamicUsage(bundle.proof_payload);
}

static inline size_t RecursiveDynamicUsage(const CShieldedBundle& bundle) {
    size_t mem = RecursiveDynamicUsageOfVector(bundle.shielded_inputs) +
                 RecursiveDynamicUsageOfVector(bundle.shielded_outputs) +
                 RecursiveDynamicUsageOfVector(bundle.view_grants) +
                 memusage::DynamicUsage(bundle.proof);
    if (bundle.v2_bundle) mem += RecursiveDynamicUsage(*bundle.v2_bundle);
    return mem;
}

static inline size_t RecursiveDynamicUsage(const CTransaction& tx) {
    size_t mem = memusage::DynamicUsage(tx.vin) + memusage::DynamicUsage(tx.vout);
    for (std::vector<CTxIn>::const_iterator it = tx.vin.begin(); it != tx.vin.end(); it++) {
        mem += RecursiveDynamicUsage(*it);
    }
    for (std::vector<CTxOut>::const_iterator it = tx.vout.begin(); it != tx.vout.end(); it++) {
        mem += RecursiveDynamicUsage(*it);
    }
    if (tx.HasShieldedBundle()) {
        mem += RecursiveDynamicUsage(tx.GetShieldedBundle());
    }
    return mem;
}

static inline size_t RecursiveDynamicUsage(const CMutableTransaction& tx) {
    size_t mem = memusage::DynamicUsage(tx.vin) + memusage::DynamicUsage(tx.vout);
    for (std::vector<CTxIn>::const_iterator it = tx.vin.begin(); it != tx.vin.end(); it++) {
        mem += RecursiveDynamicUsage(*it);
    }
    for (std::vector<CTxOut>::const_iterator it = tx.vout.begin(); it != tx.vout.end(); it++) {
        mem += RecursiveDynamicUsage(*it);
    }
    if (tx.HasShieldedBundle()) {
        mem += RecursiveDynamicUsage(tx.GetShieldedBundle());
    }
    return mem;
}

static inline size_t RecursiveDynamicUsage(const CBlock& block) {
    size_t mem = memusage::DynamicUsage(block.vtx);
    for (const auto& tx : block.vtx) {
        mem += memusage::DynamicUsage(tx) + RecursiveDynamicUsage(*tx);
    }
    return mem;
}

static inline size_t RecursiveDynamicUsage(const CBlockLocator& locator) {
    return memusage::DynamicUsage(locator.vHave);
}

template<typename X>
static inline size_t RecursiveDynamicUsage(const std::shared_ptr<X>& p) {
    return p ? memusage::DynamicUsage(p) + RecursiveDynamicUsage(*p) : 0;
}

#endif // BITCOIN_CORE_MEMUSAGE_H
