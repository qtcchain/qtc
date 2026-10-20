package policy

import "testing"

func TestSignVerifyAndWindow(t *testing.T) {
	doc := Default()
	doc["agent_id"] = "a"
	signed := Sign(doc, "s")
	if !Verify(signed, "s") || Verify(signed, "t") {
		t.Fatal("signature")
	}
	signed["limits"].(map[string]any)["per_tx_atoms"] = 1
	if Verify(signed, "s") {
		t.Fatal("tamper undetected")
	}
	// Monday 2026-10-19 12:00 UTC
	now := float64(1792411200)
	if !InWindow([]any{map[string]any{"days": "mon-fri", "utc": "09:00-17:00"}}, now) {
		t.Fatal("inside window")
	}
	if InWindow([]any{map[string]any{"days": "sat-sun"}}, now) || InWindow([]any{map[string]any{"utc": "00:00-00:01"}}, now) {
		t.Fatal("outside window")
	}
	if !InWindow([]any{map[string]any{"days": "fri-mon"}}, now) {
		t.Fatal("wrapped window")
	}
}
