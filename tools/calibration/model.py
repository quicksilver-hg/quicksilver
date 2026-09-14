#!/usr/bin/env python3
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Model for the transaction-cost and chain-growth flag day.

Reads the M1-M6 artifacts and reports which parameter combinations satisfy all
five decision criteria. See doc/design/chain-growth.md and
tools/calibration/stage2-byte-pricing.md for the public design and decision
record.
"""

SECONDS_PER_YEAR = 365 * 24 * 3600

#: Mint-safety margin. Minting by transaction must earn at most this fraction of
#: minting by mining, per unit of real work.
ALPHA = 0.25
#: Tail subsidy, in cinnabar (1 COIN).
S_TAIL_CINNABAR = 100_000_000
#: nTxPowMint: cinnabar minted per anchor-valid transaction.
NTXPOWMINT_CINNABAR = 57_143


def solver_rate_ratio(seconds_per_graph_tx, seconds_per_graph_block):
    """r: the transaction-solver rate relative to the block-solver rate.

    Rate is inverse to time per graph, so r = t_block / t_tx. The E29
    unification makes both solvers the same graph size, hence r = 1 by
    construction. Any change to nTxEdgeBits moves r away from 1.
    """
    if seconds_per_graph_tx <= 0:
        raise ValueError("time per graph must be positive")
    return seconds_per_graph_block / seconds_per_graph_tx


def mint_safety_ratio(k, r, mint_per_tx=NTXPOWMINT_CINNABAR,
                      s_tail=S_TAIL_CINNABAR):
    """Value per unit REAL work, minting by transaction against by mining.

    Criterion 3 requires this to stay at or below ALPHA.

    Derivation. Spend one second on each path. The transaction path completes
    K / (t_tx * W) transactions, each minting C. The mining path completes
    1 / (t_block * W) blocks, each minting S_tail. The ratio is

        C * K * t_block / (S_tail * t_tx) = C * K * r / S_tail

    so a FASTER transaction solver (larger r) makes minting by transaction more
    attractive and must be offset by a SMALLER K -- more work per transaction.

    NOTE: doc/audit/mainnet-difficulty-floor-model.md:196 writes the ceiling as
    K = alpha * r * S_tail / C, placing r in the numerator, which inverts this.
    Both forms agree at r = 1, so nothing shipped is affected; they diverge only
    once nTxEdgeBits moves. See test_model.py for the reductio that decides it.
    """
    return mint_per_tx * k * r / s_tail


def max_k_for_mint_safety(r, alpha=ALPHA, mint_per_tx=NTXPOWMINT_CINNABAR,
                          s_tail=S_TAIL_CINNABAR):
    """The largest K that keeps mint_safety_ratio at or below `alpha`."""
    if r <= 0:
        raise ValueError("solver rate ratio must be positive")
    return alpha * s_tail / (mint_per_tx * r)


def max_r_for_k(k, alpha=ALPHA, mint_per_tx=NTXPOWMINT_CINNABAR,
                s_tail=S_TAIL_CINNABAR):
    """The fastest transaction solver a given K tolerates.

    Read the other way round from max_k_for_mint_safety: with K fixed at its
    shipped value, this is the ceiling on how much cheaper per-transaction PoW
    may become before mint safety is violated.
    """
    if k <= 0:
        raise ValueError("K must be positive")
    return alpha * s_tail / (mint_per_tx * k)


def required_tx_work_cycles(mean_block_work, k):
    """RequiredTxWork: mean block work over K, integer-divided, floored at 1.

    The integer division is not incidental — it is why the congestion
    multiplier is inert at launch (spec finding 3.2).
    """
    return max(1, mean_block_work // k)


def fill_to_mine_ratio(block_weight, max_tx_weight, k, mean_block_work,
                       reference_weight=None, r=1.0):
    """Cost to fill a block to capacity, divided by cost to mine it.

    Criterion 1 requires this to be >= 1.

    `r` converts the attacker's transaction cycles into the miner's block
    cycles. Counting raw cycles on both sides is only fair because the E29
    unification makes them the same work; at r != 1 the attacker's cycles are
    cheaper by exactly r, so the ratio divides by it.

    Today (reference_weight=None) work is charged per transaction, so the
    attacker chooses the multiplier by choosing their transaction size. Their
    best choice is the largest transaction allowed, giving
    n_max = block_weight / max_tx_weight = 10 and a ratio of n_max / K.

    With a size term, work is charged per byte against a reference weight fixed
    by consensus. The multiplier becomes block_weight / reference_weight, which
    the attacker cannot influence — that, not the arithmetic, is the point.
    """
    if r <= 0:
        raise ValueError("solver rate ratio must be positive")
    per_tx = required_tx_work_cycles(mean_block_work, k)
    divisor = max_tx_weight if reference_weight is None else reference_weight
    n_eff = block_weight / divisor
    return (per_tx * n_eff) / (mean_block_work * r)


def max_reference_weight(block_weight, k, r=1.0, max_block_work=100_000):
    """Largest size-term reference weight that keeps fill ÷ mine at or above 1
    across criterion 1's whole range of mean block work.

    Not the asymptote. `RequiredTxWork` floors `mean_block_work // k`, and the
    worst case is `W = 2k - 1`: the quotient is 1 while W is nearly 2k, so almost
    half the intended work is lost to the floor. That makes the true bound
    `block_weight / ((2k - 1) * r)` — about HALF the asymptotic
    `block_weight / (k * r)`, which dips below parity inside the range.

    Computed by scanning rather than by the closed form, so that a later change
    to `required_tx_work_cycles` cannot silently invalidate it.
    """
    if r <= 0:
        raise ValueError("solver rate ratio must be positive")
    return min(block_weight * required_tx_work_cycles(w, k) / (w * r)
               for w in range(2, max_block_work + 1))


def growth_gb_per_year(bytes_per_block, spacing_seconds):
    """Worst-case chain growth, using MEASURED bytes per block (M5), which
    includes undo files, block index, and chainstate — not block bytes alone.

    M5 found chainstate is 75% of the total, so passing block bytes here
    understates an operator's real cost by about 4x.
    """
    blocks_per_year = SECONDS_PER_YEAR / spacing_seconds
    return bytes_per_block * blocks_per_year / 1e9


def mint_per_block_coin(block_weight, lightest_tx_weight, ntxpowmint_cinnabar):
    """Criterion 4: issuance must not depend on transaction shape.

    Design intent is 2.00 COIN at a full block.

    Capacity is bound by WEIGHT. consensus.h documents MAX_BLOCK_SERIALIZED_SIZE
    as "only for buffer size limits"; the network rule is
    GetBlockWeight(block) > MAX_BLOCK_WEIGHT. Dividing the serialized cap by
    transaction bytes instead overstates capacity ~3.6x and invents a mint drift
    that is not there — see tools/calibration/m4-tx-sizes/README.md.
    """
    if lightest_tx_weight <= 0:
        raise ValueError("transaction weight must be positive")
    txs_per_block = block_weight // lightest_tx_weight
    return txs_per_block * ntxpowmint_cinnabar / 1e8


def grind_seconds_per_tx(seconds_per_graph, cycle_rate):
    """Honest sender cost (criterion 5): expected graphs is 1/cycle_rate."""
    if cycle_rate <= 0:
        raise ValueError("cycle rate must be positive")
    return seconds_per_graph / cycle_rate


# --- Stage 2: byte-denominated pricing -------------------------------------

#: nTxWorkRefBytes: serialized bytes that cost one unit of base work.
#: Stage 1 fixed the weight reference at R = 18,957. bytes <= weight <= 4*bytes,
#: so R/4 against bytes charges at least what R against weight charged, and
#: exactly the same for a zero-witness transaction. Rounding down is the safe
#: direction: a smaller reference charges MORE work.
R_B = 18_957 // 4  # 4739

#: nTxUtxoRefCount: net new UTXOs that cost one unit of base work.
U = 50

#: M5, adversarial shape: marginal on-disk cost of one UTXO, in bytes.
BYTES_PER_UTXO = 121.1
#: M5, adversarial shape: 99,228 serialized bytes carrying 2,300 outputs.
ADVERSARIAL_TX_BYTES = 99_228
ADVERSARIAL_OUTPUTS = 2_300
BYTES_PER_OUTPUT = ADVERSARIAL_TX_BYTES / ADVERSARIAL_OUTPUTS  # 43.14

#: M5, honest shape: a two-output transaction measures 1,291 weight at 3.67
#: weight per serialized byte.
HONEST_TX_WEIGHT = 1_291
HONEST_WEIGHT_PER_BYTE = 3.67
HONEST_TX_BYTES = int(HONEST_TX_WEIGHT / HONEST_WEIGHT_PER_BYTE)  # 351


def tx_required_work(base, tx_bytes, utxo_delta, r_b=R_B, u=U, round_up=True):
    """The transaction-local charge, exactly as consensus computes it.

    ONE division, not two summed floored quotients -- summing loses precision
    twice, which is the class of defect behind spec finding 3.2 and the
    W = 2K-1 correction.

    `round_up` exists so the tests can DEMONSTRATE that flooring is unsafe
    rather than assert it. Consensus always rounds up: under floor division an
    attacker picks transactions of 2*r_b - 1 bytes, each charged one unit while
    carrying nearly two, and fills a block for half the honest price.

    The UTXO delta clamps at zero. Crediting a negative delta would refund 1/u
    against an input costing ~148/r_b of base, so at the low end of the
    admissible window adding inputs would LOWER required work.
    """
    if base <= 0:
        raise ValueError("base work must be positive")
    delta = max(0, utxo_delta)
    numerator = base * (tx_bytes * u + delta * r_b)
    denominator = r_b * u
    if round_up:
        return max(1, -(-numerator // denominator))
    return max(1, numerator // denominator)


def utxo_term_share(tx_bytes, utxo_delta, u=U, r_b=R_B):
    """The UTXO term as a multiple of the byte term, for one transaction shape.

    Both ends of the admissible window for `u` are read off this: honest senders
    need it well below 1 (the term must not become their bill), and the bloat
    shape needs it at or above 1 (the term must actually price what it exists to
    price). It is a pure shape ratio -- `base` cancels.
    """
    if tx_bytes <= 0:
        raise ValueError("transaction bytes must be positive")
    if u <= 0:
        raise ValueError("the UTXO reference must be positive")
    return (max(0, utxo_delta) / u) / (tx_bytes / r_b)


def fill_to_mine_ratio_sliced(block_bytes, k, mean_block_work, r_b=R_B,
                              slice_bytes=None, u=U, round_up=True):
    """Criterion 1 when the attacker chooses their transaction size.

    Differs from `fill_to_mine_ratio` in that it charges each slice through the
    integer arithmetic consensus actually runs, rather than treating the block as
    one continuous quantity. That per-transaction rounding is the whole question:
    the attacker slices their bytes into whichever sizes are cheapest for them,
    so the criterion must be evaluated at their best choice.

    The attacker's shape here is zero-witness and creates no UTXOs -- the
    cheapest bytes available -- so the UTXO term contributes nothing and `u`
    appears only through the shared denominator.
    """
    if slice_bytes is None or slice_bytes <= 0:
        slice_bytes = block_bytes
    base = required_tx_work_cycles(mean_block_work, k)
    whole, remainder = divmod(block_bytes, slice_bytes)
    cost = whole * tx_required_work(base, slice_bytes, 0, r_b, u, round_up)
    if remainder:
        cost += tx_required_work(base, remainder, 0, r_b, u, round_up)
    return cost / mean_block_work


def adversarial_growth_gb_per_year(k, r_b=R_B, u=U, spacing_seconds=300,
                                   bytes_per_output=BYTES_PER_OUTPUT,
                                   bytes_per_utxo=BYTES_PER_UTXO,
                                   block_bytes_cap=1_000_000):
    """Criterion 2: on-disk growth from an attacker paying criterion 1's floor.

    The attacker's budget is the cost of mining one block, W cycles. Spending it
    through the size and UTXO terms buys `s` bytes and `d` new UTXOs subject to

        s/r_b + d/u <= K            (work budget, since base = W/K)
        s <= block_bytes_cap        (the block weight cap, at zero witness)
        s >= bytes_per_output * d   (the bytes needed to carry d outputs)

    and what it costs an operator to keep is `s + bytes_per_utxo * d`. M5
    measured chainstate at 75-83% of real on-disk cost, blocks and undo at 24%,
    and the block index at 320 bytes per block, so UTXOs are the expensive part
    and the optimum is the all-output corner.
    """
    if u <= 0 or r_b <= 0 or k <= 0:
        raise ValueError("references and K must be positive")
    d = k / (bytes_per_output / r_b + 1.0 / u)
    s = bytes_per_output * d
    if s > block_bytes_cap:  # the block cap binds before the work budget does
        s = block_bytes_cap
        d = s / bytes_per_output
    return growth_gb_per_year(s + bytes_per_utxo * d, spacing_seconds)
