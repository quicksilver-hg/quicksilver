#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Unit tests for the calibration model. Run: python3 -m pytest test_model.py"""

import model


def test_fill_ratio_reproduces_the_spec_finding():
    # Spec finding 3.3: above the floor, fill cost / mine cost settles at n_max/K.
    # n_max = 4_000_000 / 400_000 = 10, K = 106 -> 0.094.
    ratio = model.fill_to_mine_ratio(
        block_weight=4_000_000, max_tx_weight=400_000, k=106, mean_block_work=1060)
    assert abs(ratio - 10 / 106) < 1e-9


def test_floor_binds_below_the_threshold():
    # At the launch floor, base = 2 // 106 = 0 -> clamped to 1 cycle per tx.
    # Filling costs 10 cycles against 2 to mine: 5x, the defended case.
    ratio = model.fill_to_mine_ratio(
        block_weight=4_000_000, max_tx_weight=400_000, k=106, mean_block_work=2)
    assert abs(ratio - 5.0) < 1e-9


def test_size_term_takes_the_ratio_out_of_the_attackers_hands():
    # Today the attacker picks their transaction size, so they pick n_max. With
    # work charged per byte against a protocol-chosen reference weight, slicing
    # stops helping: the ratio no longer depends on max_tx_weight at all.
    a = model.fill_to_mine_ratio(
        block_weight=4_000_000, max_tx_weight=400_000, k=106,
        mean_block_work=1060, reference_weight=400_000)
    b = model.fill_to_mine_ratio(
        block_weight=4_000_000, max_tx_weight=4_000, k=106,
        mean_block_work=1060, reference_weight=400_000)
    assert abs(a - b) < 1e-9


def test_reference_weight_is_the_lever_that_satisfies_criterion_1():
    # Criterion 1 needs fill/mine >= 1. With a size term the protocol sets the
    # multiplier directly: reference_weight = block_weight / K reaches parity.
    ratio = model.fill_to_mine_ratio(
        block_weight=4_000_000, max_tx_weight=400_000, k=106,
        mean_block_work=1060, reference_weight=4_000_000 / 106)
    assert ratio >= 1.0


def test_growth_uses_measured_disk_cost_not_block_bytes():
    # 4 MB blocks at 288/day is 420 GB/yr counting block bytes alone; the
    # measured per-block cost includes undo, index, and chainstate.
    gb = model.growth_gb_per_year(bytes_per_block=5_000_000, spacing_seconds=300)
    assert 500 < gb < 530


# --- Tests added from what M4 and M5 actually measured -----------------------


def test_measured_growth_reproduces_the_m5_total():
    # M5: 4,055,004 bytes/block at 300s spacing. The spec's headline is 420 GB/yr.
    # These agree to ~1.5% while being built from different parts, which is the
    # coincidence M5 documents — pinned here so a later model change that
    # "corrects" one of them has to confront the other.
    gb = model.growth_gb_per_year(bytes_per_block=4_055_004, spacing_seconds=300)
    assert 420 < gb < 432


def test_honest_and_adversarial_growth_straddle_the_criterion_2_ceiling():
    # M5: blocks+undo alone is 992,732 bytes/block; the UTXO-heavy total is
    # 4,055,004. Criterion 2's ceiling is 200 GB/year. Which side the design
    # lands on depends on whether UTXO growth is priced, so the model must not
    # collapse these into one number.
    honest = model.growth_gb_per_year(bytes_per_block=992_732, spacing_seconds=300)
    adversarial = model.growth_gb_per_year(bytes_per_block=4_055_004,
                                           spacing_seconds=300)
    assert honest < 200 < adversarial


def test_mint_per_block_is_weight_bound_not_size_bound():
    # M4: the lightest transaction is 1,119 weight, giving 3,574 per block and
    # 2.042 COIN against the 2.00 design intent. Dividing the 4 MB SERIALIZED cap
    # by 309 bytes instead gives 12,944 per block and a fictitious 3.7x drift.
    # consensus.h calls MAX_BLOCK_SERIALIZED_SIZE "only for buffer size limits";
    # the network rule is GetBlockWeight > MAX_BLOCK_WEIGHT.
    coin = model.mint_per_block_coin(block_weight=4_000_000,
                                     lightest_tx_weight=1_119,
                                     ntxpowmint_cinnabar=57_143)
    assert abs(coin - 2.042) < 0.001


