// Package indexer follows the node's active chain and keeps the Bleve store
// consistent with it, including reorgs. It polls RPC; ZMQ can be added later
// as an optimisation without changing the store.
package indexer

import (
	"context"
	"errors"
	"fmt"
	"log"
	"sync"
	"time"

	"qtc-explorer/internal/rpc"
	"qtc-explorer/internal/store"
)

// Params are the consensus values needed to compute the base subsidy.
type Params struct {
	InitialSubsidy  int64 // atoms
	HalvingInterval int64
	MaxReorgDepth   int64 // deeper reorgs stop the indexer and require operator action
}

func DefaultParams() Params {
	return Params{InitialSubsidy: 50 * 1e8, HalvingInterval: 210_000, MaxReorgDepth: 100}
}

func (p Params) BaseSubsidy(height int64) int64 {
	if p.HalvingInterval <= 0 {
		return 0
	}
	h := height / p.HalvingInterval
	if h >= 64 {
		return 0
	}
	return p.InitialSubsidy >> uint(h)
}

// MempoolTx is the explorer's view of an unconfirmed transaction.
type MempoolTx struct {
	Txid      string `json:"txid"`
	Size      int64  `json:"size"`
	Fee       int64  `json:"fee"`
	FirstSeen int64  `json:"first_seen"`
	Ancestors int64  `json:"ancestors"`
}

type Indexer struct {
	rpc    *rpc.Client
	st     *store.Store
	params Params
	poll   time.Duration

	mu       sync.RWMutex
	mempool  map[string]MempoolTx
	nodeTip  int64
	lastErr  string
	lastSync time.Time
	halted   bool
}

func New(c *rpc.Client, s *store.Store, p Params, poll time.Duration) *Indexer {
	return &Indexer{rpc: c, st: s, params: p, poll: poll, mempool: map[string]MempoolTx{}}
}

// Status is exposed on /status and /api/v1/status.
type Status struct {
	IndexedHeight int64     `json:"indexed_height"`
	IndexedHash   string    `json:"indexed_hash"`
	NodeHeight    int64     `json:"node_height"`
	Lag           int64     `json:"lag"`
	MempoolSize   int       `json:"mempool_size"`
	Reorgs        int64     `json:"reorgs"`
	LastReorg     int64     `json:"last_reorg_depth"`
	LastSync      time.Time `json:"last_sync"`
	LastError     string    `json:"last_error,omitempty"`
	Halted        bool      `json:"halted"`
	Docs          uint64    `json:"documents"`
}

func (ix *Indexer) Status() Status {
	st, _ := ix.st.GetState()
	docs, _ := ix.st.DocCount()
	ix.mu.RLock()
	defer ix.mu.RUnlock()
	return Status{
		IndexedHeight: st.Height, IndexedHash: st.Hash, NodeHeight: ix.nodeTip,
		Lag: ix.nodeTip - st.Height, MempoolSize: len(ix.mempool), Reorgs: st.Reorgs,
		LastReorg: st.LastReorg, LastSync: ix.lastSync, LastError: ix.lastErr, Halted: ix.halted, Docs: docs,
	}
}

func (ix *Indexer) Mempool() []MempoolTx {
	ix.mu.RLock()
	defer ix.mu.RUnlock()
	out := make([]MempoolTx, 0, len(ix.mempool))
	for _, m := range ix.mempool {
		out = append(out, m)
	}
	return out
}

func (ix *Indexer) MempoolTx(txid string) (MempoolTx, bool) {
	ix.mu.RLock()
	defer ix.mu.RUnlock()
	m, ok := ix.mempool[txid]
	return m, ok
}

// Run polls until ctx is cancelled.
func (ix *Indexer) Run(ctx context.Context) {
	t := time.NewTicker(ix.poll)
	defer t.Stop()
	for {
		if err := ix.step(ctx); err != nil && !errors.Is(err, context.Canceled) {
			log.Printf("indexer: %v", err)
			ix.mu.Lock()
			ix.lastErr = err.Error()
			ix.mu.Unlock()
		}
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
	}
}

