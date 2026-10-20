// Package jsonx: canonical JSON (sorted keys, no spaces, no HTML escaping), number-preserving decoding,
// and small accessors over map[string]any documents (policies, RPC results).
package jsonx

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
)

// Canonical encodes v with sorted keys and compact separators. Numbers decoded with Decode keep their
// original text, so sign/verify round-trips are byte-stable.
func Canonical(v any) string {
	var b bytes.Buffer
	writeCanonical(&b, v)
	return b.String()
}

func writeCanonical(b *bytes.Buffer, v any) {
	switch t := v.(type) {
	case nil:
		b.WriteString("null")
	case map[string]any:
		keys := make([]string, 0, len(t))
		for k := range t {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		b.WriteByte('{')
		for i, k := range keys {
			if i > 0 {
				b.WriteByte(',')
			}
			writeString(b, k)
			b.WriteByte(':')
			writeCanonical(b, t[k])
		}
		b.WriteByte('}')
	case []any:
		b.WriteByte('[')
		for i, e := range t {
			if i > 0 {
				b.WriteByte(',')
			}
			writeCanonical(b, e)
		}
		b.WriteByte(']')
	case []string:
		arr := make([]any, len(t))
		for i, s := range t {
			arr[i] = s
		}
		writeCanonical(b, arr)
	case []map[string]any:
		arr := make([]any, len(t))
		for i, s := range t {
			arr[i] = s
		}
		writeCanonical(b, arr)
	case string:
		writeString(b, t)
	case json.Number:
		b.WriteString(string(t))
	case bool:
		if t {
			b.WriteString("true")
		} else {
			b.WriteString("false")
		}
	case int:
		b.WriteString(strconv.Itoa(t))
	case int64:
		b.WriteString(strconv.FormatInt(t, 10))
	case float64:
		if t == math.Trunc(t) && math.Abs(t) < 1e15 {
			b.WriteString(strconv.FormatInt(int64(t), 10))
		} else {
			b.WriteString(strconv.FormatFloat(t, 'f', -1, 64))
		}
	default:
		// structs etc.: marshal then re-decode to get sorted keys
		raw, err := json.Marshal(t)
		if err != nil {
			b.WriteString("null")
			return
		}
		var any2 any
		if err := Decode(raw, &any2); err != nil {
			b.WriteString("null")
			return
		}
		writeCanonical(b, any2)
	}
}

func writeString(b *bytes.Buffer, s string) {
	// json.Marshal of a string, without HTML escaping (ensure_ascii=False equivalent: UTF-8 kept)
	var tmp bytes.Buffer
	enc := json.NewEncoder(&tmp)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(s)
	b.Write(bytes.TrimRight(tmp.Bytes(), "\n"))
}

// Decode unmarshals keeping numbers as json.Number.
func Decode(data []byte, v any) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	return dec.Decode(v)
}

// Pretty renders v as indented JSON without HTML escaping.
func Pretty(v any) string {
	var b bytes.Buffer
	enc := json.NewEncoder(&b)
	enc.SetEscapeHTML(false)
	enc.SetIndent("", "  ")
	_ = enc.Encode(v)
	return b.String()
}

// Compact renders v as compact JSON without HTML escaping (key order as encoding/json: sorted for maps).
func Compact(v any) []byte {
	var b bytes.Buffer
	enc := json.NewEncoder(&b)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
	return bytes.TrimRight(b.Bytes(), "\n")
}

// Clone deep-copies a JSON-like value.
func Clone(v any) any {
	var out any
	_ = Decode(Compact(v), &out)
	return out
}

// Int converts a JSON-ish scalar to int64 (json.Number, float64, int, int64, string digits). ok=false otherwise.
func Int(v any) (int64, bool) {
	switch t := v.(type) {
	case json.Number:
		if i, err := t.Int64(); err == nil {
			return i, true
		}
		if f, err := t.Float64(); err == nil && f == math.Trunc(f) {
			return int64(f), true
		}
		return 0, false
	case float64:
		if t == math.Trunc(t) {
			return int64(t), true
		}
		return 0, false
	case int:
		return int64(t), true
	case int64:
		return t, true
	case string:
		i, err := strconv.ParseInt(strings.TrimSpace(t), 10, 64)
		return i, err == nil
	}
	return 0, false
}

// IsIntegral reports whether v is an integer-valued JSON number (floats such as 1.5 and bools fail).
func IsIntegral(v any) bool {
	switch t := v.(type) {
	case bool:
		return false
	case json.Number:
		if _, err := t.Int64(); err == nil {
			return true
		}
		return !strings.ContainsAny(string(t), ".eE")
	}
	_, ok := Int(v)
	return ok
}

// Float converts a JSON-ish scalar to float64.
func Float(v any) (float64, bool) {
	switch t := v.(type) {
	case json.Number:
		f, err := t.Float64()
		return f, err == nil
	case float64:
		return t, true
	case int:
		return float64(t), true
	case int64:
		return float64(t), true
	case string:
		f, err := strconv.ParseFloat(t, 64)
		return f, err == nil
	}
	return 0, false
}

// M returns m[key] as a map (empty map when missing/not a map).
func M(m map[string]any, key string) map[string]any {
	if m == nil {
		return map[string]any{}
	}
	if sub, ok := m[key].(map[string]any); ok {
		return sub
	}
	return map[string]any{}
}

// L returns m[key] as a slice (nil when missing).
func L(m map[string]any, key string) []any {
	if m == nil {
		return nil
	}
	if l, ok := m[key].([]any); ok {
		return l
	}
	return nil
}

// Strs returns m[key] as a slice of strings.
func Strs(m map[string]any, key string) []string {
	var out []string
	for _, e := range L(m, key) {
		if s, ok := e.(string); ok {
			out = append(out, s)
		}
	}
	return out
}

// S returns m[key] as a string ("" when missing).
func S(m map[string]any, key string) string {
	if m == nil {
		return ""
	}
	s, _ := m[key].(string)
	return s
}

// I returns m[key] as int64 with a default.
func I(m map[string]any, key string, def int64) int64 {
	if m == nil {
		return def
	}
	if v, ok := Int(m[key]); ok {
		return v
	}
	return def
}

// F returns m[key] as float64 with a default.
func F(m map[string]any, key string, def float64) float64 {
	if m == nil {
		return def
	}
	if v, ok := Float(m[key]); ok {
		return v
	}
	return def
}

// B returns m[key] as bool with a default.
func B(m map[string]any, key string, def bool) bool {
	if m == nil {
		return def
	}
	if v, ok := m[key].(bool); ok {
		return v
	}
	return def
}

// Contains reports whether s is among the strings in list.
func Contains(list []string, s string) bool {
	for _, e := range list {
		if e == s {
			return true
		}
	}
	return false
}

// Str formats any scalar for messages.
func Str(v any) string { return fmt.Sprint(v) }
