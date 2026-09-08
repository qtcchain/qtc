#!/usr/bin/env python3
"""QTC Option-B stall simulation on regtest (deterministic, mocktime-driven).

Phases (each block's nBits is compared against an independent Python model of
the node's clamped ASERT formula, anchored at genesis):
  P1 ontime   : 30 blocks at exactly 90 s cadence   -> target must sit at the floor
  P2 tighten  : N blocks with mocktime pinned        -> each block ~90 s early, target
                                                       tightens ~2^(-90/tau) per block
  P3 stall    : jump mocktime by S half-lives, then M blocks with mocktime pinned
                (fleet mines as fast as the floor allows) -> target eases, clamps
                at powLimit (never below), holds while the surplus burns, lifts off
  P4 ontime2  : 30 blocks at 90 s cadence            -> steady

Variants: nodrift (raw ASERT + floor) and drift (D4: future-MTP-drift bound,
timewarp reconcile and BIP94 active from genesis = the faithful Option B).
"""
import argparse
import csv
import json
import math
import os
import shutil
import subprocess
import sys
import time

SCRATCH = os.environ.get("QTC_SIM_OUT", os.getcwd())
QTC_BIN = os.environ.get("QTC_BIN", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "bin"))
TAU = 172_800   # overridden by --tau
SPACING = 600   # overridden by --spacing
DRIFT = 43_200  # overridden by --drift
GENESIS_BITS = 0x207FFFFF
LOG2_TOL = 0.002  # compact-encoding + ASERT polynomial rounding


def compact_to_target(nbits):
    size = nbits >> 24
    word = nbits & 0x007FFFFF
    return word >> (8 * (3 - size)) if size <= 3 else word << (8 * (size - 3))


def target_to_compact(t):
    """Mirror arith_uint256::GetCompact (truncating mantissa)."""
    size = (t.bit_length() + 7) // 8
    c = t << (8 * (3 - size)) if size <= 3 else t >> (8 * (size - 3))
    if c & 0x00800000:
        c >>= 8
        size += 1
    return c | (size << 24)


def asert_expected(anchor_target, floor, time_diff, height_diff):
    """Clamped aserti3-2d, real-valued; returns the target the node should emit."""
    exponent = (time_diff - SPACING * (height_diff + 1)) / TAU
    t = anchor_target * (2.0 ** exponent)
    if t > floor:
        return floor
    return max(1, int(t))


