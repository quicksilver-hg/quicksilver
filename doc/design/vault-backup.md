# Vault Backup

> Status: **decided and built** (2026-08-04). A user-triggered encrypted archive of the
> vault's history ships in v1, with a restore path. Automatic backup does not ship, and
> the reason is below.

## 1. Why this exists

A thin vault cannot rescan. That single fact is the whole problem.

In a vault that scans the chain, the chain can reconstruct history after its keys are
restored. Quicksilver's vault follows headers and stores no chain history
([agent-client.md](agent-client.md)), so there is nothing to rescan against. The
recovery paths that actually ship recover different things:

| mechanism | recovers | does not recover |
| --- | --- | --- |
| vault-file backup | keys and history present when the backup was made | later activity |
| `scantxoutset` | current balance for keys the vault still has | lost keys or any history |
| **vault archive** | full transaction history, labels, descriptors | keys |

The application does not issue a recovery phrase. A current vault-file backup is the
only complete recovery artifact; the history archive is deliberately narrower. That is
why the desktop treats backup as a standing banner rather than a settings item.

## 2. What ships

Two RPCs, and the mechanism behind them:

    exportvaultarchive "destination" "passphrase"
    importvaultarchive "source" "passphrase"

The archive contains:

- **Descriptors, public form only** — enough to recognise the vault's own outputs.
- **The address book** — labels and purposes.
- **The full transaction history** — every transaction with its confirmation state,
  receipt time, and ordering.
- **The vault birth time**, so a restored vault knows how far back it matters.

It contains **no private keys**. Keys are the vault-file backup's job. An archive holding
both would turn a history backup into a second copy of the vault's secrets, stored
wherever users store backups. Unit and functional tests both assert that no extended
private key appears anywhere in an archive file.

It is encrypted anyway (AES-256-CBC, key derived from the passphrase over 200,000
rounds), because a complete payment history is disclosive even when it cannot spend. The
body is prefixed with its own hash before encryption, so a wrong passphrase is reported
as a wrong passphrase rather than surfacing as a corrupt file — those need different
things from the user.

Writes go to a temporary file and are renamed into place. Losing history is the failure
this feature exists to prevent; an interrupted export must not become that failure by
replacing a good archive with a truncated one.

## 3. The address book is history, not decoration

This was the non-obvious finding, and it is recorded because it would otherwise be
rediscovered the hard way.

`CAddressBookData` uses **the presence or absence of a label** to distinguish a change
address from a real one. A restored vault with no address book therefore treats every
address as change — and a self-payment, whose outputs are then all "change" and whose
inputs are all its own, produces **no history rows at all**. The first implementation
restored descriptors and transactions and looked correct: balances were right, most
transactions listed, and exactly the self-sends vanished.

So the address book is part of the archive, and the restore preserves the distinction
between "a label that is present but empty" (a real address with the default label) and
"no label at all" (change). Collapsing those two rewrites the user's history.

A second, smaller finding: `AddVaultDescriptor` does not activate a descriptor. An
inactive descriptor is treated as change and a restored vault would hand out addresses
from a fresh descriptor instead of the archived one, so the import restores the active
set the archive recorded.

## 4. What does not ship, and why

**Automatic backup.** Backing up on every new address is Bitcoin-era wallet behaviour
that silently produces stale copies: the user ends up with many archives, none current,
and no way to tell which is which. Export is explicit and user-triggered, and the desktop
keeps a standing banner rather than pretending the problem is solved.

**Restoring balance from the archive.** The archive is not a claim about what the user
owns; the UTXO set is. `scantxoutset` can rediscover outputs for keys a vault already
has, but neither the archive nor that scan can recreate lost keys.

## 5. What a restored vault is

Importing an archive into a vault with no keys produces a tracking-only history vault: it
can show everything that happened and spend nothing. To regain spending ability, restore
a vault-file backup. Importing a newer archive into that restored vault can then bring
its recorded history forward without introducing another copy of the private keys.

Importing the same archive twice is harmless. Descriptors and transactions already
present are skipped rather than overwritten, and the RPC reports what it skipped so the
user is told rather than left to infer it.

An archive names the chain and genesis hash it came from and refuses to restore onto a
different network, where the block heights it carries would refer to different blocks.

## 6. The vault file's silent format surface

Not everything the vault stores announces itself as a format. The active
ScriptPubKeyMan records key their descriptors by `OutputType` written as a raw
ordinal, and the loader casts the byte straight back to the enum. The enum
therefore *is* a stored format, and C++ gives no sign of it: renumbering a member
re-files every descriptor in every existing vault under some other address type,
and the vault still opens.

The values use a compact mapping established before launch and are pinned by
`outputtype_tests`. A mismatch is reported instead of thrown, so a vault written
by an incompatible build says which types disagree rather than taking the
application down.

The general point is worth keeping past this instance: **before removing or
reordering any enum, check whether it reaches a database key or a serialized
field.** A grep for the enum's name will not tell you — the write side takes a
`uint8_t`.

## 7. Trigger for revisiting

If users are found to be losing history *despite* the banner — that is, if explicit
export turns out to be a step people reliably skip — then the answer is a scheduled or
prompted export with clear staleness reporting, not silent automatic backup. Staleness is
the failure mode to design against, and it is the reason automatic backup was refused
here.

## 8. Related

- [agent-client.md](agent-client.md) — why the vault cannot rescan
- [desktop-application.md](desktop-application.md) — the standing backup banner
- [chain-storage.md](chain-storage.md) — who keeps the chain the vault does not
- [v1-scope.md](v1-scope.md) — what v1 does and does not do
