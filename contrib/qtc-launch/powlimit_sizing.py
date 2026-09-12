#!/usr/bin/env python3
"""QTC mainnet powLimit sizing from the measured A6000 full-digest rate (D3, Option B).

Model: per-digest success probability P = target / 2^256; block interval at target = 1/(P*R).
Launch ASERT (anchored at genesis, tau=172,800 s, spacing 600 s): target_h = floor * 2^(-(deficit)/tau),
clamped at floor, where deficit = h*600 - t_h (seconds ahead of schedule).
"""
import math
SPACING, TAU = 600, 172_800
RATES = {  # full digests per second (n=512, b=16, r=8, oracle v2, digest v4)
    "A6000 (Thunder container, measured)": 8_800,
    "A6000 (bare metal, GPU-bound est.)": 13_000,
    "A6000 + RTX 4000 Ada (Ada est. 0.5x)": 13_200,
    "Apple M5 Metal (measured)": 840,
    "one CPU core, M5 (measured)": 3.6,
    "one CPU core, x86 server (measured)": 1.0,
}
def compact(mant, exp): return (exp << 24) | mant
def target(mant, exp): return mant << (8 * (exp - 3))
def P(mant, exp): return target(mant, exp) / 2**256
def roundtrip_ok(mant, exp):
    t = target(mant, exp)
    # target_to_compact as in stall_sim / arith_uint256::GetCompact
    size = (t.bit_length() + 7) // 8
    word = t >> (8 * (size - 3)) if size > 3 else t << (8 * (3 - size))
    if word & 0x800000: word >>= 8; size += 1
    return ((size << 24) | word) == compact(mant, exp) and (word << (8 * (size - 3))) == t
def launch_overshoot(p_floor, R):
    """Expected-value ASERT walk from genesis with a constant fleet R mining at the floor."""
    t, h, p = 0.0, 0, p_floor
    while True:
        t += 1.0 / (p * R); h += 1
        deficit = h * SPACING - t
        p = min(p_floor, p_floor * 2 ** (-deficit / TAU))
        if 1.0 / (p * R) >= 0.98 * SPACING or h > 20000:
            return h, t / 3600, h - t / SPACING   # blocks, hours, blocks ahead of schedule
CANDS = [
    ("placeholder (0x1e013333)", 0x013333, 0x1e),
    ("A: fleet-sized, A6000+Ada at 600 s", 0x021e4a, 0x1e),
    ("B: one A6000 at 600 s (RECOMMENDED)", 0x033333, 0x1e),
    ("C: one A6000 at 300 s (2x headroom)", 0x066666, 0x1e),
]
print(f"{'candidate':40s} {'compact':>10s} {'P per digest':>13s} {'log2 P':>7s} compact-exact")
for name, m, e in CANDS:
    print(f"{name:40s} 0x{compact(m,e):08x} {P(m,e):13.3e} {math.log2(P(m,e)):7.2f} {roundtrip_ok(m,e)}")
print("\nExpected seconds per block AT THE FLOOR (floor is the easiest target the chain can ever have):")
print(f"{'fleet':40s}" + "".join(f"{n.split(' ')[0]:>14s}" for n,_,_ in CANDS))
for fleet, R in RATES.items():
    row = f"{fleet:40s}"
    for _, m, e in CANDS:
        s = 1 / (P(m, e) * R)
        row += f"{s:14,.0f}" if s < 1e5 else f"{s/86400:12,.1f} d"
    print(row)
print("\nLaunch overshoot with the expected fleet (A6000 + Ada est., 13,200/s), ASERT from genesis:")
print(f"{'candidate':40s} {'blocks to settle':>16s} {'hours':>7s} {'blocks ahead of schedule':>25s}")
for name, m, e in CANDS:
    b, hrs, ahead = launch_overshoot(P(m, e), 13_200)
    print(f"{name:40s} {b:16d} {hrs:7.1f} {ahead:25.0f}")
print("\nSame with the A6000 alone (8,800/s):")
for name, m, e in CANDS:
    b, hrs, ahead = launch_overshoot(P(m, e), 8_800)
    print(f"{name:40s} {b:16d} {hrs:7.1f} {ahead:25.0f}")
print("\nCPU-only floor-difficulty block production (headers-only attack surface, H3):")
for name, m, e in CANDS:
    per_day_100cores = P(m, e) * 3.6 * 100 * 86400
    print(f"{name:40s} 100 M5 cores -> {per_day_100cores:6.2f} blocks/day at floor")
print("\nuint256 hex for chainparams (candidate B):", f"{target(0x033333,0x1e):064x}")
print("genesis nBits must equal compact(powLimit):", f"0x{compact(0x033333,0x1e):08x}")
