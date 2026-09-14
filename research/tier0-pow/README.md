# Tier-0 PoW Research Harness

This directory is frozen research provenance for Quicksilver Core. It is not
part of the Quicksilver Core build, release, or runtime path.

The harness benchmarked per-transaction proof-of-work verify/create asymmetry
for Cuckoo Cycle and Equihash, and it preserves the calibration context behind
consensus and economics constants including `K=106`, `C=57143`, and the tail
schedule. See the retained plans, reports, calibration inputs, and result
artifacts under `doc/audit/`, `tools/calibration/`, and `results/`.

## Reproduction

Reference implementations are fetched on demand rather than vendored in the
public product tree.

```sh
bash scripts/fetch_references.sh
make -C drivers/cuckoo
make -C drivers/equihash
python -m harness.cli --out results/
```

The script expects the local reference archives to be supplied outside Git.
Set `ZIPS=/path/to/reference-archives` when `cuckoo-master.zip` and
`equihash-master.zip` are not present in this directory.
