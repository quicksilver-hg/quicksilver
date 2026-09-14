# Public Identity Cleanup

This cleanup aligned public metadata with Quicksilver's current identity.

## Outcome

The repository-facing metadata now presents Quicksilver as the product:

- root project documentation and contribution guidance use Quicksilver naming;
- security reporting documentation points users to the current private reporting
  channel;
- package, desktop, installer, and generated configuration surfaces no longer
  advertise inherited product identity;
- generated examples were refreshed from the current daemon where available.

## Verification

The pass used targeted residue searches and the branding lint to distinguish
allowed upstream legal attribution from product-facing naming residue. Legal
copyright attribution was preserved.

Remaining generated GUI assets require a GUI-enabled build tree before all
manpages and translations can be regenerated together.
