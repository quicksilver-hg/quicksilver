# Sandbox Block PoW

Sandbox block mining originally required real Cuckatoo-19 cycle discovery for
every block. That made ordinary 100-block test fixtures take minutes even though
the network target was easy.

## Finding

The slowdown was deterministic, finite, and not a deadlock. The expensive step
was finding a real 42-cycle for each local test block. Existing fixtures that
mine 100 maturity blocks multiplied that cost until combined suites exceeded
practical timeouts.

Transaction PoW was not the cause. Transaction proofs remained real and covered
the feature under test.

## Resolution

Sandbox gained a consensus parameter that allows block PoW to skip the real
cycle requirement while preserving the target check. The flag is set only for
sandbox. Public networks continue to require real Cuckatoo block cycles.

Transaction PoW remains real on sandbox, so relay pool admission, mint eligibility,
and transaction proof validation continue to exercise the actual per-transaction
path.

## Verification

The focused test that formerly stalled became practical, and the bring-up
functional test completed quickly while still checking mined transactions and
mint behavior. A dedicated PoW test pins that the relaxed path applies only when
the sandbox flag is enabled.
