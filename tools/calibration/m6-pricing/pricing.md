# M6 — hardware and capacity pricing

All figures sourced and dated. Re-check if this sits unused for more than a month.

Read 2026-08-01. Prices are US, USD, and exclude tax and egress.

| Item | Price | Unit | Source | Date read |
| --- | --- | --- | --- | --- |
| Cloud GPU, current-gen (H100) | 2.01 | per GPU-hour, from | [Spheron GPU cloud pricing comparison 2026](https://www.spheron.network/blog/gpu-cloud-pricing-comparison-2026/) | 2026-08-01 |
| Cloud GPU, current-gen (A100 80GB) | 1.79 | per GPU-hour, neocloud median | [Cloud GPU Rental Price Index](https://aimultiple.com/gpu-index) | 2026-08-01 |
| Cloud GPU, prior-gen (RTX 4090) | 0.48 | per GPU-hour, median | [Cloud GPU Rental Price Index](https://aimultiple.com/gpu-index) | 2026-08-01 |
| Cloud GPU, prior-gen (RTX 4090) | 0.14 | per GPU-hour, spot floor | [RTX 4090 cloud pricing, 14+ providers](https://getdeploying.com/gpus/nvidia-rtx-4090) | 2026-08-01 |
| Cloud GPU, prior-gen (L40S) | 1.56 | per GPU-hour, median | [Cloud GPU Rental Price Index](https://aimultiple.com/gpu-index) | 2026-08-01 |
| Cloud vCPU, general purpose | 0.0504 | per vCPU-hour (m7i.large, $0.1008/hr ÷ 2 vCPU, us-east-1) | [m7i.large pricing and specs](https://instances.vantage.sh/aws/ec2/m7i.large) | 2026-08-01 |
| Used P102-100 (the card measured here) | 64.99 | outright | [eBay listing, P102-100 10GB](https://www.ebay.com/itm/156284588757) | 2026-08-01 |
| Retail 4 TB NVMe | 439.99 | outright, cheapest tracked | [4TB NVMe SSD prices](https://best-ssd.com/4tb-nvme-prices) | 2026-08-01 |
| Retail 4 TB NVMe | 577.50 | outright, median tracked | [4TB NVMe SSD prices](https://best-ssd.com/4tb-nvme-prices) | 2026-08-01 |
| Consumer NAND | 75–76 | per TB, 2026 plateau | [SSD price tracker, $/TB history](https://cheapestssd.com/ssd-price-tracker/) | 2026-08-01 |

## Notes

- The bloat attacker rents or buys whichever is cheaper per cycle; the model
  takes the minimum.
- Storage price bounds what criterion 2 is really asking a node operator to spend.

## Storage is getting MORE expensive, which criterion 2 did not assume

The usual planning assumption — storage gets cheaper every year, so chain growth
is a shrinking problem — does not hold as of this reading. NAND supply is being
allocated to AI and enterprise demand; consumer $/TB has sat on a $75–76 plateau
since early 2026, and the tracked forecasts are flat-to-rising with no new fab
capacity expected until late 2027
([SSD price tracker](https://cheapestssd.com/ssd-price-tracker/),
[NAND flash supply analysis](https://dropreference.com/en/blog/news/ssd-price-increase-2026-nand-flash-crisis)).

Consequence for the flag day: a growth rate that is tolerable only on the
assumption of falling storage cost should not be treated as tolerable. Combined
with M5's finding that adversarial growth is 426 GB/year, one year of
adversarial growth costs a node operator roughly **$32** in NAND at $75/TB, and a
four-year retention horizon costs **$128** — against $0 today. That is not
prohibitive, but it is a recurring cost that the design currently imposes on
every full node without pricing it anywhere.

## What cannot be computed from this table alone

Per-cycle attacker cost needs a price for the card the timings were taken on. The
P102-100 has an outright price here but **no rental market** — it is a
mining-only Pascal part that no cloud provider offers. So:

- **Outright** is directly computable: $64.99 buys a card that produces one
  42-cycle at E29 every 104 s (2.39 s/graph × 43.7 graphs/cycle), i.e. ~830
  cycles/day.
- **Rented** is not. The cheapest cloud GPU ($0.14/GPU-hour, RTX 4090 spot) is a
  much faster card, so using P102 timings with 4090 prices would overstate the
  attacker's cost by whatever the 4090's speedup is — a factor this calibration
  never measured.

**This is a real gap in the feasible region, not a rounding concern.** Criterion 1
asks whether filling a block costs at least as much as mining it, and both sides
of that comparison are attacker costs on attacker hardware. Closing it needs a
timing run on a rentable card. Until then the model should use the outright P102
figure and state that the rental path is unpriced, rather than substitute a
plausible-looking number.