def test_mint_span_across_transaction_shapes_is_enormous():
    # M4: 3,574 tx/block at the light end against 10 at the heavy end. Criterion
    # 4 asks that issuance not depend on transaction size; it does, by ~357x.
    light = model.mint_per_block_coin(4_000_000, 1_119, 57_143)
    heavy = model.mint_per_block_coin(4_000_000, 399_999, 57_143)
    assert light / heavy > 300


def test_measured_e29_grind_lands_in_the_spec_band():
    # M1: 49.6925 s per graph at 8 CPU threads, pooled cycle rate 0.02289.
    # The spec records 33-41 min per transaction; reproducing it from parts
    # measured here is what licenses using this machine for the other sizes.
    seconds = model.grind_seconds_per_tx(seconds_per_graph=49.6925,
                                         cycle_rate=0.02289)
    assert 33 * 60 < seconds < 41 * 60


def test_one_cycle_rate_serves_every_edge_bits():
    # M1: the 42-cycle rate per graph is statistically independent of graph size
    # (chi-square 4.03 on 7 dof). So the ratio of grind costs between two
    # candidate sizes is just the ratio of their per-graph times, and the flag
    # day can choose nTxEdgeBits from the timing table alone.
    rate = 0.02289
    e27 = model.grind_seconds_per_tx(9.0369, rate)
    e29 = model.grind_seconds_per_tx(49.6925, rate)
    assert abs(e29 / e27 - 49.6925 / 9.0369) < 1e-9


# --- Criterion 3: mint safety under a changed nTxEdgeBits ---------------------


def test_mint_safety_reproduces_the_audit_doc_at_r_one():
    # doc/audit/mainnet-difficulty-floor-model.md:217-218 records, for r = 1:
    #   K = 106 -> 0.0606 (16.5x margin); K = 437 -> 0.2497 (the alpha limit).
    assert abs(model.mint_safety_ratio(k=106, r=1.0) - 0.0606) < 0.0001
    assert abs(model.mint_safety_ratio(k=437, r=1.0) - 0.2497) < 0.0001


def test_k_ceiling_at_r_one_is_the_documented_437():
    assert abs(model.max_k_for_mint_safety(r=1.0) - 437.5) < 0.1


def test_a_faster_tx_solver_TIGHTENS_the_k_ceiling():
    # THE contested direction. doc/audit/mainnet-difficulty-floor-model.md:196
    # writes K = alpha * r * S_tail / C, which puts r in the NUMERATOR and so
    # makes a faster tx solver permit a LARGER K -- less work per transaction.
    # That is backwards: a cheaper tx cycle must be compensated by demanding MORE
    # cycles, not fewer. Both forms agree at r = 1, which is why the error is
    # invisible today and becomes load-bearing the moment nTxEdgeBits moves.
    assert model.max_k_for_mint_safety(r=4.37) < model.max_k_for_mint_safety(r=1.0)


def test_a_nearly_free_tx_solver_demands_more_work_than_a_block():
    # Reductio that decides the direction. Per-tx PoW at E19 against block PoW at
    # E29 is measured at r = 703: minting by transaction is ~700x cheaper per
    # unit real work. Safety then requires K < 1, i.e. RequiredTxWork = W/K > W
    # -- one transaction must cost more than one block. Under the doc's form the
    # ceiling rises to ~307,000, making tx work ~0: free minting.
    assert model.max_k_for_mint_safety(r=703.0) < 1.0


def test_current_k_of_106_admits_only_a_marginally_faster_tx_solver():
    # K = 106 is shipped. The ceiling it implies on r is 437.5/106 = 4.13, so a
    # candidate whose tx solver is more than ~4.1x faster than the block solver
    # violates mint safety without also lowering K.
    assert abs(model.max_r_for_k(k=106) - 4.127) < 0.01


def test_solver_rate_ratio_is_block_time_over_tx_time():
    # r is the tx-solver rate relative to the block-solver rate, so a tx solver
    # that takes half as long per graph runs at twice the rate.
    assert abs(model.solver_rate_ratio(seconds_per_graph_tx=1.1596,
                                       seconds_per_graph_block=2.3907) - 2.062) < 0.001