func (ix *Indexer) step(ctx context.Context) error {
	ix.mu.RLock()
	halted := ix.halted
	ix.mu.RUnlock()
	if halted {
		return nil
	}
	ci, err := ix.rpc.GetBlockchainInfo(ctx)
	if err != nil {
		return err
	}
	ix.mu.Lock()
	ix.nodeTip = ci.Blocks
	ix.mu.Unlock()

	// Catch up as many blocks as are available; each block is one atomic batch.
	for {
		st, err := ix.st.GetState()
		if err != nil {
			return err
		}
		if st.Height >= 0 && st.Hash == ci.BestBlockHash {
			break
		}
		if st.Height >= ci.Blocks && st.Height >= 0 {
			// Same height, different hash: the node switched branches.
			if err := ix.rollbackToFork(ctx, st); err != nil {
				return err
			}
			continue
		}
		next := st.Height + 1
		hash, err := ix.rpc.GetBlockHash(ctx, next)
		if err != nil {
			return err
		}
		blk, err := ix.rpc.GetBlock(ctx, hash)
		if err != nil {
			return err
		}
		if st.Height >= 0 && blk.PreviousBlockHash != st.Hash {
			if err := ix.rollbackToFork(ctx, st); err != nil {
				return err
			}
			continue
		}
		if err := ix.applyBlock(blk, st); err != nil {
			return fmt.Errorf("apply block %d: %w", blk.Height, err)
		}
		if blk.Height%1000 == 0 || blk.Height == ci.Blocks {
			log.Printf("indexer: height %d/%d", blk.Height, ci.Blocks)
		}
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if blk.Height >= ci.Blocks {
			break
		}
	}
	ix.mu.Lock()
	ix.lastSync = time.Now()
	ix.lastErr = ""
	ix.mu.Unlock()
	return ix.refreshMempool(ctx)
}

func (ix *Indexer) refreshMempool(ctx context.Context) error {
	raw, err := ix.rpc.GetRawMempool(ctx)
	if err != nil {
		return err
	}
	m := make(map[string]MempoolTx, len(raw))
	for txid, e := range raw {
		m[txid] = MempoolTx{Txid: txid, Size: e.VSize, Fee: rpc.Atoms(e.Fees.Base), FirstSeen: e.Time, Ancestors: e.AncestorCount}
	}
	ix.mu.Lock()
	ix.mempool = m
	ix.mu.Unlock()
	return nil
}

// rollbackToFork unwinds indexed blocks until the indexed tip is on the node's active chain.
func (ix *Indexer) rollbackToFork(ctx context.Context, st *store.State) error {
	depth := int64(0)
	for st.Height >= 0 {
		active, err := ix.rpc.GetBlockHash(ctx, st.Height)
		if err != nil {
			return err
		}
		if active == st.Hash {
			break
		}
		if depth >= ix.params.MaxReorgDepth {
			ix.mu.Lock()
			ix.halted = true
			ix.mu.Unlock()
			return fmt.Errorf("reorg deeper than %d blocks at height %d; indexer halted, operator action required", ix.params.MaxReorgDepth, st.Height)
		}
		blk, err := ix.st.GetBlock(st.Hash)
		if err != nil {
			return fmt.Errorf("load indexed block %s: %w", st.Hash, err)
		}
		if err := ix.rollbackBlock(blk, st, depth+1); err != nil {
			return fmt.Errorf("rollback block %d: %w", blk.Height, err)
		}
		depth++
		st, err = ix.st.GetState()
		if err != nil {
			return err
		}
	}
	if depth > 0 {
		log.Printf("indexer: reorg of depth %d resolved at height %d", depth, st.Height)
	}
	return nil
}

// addrDelta accumulates per-address changes inside one block.
type addrDelta struct {
	received, sent, utxos int64
	txs                   map[string]struct{}
	script, typ           string
}

type blockCtx struct {
	batch *store.Batch
	utxos map[string]*store.UTXO // pending writes visible within the block
	dels  map[string]bool
	addrs map[string]*addrDelta
}

func (bc *blockCtx) delta(addr, script, typ string) *addrDelta {
	d := bc.addrs[addr]
	if d == nil {
		d = &addrDelta{txs: map[string]struct{}{}, script: script, typ: typ}
		bc.addrs[addr] = d
	}
	return d
}

