#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Model Quicksilver's mainnet launch difficulty floor and the tx-PoW cost coupled to it.

Block work (cycles per block) is 2**256 // (target + 1). RequiredTxWork is the
mean block work over the MA window, divided by K, scaled by the congestion
multiplier, floored at 1. The launch floor therefore sets both how long a block
takes to find and what a transaction costs to send.

Beyond the static floor the model also answers the two questions the floor
constant alone cannot:

  * the OPENING window -- how fast the chain runs at the floor when the launch
    fleet is armed, and whether the first retarget can reach the 300 s target
    in one 144-block epoch or is clamped and needs two;
  * DEGRADED recovery -- how long the chain runs slow when the fleet collapses
    to a single card, which depends on the steady-state difficulty the fleet
    established and therefore on how many cards were armed.

See doc/audit/mainnet-difficulty-floor-model.md for the derivation and
tools/calibration/e28-floor.md for the E28 arithmetic.
"""

import argparse

K = 106                      # consensus.nTxWorkCouplingK
CONGESTION_ONE = 65536       # src/pow.h: fixed-point scale for m (2^16 == 1.0)
TARGET_SPACING_SECONDS = 300
RETARGET_WINDOW_BLOCKS = 144  # consensus.nPowTargetTimespan / spacing
FIRST_RETARGET_SPACINGS = RETARGET_WINDOW_BLOCKS - 1
RETARGET_CLAMP = 4            # src/pow.cpp: nActualTimespan clamped to [1/4, 4]
LAUNCH_FLOOR_CYCLES = 4       # shipped: powLimit == 0x203fffff

# Cycles per graph, POOLED across M1+M2 (451 cycles over 19,300 graphs). NOT
# M2's standalone 1/43.7 == 0.022883, which understates every derived rate by
# 2.1%. This value reproduces the floor doc's own published anchor:
# 0.02337 / 1.5648 == 0.014935 cycles/s on the six-GPU-contended P104-100.
POOLED_CYCLES_PER_GRAPH = 0.02337
LOWER_95_CYCLES_PER_GRAPH = 0.02124
DEGRADED_GRAPH_TIME_MULTIPLIER = 1.25

# Final-slot E28 seconds per graph, measured by qs-solver under a stated quiet
# condition: node stopped, no solver process, display duty off the mining card,
# six graphs per sample, and the median of three samples. Windows probes ran in
# SSH session 0; windowsqs2 retained seven idle desktop/UWP compute contexts.
# These are standardized solver-capacity inputs, not production node counters.
# Full conditions: qs-planning/plans/assets/2026-09-06-baselines.md.
LAUNCH_FLEET_QUIET = {
    "P104-100":    1.4667,  # linuxqs1, Slot 1 / PCIe x4
    "GTX 950":     3.4383,  # linuxqs2, Slot 1 / PCIe x4
    "GTX 1050 Ti": 3.1912,  # windowsqs1, Slot 1 / PCIe x4
    "GTX 960":     3.2093,  # windowsqs2, Slot 1 / PCIe x4
}


def target_for_cycles(cycles_per_block: int) -> int:
    return (2 ** 256) // cycles_per_block - 1


def cycles_for_target(target: int) -> int:
    return (2 ** 256) // (target + 1)


def compact_from_target(target: int) -> int:
    """Encode a target as compact nBits (mirrors arith_uint256::GetCompact)."""
    size = (target.bit_length() + 7) // 8
    if size <= 3:
        compact = target << (8 * (3 - size))
    else:
        compact = target >> (8 * (size - 3))
    if compact & 0x00800000:
        compact >>= 8
        size += 1
    return compact | (size << 24)


def cycles_per_second(seconds_per_graph: float,
                      cycles_per_graph: float = POOLED_CYCLES_PER_GRAPH) -> float:
    """Convert a measured graph rate to a cycle rate.

    M2 established that cycles per graph is a constant 1/PROOFSIZE, independent
    of edge bits and of CPU-vs-GPU, so card speed reduces entirely to
    seconds-per-graph. All the real uncertainty lives in cycles_per_graph
    (95% CI [39.2, 47.1] graphs/cycle), not in the timing, which is measured to
    better than 0.5%.
    """
    return cycles_per_graph / seconds_per_graph


def aggregate_rate(cards: dict,
                   cycles_per_graph: float = POOLED_CYCLES_PER_GRAPH) -> float:
    """Total cycles/s for a set of {name: seconds_per_graph}. Rates add."""
    return sum(cycles_per_second(spg, cycles_per_graph)
               for spg in cards.values())


def block_seconds(work_cycles: float, rate: float) -> float:
    """Mean seconds to find a block of `work_cycles` at `rate` cycles/s."""
    return work_cycles / rate


def steady_state_work(rate: float,
                      floor_cycles: int = LAUNCH_FLOOR_CYCLES,
                      spacing: int = TARGET_SPACING_SECONDS) -> float:
    """Block work retargeting settles on, floor-clamped.

    Retargeting drives block time to `spacing`, which needs rate*spacing cycles
    of work -- unless that is below the floor, in which case the floor binds and
    blocks come in faster than target forever.
    """
    return max(float(floor_cycles), rate * spacing)


def retarget_step(actual_block_seconds: float,
                  spacing: int = TARGET_SPACING_SECONDS,
                  clamp: int = RETARGET_CLAMP,
                  observed_spacings: int = RETARGET_WINDOW_BLOCKS) -> tuple:
    """Difficulty multiplier the next retarget wants, and whether it is clamped.

    Returns (step, clamped). step > 1 means difficulty rises (blocks are coming
    in fast). The target timespan is always 144 spacings, but the first retarget
    can observe only the 143 spacings from genesis through block 143. Later
    retargets observe all 144 after F-147. src/pow.cpp clamps the measured
    timespan to [target/4, target*4].
    """
    target_timespan = spacing * RETARGET_WINDOW_BLOCKS
    actual_timespan = actual_block_seconds * observed_spacings
    step = target_timespan / actual_timespan
    return step, not (1 / clamp <= step <= clamp)


def degraded_recovery(rate_before: float, rate_after: float,
                      floor_cycles: int = LAUNCH_FLOOR_CYCLES,
                      window: int = RETARGET_WINDOW_BLOCKS,
                      spacing: int = TARGET_SPACING_SECONDS,
                      clamp: int = RETARGET_CLAMP,
                      max_epochs: int = 16) -> tuple:
    """Days of slow blocks after hashrate drops, and where block time settles.

    The retarget window is counted in BLOCKS, not time, so a chain running at
    6x its target spacing takes 6x as long to reach its next adjustment. That
    coupling is why arming an extra card lengthens this: it raises the
    steady-state difficulty the survivor has to dig out from, while the floor
    it eventually lands on is unchanged.

    Returns (days_degraded, settled_block_seconds).
    """
    work = steady_state_work(rate_before, floor_cycles, spacing)
    elapsed = 0.0
    for _ in range(max_epochs):
        seconds = block_seconds(work, rate_after)
        step, _clamped = retarget_step(seconds, spacing, clamp)
        applied = min(max(step, 1 / clamp), clamp)
        following = max(float(floor_cycles), work * applied)
        if following == work:
            return elapsed / 86400, seconds
        elapsed += window * seconds
        work = following
    return elapsed / 86400, block_seconds(work, rate_after)


def required_tx_work(mean_block_work: float, k: int = K) -> int:
    """RequiredTxWork at congestion 1.0: mean block work // K, floored at 1."""
    return max(1, int(mean_block_work) // k)


def tx_floor_release_rate(k: int = K,
                          spacing: int = TARGET_SPACING_SECONDS) -> float:
    """Hashrate (cycles/s) above which the 1-cycle tx-work floor stops binding.

    This is a hashrate threshold, not a ramp that expires with height.
    """
    return k / spacing


def report(cycles_per_second_: float, floor_cycles: int, ma_window: int) -> None:
    target = target_for_cycles(floor_cycles)
    print(f"floor: {floor_cycles} cycles/block")
    print(f"  target        : {target:#066x}")
    print(f"  nBits         : {compact_from_target(target):#010x}")
    print(f"  round-trip    : {cycles_for_target(target)} cycles/block")
    print(f"  block time @ {cycles_per_second_:.2f} cycles/s (1 GPU): "
          f"{floor_cycles / cycles_per_second_:.1f}s")

    baseline_tx_work = required_tx_work(floor_cycles)
    print(f"  tx work at floor: {baseline_tx_work} cycles "
          f"({baseline_tx_work / cycles_per_second_:.2f}s of grind)")
    print(f"  tx work at height 0 (no anchor): 1 cycle "
          f"({1 / cycles_per_second_:.2f}s of grind)")
    print(f"  MA window {ma_window} blocks = "
          f"{ma_window * TARGET_SPACING_SECONDS / 3600:.1f}h to fully populate")


def report_fleet(cards: dict, floor_cycles: int) -> None:
    """The floor's behaviour against a condition-stated arming set."""
    print(f"quiet arming set: {len(cards)} card(s), "
          f"floor {floor_cycles} cycles/block")
    print(f"  point cycle yield: {POOLED_CYCLES_PER_GRAPH:.5f} cycles/graph")
    for name, spg in cards.items():
        alone = block_seconds(floor_cycles, cycles_per_second(spg))
        print(f"  {name:<13} {spg:>8.4f} s/graph  "
              f"alone at the floor: {alone:>7.1f}s "
              f"({alone / TARGET_SPACING_SECONDS:.2f}x target)")

    rate = aggregate_rate(cards)
    opening = block_seconds(floor_cycles, rate)
    step, clamped = retarget_step(
        opening, observed_spacings=FIRST_RETARGET_SPACINGS)
    print(f"  aggregate    : {rate:.7f} cycles/s")
    print(f"  opens at     : {opening:.1f}s "
          f"({opening / TARGET_SPACING_SECONDS:.2f}x target)")
    print(f"  first retarget after "
          f"{FIRST_RETARGET_SPACINGS * opening / 3600:.2f}h "
          f"({FIRST_RETARGET_SPACINGS} observed spacings), "
          f"step {step:.2f}x"
          f"{' -- CLAMPED, needs a second epoch' if clamped else ''}")
    steady = steady_state_work(rate, floor_cycles)
    print(f"  steady work  : {steady:.2f} cycles "
          f"-> RequiredTxWork {required_tx_work(steady)} cycle")

    slowest = min(cards, key=lambda c: cycles_per_second(cards[c]))
    days, settled = degraded_recovery(
        rate, cycles_per_second(cards[slowest]), floor_cycles=floor_cycles)
    print(f"  clean survivor ({slowest} alone): {days:.2f} days to settle, "
          f"settles at {settled:.0f}s/block")

    # Conservative scenario, not a prediction or a confidence interval for
    # hardware health: the survivor crosses the deployed +25% graph-time health
    # boundary, while cycle yield sits at M1+M2's lower 95% bound. Apply the
    # yield bound to both pre-loss and post-loss rates because it is one global
    # property of Cuckatoo graphs, not a survivor-only degradation.
    conservative_before = aggregate_rate(cards, LOWER_95_CYCLES_PER_GRAPH)
    conservative_after = cycles_per_second(
        cards[slowest] * DEGRADED_GRAPH_TIME_MULTIPLIER,
        LOWER_95_CYCLES_PER_GRAPH)
    days, settled = degraded_recovery(
        conservative_before, conservative_after, floor_cycles=floor_cycles)
    print(f"  conservative survivor ({slowest}, +25% graph time, "
          f"lower-95% cycle yield): {days:.2f} days to settle, "
          f"settles at {settled:.0f}s/block")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cycles-per-second", type=float,
                        help="measured single-GPU cycle throughput (Step 1)")
    parser.add_argument("--ma-window", type=int, default=2016)
    parser.add_argument("--floors", type=int, nargs="+",
                        default=[1, 100, 600, 1200, 2400, 12000])
    parser.add_argument("--fleet", action="store_true",
                        help="report the launch arming set instead of one anchor")
    args = parser.parse_args()

    if args.fleet:
        for floor in args.floors:
            report_fleet(LAUNCH_FLEET_QUIET, floor)
            print()
        return

    if args.cycles_per_second is None:
        parser.error("--cycles-per-second is required unless --fleet is given")

    for floor in args.floors:
        report(args.cycles_per_second, floor, args.ma_window)
        print()


if __name__ == "__main__":
    main()
