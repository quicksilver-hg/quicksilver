# PoW Selection

Quicksilver selected Cuckatoo for both block PoW and transaction PoW after
comparing it with an Equihash-family candidate.

## Result

Cuckatoo passed the selection gates:

- median verification near 1.2 microseconds in the measured EDGEBITS=19 harness;
- 168-byte proof size from 42 32-bit edge indices;
- verification cost and proof size independent of EDGEBITS for supported graph
  sizes;
- create-to-verify cost ratio far above the required floor.

The Equihash-family candidate was rejected because its proof was about 2048 bytes
and verifier memory was about 140 MB in the tested parameter set. Those failures
were structural for the candidate and not appropriate for per-transaction proofs.

## Production Create Cost

Production-scale EDGEBITS=29 creation was measured on Pascal-era GPU hardware.
(The production size moved to **28** on 2026-08-02; this measurement is the E29
record that preceded it, and the conclusion below is unaffected — a smaller graph
is strictly faster to create and no more costly to verify.)
Mean CUDA solving produced per-graph times around 0.12 to 0.13 seconds on the
measured cards, with proof discovery requiring multiple graphs. This put
transaction PoW creation in the intended seconds-to-tens-of-seconds range while
keeping node verification cheap.

## Decision

Cuckatoo remains the PoW primitive. Create work is intentionally paid by miners
or transaction producers; verification must remain cheap for every node.
