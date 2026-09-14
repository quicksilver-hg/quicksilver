# Calibration Gate

The transaction-work calibration gate measured production EDGEBITS=29 solving on
commodity GPU hardware and held the existing coupling and mint parameters.

> **Note (2026-08-02).** The production size is now **28**, not 29 — both graph
> sizes moved together in the E28 flag day. This document records the gate as it
> was run at E29; the measurement stands, the size label is historical.

## Finding

The mean CUDA solver was not inherently incompatible with the measured Pascal
hardware. Earlier failures came from the wrong solver path or build assumptions,
not from a consensus limitation.

Cross-architecture measurements on later GPU hardware confirmed the same order
of magnitude and did not justify changing the already-selected transaction work
coupling or mint magnitude.

## Decision

The calibration gate closed with no consensus parameter change:

- `nTxWorkCouplingK` held at the existing value;
- `nTxPowMint` held at the existing calibrated value;
- the work-based admission and ranking model stayed unchanged.

## Artifacts

The detailed raw measurements remain local research artifacts. Public consensus
docs record the resulting behavior rather than the operator-specific run logs.
