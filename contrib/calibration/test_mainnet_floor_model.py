#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Unit tests for the launch difficulty floor model.

Run: python3 -m pytest test_mainnet_floor_model.py

These pin the model to the shipped consensus constants and to F-174's four
final-slot, condition-stated quiet medians. The figures are not round numbers;
they are measurements, and the assertions are tight on purpose. A change that
moves them requires the difficulty-floor decision to be revisited.
"""

import mainnet_floor_model as m

# src/kernel/chainparams.cpp, main and publictest.
SHIPPED_POW_LIMIT = int(
    "3fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 16)
SHIPPED_NBITS = 0x203FFFFF
SHIPPED_FLOOR_CYCLES = 4


def test_model_reproduces_the_shipped_powlimit():
    # The model is only worth anything if it agrees with what actually ships.
    # powLimit -> nBits -> cycles must round-trip to the 4-cycle floor.
    assert m.compact_from_target(SHIPPED_POW_LIMIT) == SHIPPED_NBITS
    assert m.cycles_for_target(SHIPPED_POW_LIMIT) == SHIPPED_FLOOR_CYCLES
    assert m.target_for_cycles(SHIPPED_FLOOR_CYCLES) == SHIPPED_POW_LIMIT


def test_pooled_constant_reproduces_the_floor_docs_anchor():
    # doc/audit section "Settled 2026-08-07": the P104-100 anchor is
    # 0.014935 cycles/s at E28, giving the doc's published 267.8 s.
    # That anchor came from the six-GPU rig; the M8 measurement of an
    # uncontended P104 is faster. Both must fall out of the same conversion.
    assert abs(m.cycles_per_second(1.5648) - 0.014935) < 5e-7
    assert abs(m.block_seconds(SHIPPED_FLOOR_CYCLES, 0.014935) - 267.8) < 0.05


def test_f174_inputs_are_the_final_quiet_medians():
    assert m.LAUNCH_FLEET_QUIET == {
        "P104-100": 1.4667,
        "GTX 950": 3.4383,
        "GTX 1050 Ti": 3.1912,
        "GTX 960": 3.2093,
    }


def test_four_card_quiet_arming_set_opens_at_107_seconds():
    rate = m.aggregate_rate(m.LAUNCH_FLEET_QUIET)
    assert abs(rate - 0.0373359) < 5e-7
    assert abs(m.block_seconds(SHIPPED_FLOOR_CYCLES, rate) - 107.1) < 0.05


def test_four_cards_open_inside_the_first_retarget_clamp():
    # Block 144 sees only 143 spacings back to genesis. Later F-147 periods see
    # 144. The opening still reaches target in one epoch at the shipped floor.
    rate = m.aggregate_rate(m.LAUNCH_FLEET_QUIET)
    step, clamped = m.retarget_step(
        m.block_seconds(SHIPPED_FLOOR_CYCLES, rate),
        observed_spacings=m.FIRST_RETARGET_SPACINGS)
    assert not clamped
    assert abs(step - 2.820) < 0.001


def test_later_retarget_observes_the_full_interval():
    step, clamped = m.retarget_step(m.TARGET_SPACING_SECONDS)
    assert not clamped
    assert step == 1.0


def test_halving_the_floor_would_now_clamp_the_opening():
    # At 2 cycles the quiet four-card set opens 5.64x fast. The first retarget
    # clamps at 4x and needs a second epoch to converge.
    rate = m.aggregate_rate(m.LAUNCH_FLEET_QUIET)
    step, clamped = m.retarget_step(
        m.block_seconds(2, rate),
        observed_spacings=m.FIRST_RETARGET_SPACINGS)
    assert clamped
    assert abs(step - 5.640) < 0.001


def test_clean_gtx_950_is_the_slowest_survivor():
    cards = m.LAUNCH_FLEET_QUIET
    slowest = min(cards, key=lambda c: m.cycles_per_second(cards[c]))
    assert slowest == "GTX 950"

    survivor_rate = m.cycles_per_second(cards[slowest])
    days, settled = m.degraded_recovery(
        m.aggregate_rate(cards), survivor_rate)
    assert abs(days - 2.75) < 0.01
    assert abs(settled - 588.5) < 0.05


def test_conservative_survivor_is_explicit_not_hidden_in_the_input():
    # This scenario combines the deployed +25% graph-time health boundary with
    # the pooled cycle experiment's lower 95% yield. The yield bound applies to
    # the whole chain; the +25% operational slowdown applies only after loss.
    cards = m.LAUNCH_FLEET_QUIET
    slowest = "GTX 950"
    before = m.aggregate_rate(cards, m.LOWER_95_CYCLES_PER_GRAPH)
    after = m.cycles_per_second(
        cards[slowest] * m.DEGRADED_GRAPH_TIME_MULTIPLIER,
        m.LOWER_95_CYCLES_PER_GRAPH)
    days, settled = m.degraded_recovery(before, after)
    assert abs(days - 3.43) < 0.01
    assert abs(settled - 809.4) < 0.05


def test_tx_work_sits_at_its_floor_in_every_configuration():
    # RequiredTxWork = mean block work // K, floored at 1. Nothing in the
    # four-card change comes near the K = 106 boundary.
    four = m.aggregate_rate(m.LAUNCH_FLEET_QUIET)
    slowest = m.cycles_per_second(m.LAUNCH_FLEET_QUIET["GTX 950"])
    conservative = m.aggregate_rate(
        m.LAUNCH_FLEET_QUIET, m.LOWER_95_CYCLES_PER_GRAPH)
    for work in (SHIPPED_FLOOR_CYCLES,
                 m.steady_state_work(four),
                 m.steady_state_work(slowest),
                 m.steady_state_work(conservative)):
        assert m.required_tx_work(work) == 1


def test_the_floor_stops_binding_tx_cost_far_above_the_fleet():
    # Hashrate at which mean block work exceeds K, so the 1-cycle floor stops
    # governing transaction cost. The fleet is nowhere near it.
    threshold = m.tx_floor_release_rate()
    assert abs(threshold - 0.35333) < 1e-5
    assert m.aggregate_rate(m.LAUNCH_FLEET_QUIET) < threshold / 9


def test_the_floors_stated_purpose_holds_only_for_the_p104():
    # Point estimates use the standardized quiet instrument. Only the headless
    # P104 lands near target; commodity survivors remain degraded-mode miners.
    alone = {c: m.block_seconds(SHIPPED_FLOOR_CYCLES, m.cycles_per_second(s))
             for c, s in m.LAUNCH_FLEET_QUIET.items()}
    assert abs(alone["P104-100"] - 251.0) < 0.05
    assert abs(alone["GTX 950"] - 588.5) < 0.05
    assert abs(alone["GTX 1050 Ti"] - 546.2) < 0.05
    assert abs(alone["GTX 960"] - 549.3) < 0.05
    # Only the P104 is within 25% of target; the three commodity cards are not.
    within_target = [c for c, t in alone.items() if abs(t - 300) / 300 <= 0.25]
    assert within_target == ["P104-100"]