def test_fill_to_mine_accounts_for_cheaper_transaction_cycles():
    # Criterion 1 compares real cost, not cycle counts. Cycle counts are only a
    # fair comparison because the E29 unification makes a tx cycle and a block
    # cycle the same work; at r != 1 the attacker's cycles are cheaper than the
    # miner's and the ratio must be divided by r.
    # Exact parity is at reference_weight = block_weight / K = 37,735.85, so the
    # consensus constant must round DOWN. 37,736 -- the nearest integer -- gives
    # 0.999996 and fails criterion 1 by a hair. Rounding direction is a real
    # Stage 2 detail, not an artefact of this test.
    at_e29 = model.fill_to_mine_ratio(block_weight=4_000_000, max_tx_weight=400_000,
                                      k=106, mean_block_work=1060,
                                      reference_weight=37_735, r=1.0)
    at_e28 = model.fill_to_mine_ratio(block_weight=4_000_000, max_tx_weight=400_000,
                                      k=106, mean_block_work=1060,
                                      reference_weight=37_735, r=2.062)
    assert at_e29 >= 1.0
    assert abs(at_e28 - at_e29 / 2.062) < 1e-9


def test_rounding_the_reference_weight_up_breaks_parity():
    # Guards the lesson above: one weight unit too generous and criterion 1 is
    # no longer met.
    too_big = model.fill_to_mine_ratio(block_weight=4_000_000, max_tx_weight=400_000,
                                       k=106, mean_block_work=1060,
                                       reference_weight=37_736, r=1.0)
    assert too_big < 1.0


def test_reference_weight_is_bound_by_integer_division_not_the_asymptote():
    # The asymptotic parity point B/(K*r) is too generous. RequiredTxWork floors
    # mean_block_work // K, and the worst case is W = 2K-1 = 211, where the
    # quotient is 1 but W is nearly 2K -- almost half the work is lost to the
    # floor. The true bound is B/((2K-1)*r), about half the asymptote.
    b = model.max_reference_weight(block_weight=2_000_000, k=106, r=1.0)
    assert abs(b - 2_000_000 / 211) < 0.01
    # It sits just above half the asymptote: B/(2K-1) > B/(2K), and well under
    # the asymptotic B/K that the closed form would suggest.
    assert 2_000_000 / 212 < b < 2_000_000 / 106


def test_the_bound_holds_across_the_whole_criterion_1_range():
    # Criterion 1 is evaluated from the launch floor of 2 cycles to 100,000.
    # A reference weight at the bound must not dip below parity anywhere in it.
    r_ref = int(model.max_reference_weight(block_weight=2_000_000, k=106, r=1.0))
    worst = min(
        model.fill_to_mine_ratio(2_000_000, 400_000, 106, w, reference_weight=r_ref)
        for w in range(2, 100_001)
    )
    assert worst >= 1.0


def test_the_asymptotic_reference_weight_actually_fails():
    # Guards the correction: using B/(K*r) dips under parity inside the range.
    worst = min(
        model.fill_to_mine_ratio(2_000_000, 400_000, 106, w, reference_weight=18_867)
        for w in range(2, 100_001)
    )
    assert worst < 1.0


def test_grind_seconds_rejects_an_impossible_cycle_rate():
    # A cycle rate of zero means no cycle was ever found at that size. Returning
    # infinity would quietly propagate into the feasible region as a very large
    # but finite grind cost.
    try:
        model.grind_seconds_per_tx(seconds_per_graph=1.0, cycle_rate=0.0)
    except ValueError:
        return
    assert False, "a zero cycle rate must raise, not return a number"


# --- Stage 2: byte-denominated pricing -------------------------------------
#
# The size term moves from weight against R = 18,957 to WITH-WITNESS serialized
# bytes against R_b = R/4. These tests fix the arithmetic consensus will run,
# including the rounding direction, which the design left unstated and which
# turns out to be worth a factor of two.

K = 106
#: 4 MB of block weight at the attacker's cheapest shape, which is zero-witness.
BLOCK_BYTES = 1_000_000


def test_byte_reference_is_the_weight_reference_over_four():
    # bytes <= weight <= 4*bytes, so charging bytes at R/4 charges at least what
    # the weight term charged everywhere, and exactly the same at zero witness.
    assert model.R_B == 18_957 // 4 == 4739


