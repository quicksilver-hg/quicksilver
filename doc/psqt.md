# PSQT Howto for Quicksilver

Quicksilver uses a Quicksilver-specific Partially Signed Quicksilver
Transaction (PSQT) identity for partially signed transaction workflows.

PSQT uses Quicksilver-specific `psqt\xff` magic and Quicksilver RPC/API names.

This document describes the overall workflow for producing signed transactions
through the use of PSQT, and the specific RPC commands used in typical
scenarios.

## PSQT in general

PSQT is an interchange format for Quicksilver transactions that are not fully signed
yet, together with relevant metadata to help entities work towards signing it.
It is intended to simplify workflows where multiple parties need to cooperate to
produce a transaction. Examples include hardware signers, multisig setups, and
[CoinJoin] transactions.

### Overall workflow

Overall, the construction of a fully signed Quicksilver transaction goes through the
following steps:

- A **Creator** proposes a particular transaction to be created. They construct
  a PSQT that contains certain inputs and outputs, but no additional metadata.
- For each input, an **Updater** adds information about the UTXOs being spent by
  the transaction to the PSQT. They also add information about the scripts and
  public keys involved in each of the inputs (and possibly outputs) of the PSQT.
- **Signers** inspect the transaction and its metadata to decide whether they
  agree with the transaction. They can use amount information from the UTXOs
  to assess the values involved. Quicksilver transactions are feeless. If they
  agree, they produce a
  partial signature for the inputs for which they have relevant key(s).
- A **Finalizer** is run for each input to convert the partial signatures and
  possibly script information into a final `scriptSig` and/or `scriptWitness`.
- An **Extractor** produces a valid Quicksilver transaction (in network format)
  from a PSQT for which all inputs are finalized.

Generally, each of the above (excluding Creator and Extractor) will simply
add more and more data to a particular PSQT, until all inputs are fully signed.
In a naive workflow, they all have to operate sequentially, passing the PSQT
from one to the next, until the Extractor can convert it to a real transaction.
In order to permit parallel operation, **Combiners** can be employed which merge
metadata from different PSQTs for the same unsigned transaction.

The names above in bold are the names of the roles defined in BIP-174. They're
useful in understanding the underlying steps, but in practice, software and
hardware implementations will typically implement multiple roles simultaneously.

## PSQT in Quicksilver

### RPCs

- **`converttopsqt` (Creator)** is a utility RPC that converts an
  unsigned raw transaction to PSQT format. It ignores existing signatures.
- **`createpsqt` (Creator)** is a utility RPC that takes a list of inputs and
  outputs and converts them to a PSQT with no additional information. It is
  equivalent to calling `createrawtransaction` followed by `converttopsqt`.
- **`vaultcreatefundedpsqt` (Creator, Updater)** is a vault RPC that creates a
  PSQT with the specified inputs and outputs, adds additional inputs and change
  to it to balance it out, and adds relevant metadata. In particular, for inputs
  that the vault knows about (counting towards its balance), UTXO
  information will be added. For outputs and inputs with UTXO
  information present, key and script information will be added which the vault
  knows about. It is equivalent to running `createrawtransaction`, followed by
  `fundrawtransaction`, and `converttopsqt`.
- **`vaultprocesspsqt` (Updater, Signer, Finalizer)** is a vault RPC that takes as
  input a PSQT, adds UTXO, key, and script data to inputs and outputs that miss
  it, and optionally signs inputs. Where possible it also finalizes the partial
  signatures.
- **`descriptorprocesspsqt` (Updater, Signer, Finalizer)** is a node RPC that takes
  as input a PSQT and a list of descriptors. It updates witness inputs with
  information available from the UTXO set and the relay pool and signs the inputs using
  the provided descriptors. Where possible it also finalizes the partial signatures.
- **`utxoupdatepsqt` (Updater)** is a node RPC that takes a PSQT and updates it
  to include information available from the UTXO set (works only for witness
  inputs).
