// Package pq decodes P2MR (witness v2) spends for display. It never verifies signatures.
package pq

import (
	"encoding/binary"
	"encoding/hex"
)

// Opcode values from src/script/script.h.
const (
	opCLTV              = 0xb1
	opCSV               = 0xb2
	opCTV               = 0xb3
	opNumEqual          = 0x9c
	opCheckSigMLDSA     = 0xbb
	opCheckSigSLHDSA    = 0xbc
	opCheckSigFromStack = 0xbd
	opCheckSigAddMLDSA  = 0xbe
	opCheckSigAddSLHDSA = 0xbf
	opPushData1         = 0x4c
	opPushData2         = 0x4d

	MLDSA44PubKeyLen = 1312
	MLDSA44SigLen    = 2420
	SLHDSAPubKeyLen  = 32
)

// Decoded describes one P2MR witness.
type Decoded struct {
	IsP2MR      bool   `json:"is_p2mr"`
	Algorithm   string `json:"algorithm"`         // mldsa44, slhdsa, multisig, unknown
	Variant     string `json:"variant,omitempty"` // "", cltv, csv, ctv
	SigBytes    int    `json:"signature_bytes"`
	LeafBytes   int    `json:"leaf_script_bytes"`
	LeafVersion byte   `json:"leaf_version"`
	Siblings    int    `json:"merkle_siblings"`
	PubKeyBytes int    `json:"pubkey_bytes,omitempty"`
	Keys        int    `json:"keys,omitempty"`
	Note        string `json:"note,omitempty"`
}

// Decode inspects a witness stack. A P2MR key-path style spend is
// [signature, leaf script, control block]; anything else is reported as not P2MR.
func Decode(witness []string) Decoded {
	d := Decoded{Algorithm: "unknown"}
	if len(witness) < 3 {
		return d
	}
	// The control block is always last, the leaf script second to last.
	cb, err1 := hex.DecodeString(witness[len(witness)-1])
	leaf, err2 := hex.DecodeString(witness[len(witness)-2])
	if err1 != nil || err2 != nil || len(cb) < 1 || (len(cb)-1)%32 != 0 || len(leaf) == 0 {
		return d
	}
	d.IsP2MR = true
	d.LeafVersion = cb[0] & 0xfe
	d.Siblings = (len(cb) - 1) / 32
	d.LeafBytes = len(leaf)
	sig, _ := hex.DecodeString(witness[0])
	d.SigBytes = len(sig)

	// Strip timelock / covenant prefixes: <push> OP_x OP_DROP.
	body := leaf
	if n, op := leadingPushThenOp(body); n > 0 {
		switch op {
		case opCLTV:
			d.Variant = "cltv"
		case opCSV:
			d.Variant = "csv"
		case opCTV:
			d.Variant = "ctv"
		}
		if d.Variant != "" {
			body = body[n:]
		}
	}
	if len(body) == 0 {
		return d
	}
	last := body[len(body)-1]
	switch {
	case last == opNumEqual:
		d.Algorithm = "multisig"
		d.Keys = countKeys(body)
	case last == opCheckSigMLDSA:
		if n, l := pushLen(body); n > 0 && l == MLDSA44PubKeyLen {
			d.Algorithm = "mldsa44"
			d.PubKeyBytes = l
		}
	case last == opCheckSigSLHDSA:
		if n, l := pushLen(body); n > 0 && l == SLHDSAPubKeyLen {
			d.Algorithm = "slhdsa"
			d.PubKeyBytes = l
		}
	case last == opCheckSigFromStack:
		d.Algorithm = "csfs"
	}
	if d.Algorithm == "mldsa44" && d.SigBytes != MLDSA44SigLen {
		d.Note = "signature length is not the ML-DSA-44 size"
	}
	return d
}

// pushLen returns the encoded length of a leading push and its payload length.
func pushLen(b []byte) (int, int) {
	if len(b) == 0 {
		return 0, 0
	}
	switch op := b[0]; {
	case op >= 1 && op <= 75:
		return 1, int(op)
	case op == opPushData1 && len(b) >= 2:
		return 2, int(b[1])
	case op == opPushData2 && len(b) >= 3:
		return 3, int(binary.LittleEndian.Uint16(b[1:3]))
	}
	return 0, 0
}

// leadingPushThenOp reports the byte count of "<push> OP OP_DROP" and the OP.
func leadingPushThenOp(b []byte) (int, byte) {
	n, l := pushLen(b)
	if n == 0 || len(b) < n+l+2 {
		return 0, 0
	}
	op := b[n+l]
	if b[n+l+1] != 0x75 { // OP_DROP
		return 0, 0
	}
	return n + l + 2, op
}

func countKeys(b []byte) int {
	keys := 0
	for i := 0; i < len(b); {
		n, l := pushLen(b[i:])
		if n > 0 && (l == MLDSA44PubKeyLen || l == SLHDSAPubKeyLen) && i+n+l < len(b) {
			switch b[i+n+l] {
			case opCheckSigMLDSA, opCheckSigSLHDSA, opCheckSigAddMLDSA, opCheckSigAddSLHDSA:
				keys++
			}
			i += n + l + 1
			continue
		}
		i++
	}
	return keys
}