func (ix *Indexer) applyBlock(b *rpc.Block, st *store.State) error {
	bc := &blockCtx{batch: ix.st.NewBatch(), utxos: map[string]*store.UTXO{}, addrs: map[string]*addrDelta{}}
	blk := &store.Block{
		Height: b.Height, Hash: b.Hash, Prev: b.PreviousBlockHash, Time: b.Time, MedianTime: b.MedianTime,
		Version: b.Version, VersionHex: b.VersionHex, MerkleRoot: b.MerkleRoot, Bits: b.Bits, Target: b.Target,
		Difficulty: b.Difficulty, ChainWork: b.ChainWork, Nonce64: b.Nonce64, MatmulDigest: b.MatmulDigest,
		MatmulDim: b.MatmulDim, SeedA: b.SeedA, SeedB: b.SeedB, MatrixCWords: b.MatrixCWords,
		Size: b.Size, StrippedSize: b.StrippedSize, Weight: b.Weight, NTx: b.NTx,
		SubsidyBase: ix.params.BaseSubsidy(b.Height),
	}
	if st.Height >= 0 {
		if prev, err := ix.st.GetBlock(st.Hash); err == nil {
			blk.Interval = b.Time - prev.Time
		}
	}
	for i := range b.Tx {
		tx, err := ix.applyTx(bc, &b.Tx[i], b, int64(i))
		if err != nil {
			return err
		}
		blk.Txids = append(blk.Txids, tx.Txid)
		if tx.Coinbase {
			blk.CoinbaseValue = tx.OutputValue
		} else {
			blk.FeesTotal += tx.Fee
		}
	}
	for _, u := range bc.utxos {
		bc.batch.PutUTXO(u)
	}
	for addr, d := range bc.addrs {
		a, err := ix.st.GetAddr(addr)
		if errors.Is(err, store.ErrNotFound) {
			a = &store.Addr{Address: addr, Script: d.script, Type: d.typ, FirstHeight: b.Height}
		} else if err != nil {
			return err
		}
		a.Received += d.received
		a.Sent += d.sent
		a.UTXOCount += d.utxos
		a.TxCount += int64(len(d.txs))
		bc.batch.PutAddr(a)
	}
	bc.batch.PutBlock(blk)
	bc.batch.PutState(&store.State{Height: b.Height, Hash: b.Hash, Chain: st.Chain, Reorgs: st.Reorgs, LastReorg: st.LastReorg, UpdatedAt: time.Now().Unix()})
	return ix.st.Commit(bc.batch)
}

func (ix *Indexer) lookupUTXO(bc *blockCtx, txid string, n int64) (*store.UTXO, error) {
	id := store.UTXOID(txid, n)
	if u := bc.utxos[id]; u != nil {
		return u, nil
	}
	u, err := ix.st.GetUTXO(txid, n)
	if err != nil {
		return nil, fmt.Errorf("spent output %s:%d not in index: %w", txid, n, err)
	}
	bc.utxos[id] = u
	return u, nil
}

func (ix *Indexer) applyTx(bc *blockCtx, t *rpc.Tx, b *rpc.Block, idx int64) (*store.Tx, error) {
	tx := &store.Tx{
		Txid: t.Txid, Hash: t.Hash, Height: b.Height, BlockHash: b.Hash, BlockTime: b.Time, Index: idx,
		Version: t.Version, Locktime: t.Locktime, Size: t.Size, Weight: t.Weight, Fee: rpc.Atoms(t.Fee),
		Shielded: t.Shielded, ShieldedIn: t.ShieldedInputCount, ShieldedOut: t.ShieldedOutputCount, Hex: t.Hex,
	}
	seen := map[string]bool{}
	touch := func(addr, script, typ string) {
		if addr == "" {
			return
		}
		d := bc.delta(addr, script, typ)
		d.txs[t.Txid] = struct{}{}
		if !seen[addr] {
			seen[addr] = true
			tx.Addresses = append(tx.Addresses, addr)
		}
	}
	for _, in := range t.Vin {
		ti := store.TxIn{Coinbase: in.Coinbase, Txid: in.Txid, Vout: in.Vout, Sequence: in.Sequence, Witness: in.Witness, Nullifier: in.Nullifier}
		if in.Coinbase != "" {
			tx.Coinbase = true
		} else if in.Txid != "" {
			u, err := ix.lookupUTXO(bc, in.Txid, in.Vout)
			if err != nil {
				return nil, err
			}
			u.Spent, u.SpentBy, u.SpentAt = true, t.Txid, b.Height
			ti.Value, ti.Address, ti.Script = u.Value, u.Address, u.Script
			tx.InputValue += u.Value
			if u.Address != "" {
				d := bc.delta(u.Address, u.Script, u.Type)
				d.sent += u.Value
				d.utxos--
				touch(u.Address, u.Script, u.Type)
			}
		}
		tx.Vin = append(tx.Vin, ti)
	}
	for _, out := range t.Vout {
		v := rpc.Atoms(out.Value)
		to := store.TxOut{N: out.N, Value: v, Script: out.ScriptPubKey.Hex, Type: out.ScriptPubKey.Type, Address: out.ScriptPubKey.Address, Asm: out.ScriptPubKey.Asm, NoteCommitment: out.NoteCommitment}
		tx.Vout = append(tx.Vout, to)
		tx.OutputValue += v
		u := &store.UTXO{Txid: t.Txid, Vout: out.N, Value: v, Script: out.ScriptPubKey.Hex, Type: out.ScriptPubKey.Type, Address: out.ScriptPubKey.Address, Height: b.Height, Coinbase: tx.Coinbase}
		bc.utxos[store.UTXOID(t.Txid, out.N)] = u
		if out.ScriptPubKey.Address != "" {
			d := bc.delta(out.ScriptPubKey.Address, out.ScriptPubKey.Hex, out.ScriptPubKey.Type)
			d.received += v
			d.utxos++
			touch(out.ScriptPubKey.Address, out.ScriptPubKey.Hex, out.ScriptPubKey.Type)
		}
	}
	if tx.Addresses == nil {
		tx.Addresses = []string{}
	}
	bc.batch.PutTx(tx)
	return tx, nil
}