- **`finalizepsqt` (Finalizer, Extractor)** is a utility RPC that finalizes any
  partial signatures, and if all inputs are finalized, converts the result to a
  fully signed transaction which can be broadcast with `sendrawtransaction`.
- **`combinepsqt` (Combiner)** is a utility RPC that implements a Combiner. It
  can be used at any point in the workflow to merge information added to
  different versions of the same PSQT. In particular it is useful to combine the
  output of multiple Updaters or Signers.
- **`joinpsqts`** (Creator) is a utility RPC that joins multiple PSQTs together,
  concatenating the inputs and outputs. This can be used to construct CoinJoin
  transactions.
- **`decodepsqt`** is a diagnostic utility RPC which will show all information in
  a PSQT in human-readable form.
- **`analyzepsqt`** is a utility RPC that examines a PSQT and reports the
  current status of its inputs, the next step in the workflow if known, and if
  possible, estimates the final virtual size.


### Workflows

#### Multisig with multiple Quicksilver instances

For a quick start see [Basic M-of-N multisig example using vaults and PSQTs](./descriptors.md#basic-multisig-example).

Alice, Bob, and Carol want to create a 2-of-3 multisig address. They're all using
Quicksilver. We assume their vaults only contain the multisig funds. In case
they also have a personal vault, this can be accomplished through the
multivault feature - possibly resulting in a need to add `-rpcvault=name` to
the command line in case `quicksilver-cli` is used.

Setup:
- All three call `getnewaddress` to create a new address; call these addresses
  *Aalice*, *Abob*, and *Acarol*.
- All three call `getaddressinfo "X"`, with *X* their respective address, and
  remember the corresponding public keys. Call these public keys *Kalice*,
  *Kbob*, and *Kcarol*.
- All three now run `addmultisigaddress 2 ["Kalice","Kbob","Kcarol"]` to teach
  their vault about the multisig script. Call the address produced by this
  command *Amulti*. They may be required to explicitly specify the same
  addresstype option each, to avoid constructing different versions due to
  differences in configuration.
- Others can verify the produced address by running
  `createmultisig 2 ["Kalice","Kbob","Kcarol"]`, and expecting *Amulti* as
  output. Again, it may be necessary to explicitly specify the addresstype
  in order to get a result that matches. This command won't enable them to
  initiate transactions later, however.
- They can now give out *Amulti* as address others can pay to.

Later, when *V* Hg has been received on *Amulti*, and Bob and Carol want to
move the coins in their entirety to address *Asend*, with no change. Alice
does not need to be involved.
- One of them - let's assume Carol here - initiates the creation. She runs
  `vaultcreatefundedpsqt [] {"Asend":V} 0`.
  We call the resulting PSQT *P*. *P* does not contain any signatures.
- Carol needs to sign the transaction herself. In order to do so, she runs
  `vaultprocesspsqt "P"`, and gives the resulting PSQT *P2* to Bob.
- Bob inspects the PSQT using `decodepsqt "P2"` to determine if the transaction
  has indeed just the expected input and an output to *Asend*. If he agrees, he calls `vaultprocesspsqt "P2"` to sign. The
  resulting PSQT *P3* contains both Carol's and Bob's signature.
- Now anyone can call `finalizepsqt "P3"` to extract a fully signed transaction
  *T*.
- Finally anyone can broadcast the transaction using `sendrawtransaction "T"`.

In case there are more signers, it may be advantageous to let them all sign in
parallel, rather than passing the PSQT from one signer to the next one. In the
above example this would translate to Carol handing a copy of *P* to each signer
separately. They can then all invoke `vaultprocesspsqt "P"`, and end up with
their individually-signed PSQT structures. They then all send those back to
Carol (or anyone) who can combine them using `combinepsqt`. The last two steps
(`finalizepsqt` and `sendrawtransaction`) remain unchanged.

Stock HWI tools use a different partially-signed transaction magic and derivation interface. Quicksilver's PSQT magic and qpub/squb derivation intentionally make that interoperability out of scope.