def test_flooring_lets_a_slicing_attacker_halve_the_fill_cost():
    # The binding case from Stage 1: W = 2K - 1, where base floors to 1.
    w = 2 * K - 1
    whole_block = model.fill_to_mine_ratio_sliced(
        BLOCK_BYTES, K, w, slice_bytes=BLOCK_BYTES, round_up=False)
    assert abs(whole_block - 1.0) < 0.01

    # The attacker's best slice under FLOOR division is 2*R_b - 1 bytes: each
    # transaction floors to one unit of base while carrying nearly two.
    sliced = model.fill_to_mine_ratio_sliced(
        BLOCK_BYTES, K, w, slice_bytes=2 * model.R_B - 1, round_up=False)
    assert sliced < 0.51, f"floor division must be shown to fail, got {sliced}"


def test_ceiling_division_closes_the_slicing_hole_at_every_slice():
    w = 2 * K - 1
    for slice_bytes in (237, 4738, 4739, 9477, 9478, 100_000, BLOCK_BYTES):
        ratio = model.fill_to_mine_ratio_sliced(
            BLOCK_BYTES, K, w, slice_bytes=slice_bytes, round_up=True)
        assert ratio >= 1.0, f"slice {slice_bytes} gives {ratio}"


def test_criterion_1_holds_across_the_whole_range_under_ceiling_division():
    # Criterion 1 as committed in spec section 6: mean block work from the launch
    # floor of 2 to 100,000, evaluated at the attacker's best slice at each point.
    slices = (237, 4738, 4739, 9477, 9478, 100_000, BLOCK_BYTES)
    violations = [
        (w, s)
        for w in range(2, 100_001)
        for s in slices
        if model.fill_to_mine_ratio_sliced(BLOCK_BYTES, K, w, slice_bytes=s,
                                           round_up=True) < 1.0
    ]
    assert violations == []


def test_utxo_term_is_zero_clamped():
    # A consolidating transaction pays the byte term only -- never a refund.
    consolidating = model.tx_required_work(base=1000, tx_bytes=4739, utxo_delta=-19)
    byte_only = model.tx_required_work(base=1000, tx_bytes=4739, utxo_delta=0)
    assert consolidating == byte_only == 1000


def test_u_of_50_meets_criterion_2_with_margin():
    # Criterion 2's ceiling is 200 GB/year, and the spec directs that it be
    # treated as an upper bound to BEAT, not a target to meet.
    growth = model.adversarial_growth_gb_per_year(k=K, u=model.U)
    assert growth <= 100.0, f"U={model.U} gives {growth} GB/year"


def test_u_of_50_leaves_honest_two_output_transactions_alone():
    # At the launch floor the one-cycle floor absorbs the whole charge.
    assert model.tx_required_work(base=1, tx_bytes=model.HONEST_TX_BYTES,
                                  utxo_delta=1) == 1
    # And at any base, the UTXO term stays a minority of an honest payment's bill.
    share = model.utxo_term_share(model.HONEST_TX_BYTES, utxo_delta=1, u=model.U)
    assert share <= 0.5, f"honest senders carry a {share:.2f} UTXO surcharge"


def test_u_of_50_makes_the_utxo_term_dominate_for_bloat():
    # The term exists to price UTXO creation, so on M5's adversarial shape it must
    # be the larger half of the bill, not a rounding decoration.
    share = model.utxo_term_share(model.ADVERSARIAL_TX_BYTES,
                                  utxo_delta=model.ADVERSARIAL_OUTPUTS - 1,
                                  u=model.U)
    assert share >= 1.0, f"UTXO term is only {share:.2f} of the byte term"


def test_the_admissible_u_window_contains_50_strictly():
    # Both ends are measured, not chosen: below the window the UTXO term burdens
    # honest senders, above it bloat goes unpriced. This reproduces the 30-100
    # window feasible-region.md reports, from the same two constraints.
    admissible = [
        u for u in range(2, 501)
        if model.utxo_term_share(model.HONEST_TX_BYTES, 1, u) <= 0.5
        and model.utxo_term_share(model.ADVERSARIAL_TX_BYTES,
                                  model.ADVERSARIAL_OUTPUTS - 1, u) >= 1.0
        and model.adversarial_growth_gb_per_year(k=K, u=u) <= 200.0
    ]
    assert min(admissible) < model.U < max(admissible)
    # And the window is the one Stage 1 reported, not a different one.
    assert 25 <= min(admissible) <= 35, f"lower bound moved to {min(admissible)}"
    assert 100 <= max(admissible) <= 115, f"upper bound moved to {max(admissible)}"