// rollbackBlock reverses one indexed block. The block record stays, marked orphaned.
func (ix *Indexer) rollbackBlock(blk *store.Block, st *store.State, depth int64) error {
	bc := &blockCtx{batch: ix.st.NewBatch(), utxos: map[string]*store.UTXO{}, addrs: map[string]*addrDelta{}}
	for i := len(blk.Txids) - 1; i >= 0; i-- {
		tx, err := ix.st.GetTx(blk.Txids[i])
		if err != nil {
			return fmt.Errorf("load tx %s: %w", blk.Txids[i], err)
		}
		seen := map[string]bool{}
		for _, out := range tx.Vout {
			bc.batch.DeleteUTXO(tx.Txid, out.N)
			if out.Address != "" {
				d := bc.delta(out.Address, out.Script, out.Type)
				d.received -= out.Value
				d.utxos--
				if !seen[out.Address] {
					seen[out.Address] = true
					d.txs[tx.Txid] = struct{}{}
				}
			}
		}
		for _, in := range tx.Vin {
			if in.Txid == "" {
				continue
			}
			u, err := ix.st.GetUTXO(in.Txid, in.Vout)
			if err != nil {
				return fmt.Errorf("restore output %s:%d: %w", in.Txid, in.Vout, err)
			}
			u.Spent, u.SpentBy, u.SpentAt = false, "", 0
			bc.batch.PutUTXO(u)
			if u.Address != "" {
				d := bc.delta(u.Address, u.Script, u.Type)
				d.sent -= u.Value
				d.utxos++
				if !seen[u.Address] {
					seen[u.Address] = true
					d.txs[tx.Txid] = struct{}{}
				}
			}
		}
		bc.batch.DeleteTx(tx.Txid)
	}
	for addr, d := range bc.addrs {
		a, err := ix.st.GetAddr(addr)
		if err != nil {
			return fmt.Errorf("load addr %s: %w", addr, err)
		}
		a.Received += d.received
		a.Sent += d.sent
		a.UTXOCount += d.utxos
		a.TxCount -= int64(len(d.txs))
		if a.TxCount <= 0 && a.Received == 0 && a.Sent == 0 {
			bc.batch.DeleteAddr(addr)
		} else {
			bc.batch.PutAddr(a)
		}
	}
	blk.Orphaned = true
	bc.batch.PutBlock(blk)
	bc.batch.PutState(&store.State{Height: blk.Height - 1, Hash: blk.Prev, Chain: st.Chain, Reorgs: st.Reorgs + boolToInt(depth == 1), LastReorg: depth, UpdatedAt: time.Now().Unix()})
	return ix.st.Commit(bc.batch)
}

func boolToInt(b bool) int64 {
	if b {
		return 1
	}
	return 0
}