class Node:
    def __init__(self, name, extra_args, rpcport, port):
        self.datadir = os.path.join(SCRATCH, f"qtc-stall-{name}")
        shutil.rmtree(self.datadir, ignore_errors=True)
        os.makedirs(self.datadir)
        self.logf = open(os.path.join(self.datadir, "qtcd.stdout"), "w")
        args = [f"{QTC_BIN}/qtcd", "-regtest", f"-datadir={self.datadir}",
                f"-rpcport={rpcport}", f"-port={port}", "-listen=0", "-noconnect",
                "-dnsseed=0", "-server=1"] + extra_args
        env = dict(os.environ, QTC_MATMUL_BACKEND="cpu")
        self.proc = subprocess.Popen(args, stdout=self.logf, stderr=subprocess.STDOUT, env=env)
        self.cli = [f"{QTC_BIN}/qtc-cli", "-regtest", f"-datadir={self.datadir}", f"-rpcport={rpcport}"]
        for _ in range(240):
            if self.proc.poll() is not None:
                sys.exit(f"qtcd exited early (code {self.proc.returncode}); see {self.logf.name}")
            try:
                self.rpc("getblockcount")
                break
            except RuntimeError:
                time.sleep(0.5)
        else:
            sys.exit("RPC never came up")

    def rpc(self, *a):
        out = subprocess.run(self.cli + [str(x) for x in a], capture_output=True, text=True)
        if out.returncode != 0:
            raise RuntimeError(f"rpc {a} failed: {out.stderr.strip() or out.stdout.strip()}")
        s = out.stdout.strip()
        try:
            return json.loads(s)
        except ValueError:
            return s

    def header(self, height):
        return self.rpc("getblockheader", self.rpc("getblockhash", height))

    def stop(self):
        try:
            self.rpc("stop")
        except RuntimeError:
            pass
        self.proc.wait(timeout=180)
        self.logf.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=["nodrift", "drift"], required=True)
    # Compact-exact floor (mantissa 0x03ffff, exponent 0x20) so genesis nBits == compact(powLimit)
    # exactly, as Bitcoin does; a looser genesis nBits trips rpc/util.cpp GetTarget's CHECK_NONFATAL.
    ap.add_argument("--floor", default="03ffff" + "00" * 29, help="powLimit hex (default 0x03ffff<<232)")
    ap.add_argument("--tighten", type=int, default=480)
    ap.add_argument("--stall-halflives", type=float, default=6.0)
    ap.add_argument("--post", type=int, default=600)
    ap.add_argument("--ontime", type=int, default=30)
    ap.add_argument("--spacing", type=int, default=600, help="target block spacing in seconds (regtest override)")
    ap.add_argument("--tau", type=int, default=172_800, help="ASERT half-life in seconds (regtest override)")
    ap.add_argument("--drift", type=int, default=43_200, help="future-MTP drift bound in seconds (drift variant)")
    a = ap.parse_args()
    global SPACING, TAU, DRIFT
    SPACING, TAU, DRIFT = a.spacing, a.tau, a.drift

    floor = int(a.floor, 16)
    floor_bits = target_to_compact(floor)
    anchor = min(compact_to_target(floor_bits), floor)  # genesis nBits = compact(powLimit); node clamps anyway
    # -test=matmulasert clears regtest's inherited fPowNoRetargeting (Bitcoin regtest default) while
    # keeping nFastMineHeight = nMatMulAsertHeight = 0, i.e. ASERT anchored at genesis = Option B.
    extra = [f"-regtestmatmulpowlimit={a.floor}", f"-regtestgenesisbits={floor_bits:08x}", "-test=matmulasert",
             f"-regtestmatmulpowtargetspacing={a.spacing}", f"-regtestmatmulaserthalflife={a.tau}"]
    rpcport, port = (39754, 39755)
    if a.variant == "drift":
        extra += ["-regtestmatmulmaxfuturemtpdriftheight=0", f"-regtestmatmulmaxfuturemtpdrift={a.drift}",
                  "-regtestmatmultimewarpreconcileheight=0", "-test=bip94"]
        rpcport, port = (39764, 39765)

    print(f"[{a.variant}] floor={a.floor} compact=0x{floor_bits:08x} anchor=floor:{anchor == floor} args={extra}")
    node = Node(a.variant, extra, rpcport, port)
    rows, failures = [], []
    t_wall0 = time.time()
    try:
        node.rpc("createwallet", "stallw")
        addr = node.rpc("-rpcwallet=stallw", "getnewaddress")
        g = node.header(0)
        g_time = g["time"]
        assert int(g["bits"], 16) == floor_bits, f"genesis bits {g['bits']} != floor compact 0x{floor_bits:08x}"
        prev = {"height": 0, "time": g_time, "mediantime": g["mediantime"]}
        mock = g_time

        def mine(phase, advance):
            nonlocal prev, mock
            if advance:
                mock += advance
                node.rpc("setmocktime", mock)
            t0 = time.time()
            node.rpc("generatetoaddress", 1, addr)
            dt_wall = time.time() - t0
            h = node.header(prev["height"] + 1)
            bits = int(h["bits"], 16)
            target = compact_to_target(bits)
            expected = asert_expected(anchor, floor, prev["time"] - g_time, prev["height"])
            exp_target = compact_to_target(target_to_compact(expected))
            l2_obs = math.log2(target / floor)
            l2_exp = math.log2(exp_target / floor)
            row = dict(phase=phase, height=h["height"], time=h["time"], mediantime=h["mediantime"],
                       dt_prev=h["time"] - prev["time"], time_minus_prev_mtp=h["time"] - prev["mediantime"],
                       mocktime=mock, bits=f"0x{bits:08x}", log2_ratio_obs=round(l2_obs, 5),
                       log2_ratio_expected=round(l2_exp, 5), model_err=round(l2_obs - l2_exp, 6),
                       above_floor=target > floor, wall_s=round(dt_wall, 3))
            rows.append(row)
            if target > floor:
                failures.append(f"h{h['height']}: target ABOVE powLimit (bits {row['bits']})")
            if abs(l2_obs - l2_exp) > LOG2_TOL:
                failures.append(f"h{h['height']}: ASERT model mismatch obs={l2_obs:.5f} exp={l2_exp:.5f}")
            if a.variant == "drift" and h["time"] - prev["mediantime"] > DRIFT:
                failures.append(f"h{h['height']}: time exceeds prev MTP + {DRIFT}")
            prev = h
            return row

        # P1 on-time
        # aserti3-2d counts the anchor block itself, so exact-cadence blocks sit a constant
        # 2^(-SPACING/TAU) below the floor (not at it); the per-block model check covers this.
        for _ in range(a.ontime):
            r = mine("P1_ontime", SPACING)
            if r["log2_ratio_obs"] < -SPACING / TAU - LOG2_TOL:
                failures.append(f"h{r['height']}: on-time block tighter than one spacing below floor ({r['bits']})")
        print(f"P1 done h={prev['height']} bits={rows[-1]['bits']} wall={time.time()-t_wall0:.0f}s")

        # P2 tighten (mocktime pinned)
        last_t = compact_to_target(int(rows[-1]["bits"], 16))
        for _ in range(a.tighten):
            r = mine("P2_tighten", 0)
            t = compact_to_target(int(r["bits"], 16))
            if t > last_t:
                failures.append(f"h{r['height']}: target loosened during tighten phase")
            last_t = t
        pre_stall = rows[-1]
        print(f"P2 done h={prev['height']} log2(target/floor)={pre_stall['log2_ratio_obs']} "
              f"(predicted ~{-(a.tighten*(SPACING-1))/TAU:.3f}) wall={time.time()-t_wall0:.0f}s")

        # P3 stall: jump mocktime, then mine with mocktime pinned
        stall_s = int(a.stall_halflives * TAU)
        # The block mined right after the jump *carries* the stalled timestamp; its own nBits was
        # computed from the pre-stall parent. Post-stall difficulty first appears on the next block.
        carrier = mine("P3_carrier", stall_s)
        first = mine("P3_poststall", 0)
        print(f"P3 carrier h={carrier['height']} dt_prev={carrier['dt_prev']} bits={carrier['bits']}; "
              f"first post-stall h={first['height']} bits={first['bits']} log2={first['log2_ratio_obs']} "
              f"(floor bits 0x{floor_bits:08x})")
        reached_floor_at = first["height"] if int(first["bits"], 16) == floor_bits else None
        if a.variant == "nodrift" and reached_floor_at is None:
            failures.append("nodrift: first post-stall block did not clamp at floor")
        for _ in range(a.post - 2):
            r = mine("P3_poststall", 0)
            if reached_floor_at is None and int(r["bits"], 16) == floor_bits:
                reached_floor_at = r["height"]
        if reached_floor_at is None:
            failures.append("post-stall: floor never reached")
        floor_rows = [r for r in rows if r["phase"] == "P3_poststall" and int(r["bits"], 16) == floor_bits]
        lifted = rows[-1]["log2_ratio_obs"] < -LOG2_TOL
        if not lifted:
            failures.append("post-stall: target did not lift off the floor within --post blocks")
        print(f"P3 done h={prev['height']} floor reached at h={reached_floor_at}, "
              f"blocks at floor={len(floor_rows)}, final log2={rows[-1]['log2_ratio_obs']} "
              f"chain_time-mocktime={prev['time']-mock} wall={time.time()-t_wall0:.0f}s")

        # P4 on-time again. Pinned mining let the chain clock creep (MTP+1) ahead of mocktime, so
        # re-align mocktime to the tip first; the first P4 block's nBits still derives from the
        # last pinned parent, so the steady-state baseline is the block after it.
        mock = max(mock, prev["time"])
        node.rpc("setmocktime", mock)
        mine("P4_ontime", SPACING)
        l2_before = rows[-1]["log2_ratio_obs"]
        for _ in range(a.ontime - 1):
            mine("P4_ontime", SPACING)
        if abs(rows[-1]["log2_ratio_obs"] - l2_before) > LOG2_TOL:
            failures.append("P4: target drifted while mining exactly on cadence")
        print(f"P4 done h={prev['height']} log2={rows[-1]['log2_ratio_obs']} wall={time.time()-t_wall0:.0f}s")
    finally:
        node.stop()

    out_csv = os.path.join(SCRATCH, f"qtc-stall-{a.variant}.csv")
    with open(out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    summary = dict(variant=a.variant, floor=a.floor, floor_bits=f"0x{floor_bits:08x}", blocks=len(rows),
                   tighten=a.tighten, stall_halflives=a.stall_halflives, post=a.post,
                   pre_stall_log2=pre_stall["log2_ratio_obs"], first_poststall=first,
                   floor_reached_at=reached_floor_at, blocks_at_floor=len(floor_rows),
                   final_log2=rows[-1]["log2_ratio_obs"], max_model_err=max(abs(r["model_err"]) for r in rows),
                   any_above_floor=any(r["above_floor"] for r in rows), failures=failures,
                   wall_s=round(time.time() - t_wall0), csv=out_csv)
    with open(os.path.join(SCRATCH, f"qtc-stall-{a.variant}.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary, indent=1))
    print("RESULT:", "PASS" if not failures else f"FAIL ({len(failures)} failures)")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
