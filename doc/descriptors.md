# Support for Output Descriptors in Quicksilver
This is a simple language which can be used to describe collections of output scripts.
Supporting RPCs are:
- `scantxoutset` takes as input descriptors to scan for, and also reports
  specialized descriptors for the matching UTXOs.
- `getdescriptorinfo` analyzes a descriptor, and reports a canonicalized version
  with checksum added.
- `deriveaddresses` takes as input a descriptor and computes the corresponding
  addresses.
- `listunspent` outputs a specialized descriptor for the reported unspent outputs.
- `getaddressinfo` outputs a descriptor for solvable addresses.
- `generatetodescriptor` takes as input a descriptor and generates coins to it
  (`sandbox` only).
- `utxoupdatepsqt` takes as input descriptors to add information to the psqt.
- `createmultisig` returns a descriptor as well.
- `importdescriptors` takes as input descriptors to import into a vault.
- `listdescriptors` outputs descriptors imported into a vault.
- `scanblocks` takes as input descriptors to scan for in blocks and returns the
   relevant blockhashes.
- `getdescriptoractivity` takes as input descriptors and blockhashes (as output
  by `scanblocks`) and returns rich event data related to spends or receives associated
  with the given descriptors.

This document describes the language. For the specifics on usage, see the RPC
documentation for the functions mentioned above.

## Features

Output descriptors currently support:
- Pay-to-pubkey scripts (P2PK), through the `pk` function.
- Pay-to-pubkey-hash scripts (P2PKH), through the `pkh` function.
- Pay-to-witness-pubkey-hash scripts (P2WPKH), through the `wpkh` function.
- Pay-to-script-hash scripts (P2SH), through the `sh` function.
- Pay-to-witness-script-hash scripts (P2WSH), through the `wsh` function.
- Pay-to-taproot outputs (P2TR), through the `tr` function.
- Multisig scripts, through the `multi` function.
- Multisig scripts where the public keys are sorted lexicographically, through the `sortedmulti` function.
- Multisig scripts inside taproot script trees, through the `multi_a` (and `sortedmulti_a`) function.
- Any type of supported address through the `addr` function.
- Raw hex scripts through the `raw` function.
- Public keys (compressed and uncompressed) in hex notation, or BIP32 extended pubkeys with derivation paths.
- [Miniscript] expressions in `wsh` (P2WSH) and `tr` (P2TR) functions.

## Examples

- `pk(0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798)` describes a P2PK output with the specified public key.
- `pkh(02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5)` describes a P2PKH output with the specified public key.
- `wpkh(02f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9)` describes a P2WPKH output with the specified public key.
- `combo(0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798)` describes any P2PK, P2PKH, or P2WPKH output with the specified public key.
- `multi(1,022f8bde4d1a07209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe4,025cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc)` describes a bare *1-of-2* multisig output with keys in the specified order.
- `sh(multi(2,022f01e5e15cca351daff3843fb70f3c2f0a1bdd05e5af888a67784ef3e10a2a01,03acd484e2f0c7f65309ad178a9f559abde09796974c57e714c35f110dfc27ccbe))` describes a P2SH *2-of-2* multisig output with keys in the specified order.
- `sh(sortedmulti(2,03acd484e2f0c7f65309ad178a9f559abde09796974c57e714c35f110dfc27ccbe,022f01e5e15cca351daff3843fb70f3c2f0a1bdd05e5af888a67784ef3e10a2a01))` describes a P2SH *2-of-2* multisig output with keys sorted lexicographically in the resulting redeemScript.
- `wsh(multi(2,03a0434d9e47f3c86235477c7b1ae6ae5d3442d49b1943c2b752a68e2a47e247c7,03774ae7f858a9411e5ef4246b70c65aac5649980be5c17891bbec17895da008cb,03d01115d548e7561b15c38f004d734633687cf4419620095bc5b0f47070afe85a))` describes a P2WSH *2-of-3* multisig output with keys in the specified order.
- `pk(qpub3pBWqhEkneoFJpBrbaNAqYXKdV8QkRFZzDn673tUnBVDUCFNpfYi36RiQaS8SuAPTKPFm6juX1ehhGYvcnJvWRg9bk2uNqXDv24VWAmPEBP)` describes a P2PK output with the public key of the specified qpub.
- `pkh(qpub3rSvqQYDsuvETRKZoLEoiadRxJgm8VjafqCz59GbkNgVqUYZEu5oAEn8BqzqCD4vTXfpdLHLFeinffX4Zp3JaK8soNqAJ9RSRxC3orGZq7t/1/2)` describes a P2PKH output with child key *1/2* of the specified qpub.
- `pkh([d34db33f/44'/9556'/0']qpub3xbKgzsXRc4LP8QQ46n6ALxasearqfi8fGcTryvGdMr82d96b9YkFKhdQz3khDMcuL8wpyQNJ84CyKzazox5gmxN8z7SFEwu2FsPqaZo3YZ/1/*)` describes a set of P2PKH outputs, but additionally specifies that the specified qpub is a child of a master with fingerprint `d34db33f`, and derived using Quicksilver path `44'/9556'/0'`.
- `wsh(multi(1,qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/1/0/*,qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/0/0/*))` describes a set of *1-of-2* P2WSH multisig outputs where the first multisig key is the *1/0/`i`* child of the first specified qpub and the second multisig key is the *0/0/`i`* child of the second specified qpub, and `i` is any number in a configurable range (`0-1000` by default).
- `wsh(sortedmulti(1,qpub3pBWqhEkneoFJRhBhjSTHkF5sgGqS3WPfoZpPzdJkZc5mBGZzpKeTzbJkgX7A9XMMsd3fmQ4v69xR4H9jyE5eLg6TgENDL1tEPjbn951hUS/1/0/*,qpub3sTG7QviGfdKphHUqWpLMYecEe2691zgDyKBZoekpyU3T9iYa5ficqAZL9UzxEXnaC7DXzPpKAzvaDhqd7vVywLNjiy37gAGMsrGDPYnxK9/0/0/*))` describes a set of *1-of-2* P2WSH multisig outputs where one multisig key is the *1/0/`i`* child of the first specified qpub and the other multisig key is the *0/0/`i`* child of the second specified qpub, and `i` is any number in a configurable range (`0-1000` by default). The order of public keys in the resulting witnessScripts is determined by the lexicographic order of the public keys at that index.
- `tr(c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5,{pk(fff97bd5755eeea420453a14355235d382f6472f8568a18b2f057a1460297556),pk(e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13)})` describes a P2TR output with the `c6...` x-only pubkey as internal key, and two script paths.
- `tr(c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5,sortedmulti_a(2,2f8bde4d1a07209355b4a7250a5c5128e88b84bddc619ab7cba8d569b240efe4,5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc))` describes a P2TR output with the `c6...` x-only pubkey as internal key, and a single `multi_a` script that needs 2 signatures with 2 specified x-only keys, which will be sorted lexicographically.
- `wsh(sortedmulti(2,[6f53d49c/44h/1h/0h]squb6boa1WNtWN3Sm9jFtjycGECdBhvHmEAWvyztav9CxvVnZTqMDucxbSkBJx11wX8q9md7EpdVdBoQBBv8cpkBWeyYmPMECtrX4yihDnodaTm/0/*,[e6807791/44h/1h/0h]squb6bENjtr4vz13SGzi2wsHhruJCDncDb1pbQcSmwq4Z1eVSSMU3C6zLAn37EYVFPcAxoqzM7au2BERTsWvXGohRF3hSmxNujZMSMgrbggQQQs/0/*,[367c9cfa/44h/1h/0h]squb6bx6bXqzuZB6QmUKyey4f3mpRBy73Y9T2eh5WQWLv7UVW6Hp8JA8oEqzgsJbYrVoN3JAfxRLdrYQ47rA48rsRRNK8bYZf8yEPwsY4Tg51Yj/0/*))#4hf6x5a8` describes a *2-of-3* multisig. For brevity, the internal "change" descriptor accompanying the above external "receiving" descriptor is not included here, but it typically differs only in the qpub derivation steps, ending in `/1/*` for change addresses.
- `wsh(thresh(4,pk([7258e4f9/44h/1h/0h]squb6adZZVxiprAXGZ9RWmJtzaciCLvEuzhxmbpmRMxNp1k1kPESXoBy1MTXNqdw8BHiPnJDN5i7ZGE3zvq6iZGFWdzKAzavZtBsePER6yqzhPF/0/*),s:pk([c80b1469/44h/1h/0h]squb6b7Bm2THjHHXS2Pp7Fst5rgFNA5gRkFPou1m91AcZHdQwcWQdu5BGwrt6F2kVaQSsutGDiXYptQvEkT5AJnqaDgVBhtenvFKyH2oF4gUsji/0/*),s:pk([4e5024fe/44h/1h/0h]squb6bQZdV9Fk9UfVx6TNGoBCv33ny1eotDquiRDSVjgi7gz39XH8sswh3jCTgMbH3rWgh1Y6Hc4wWD3yuFvY5ytkzQ74WzSUSVCHujYvUj9CYT/0/*),s:pk([3b1d1ee9/44h/1h/0h]squb6apvGFXfsD38Hc2GqsTUvJBVdd2uzQAFWVH4bPnxB1R1WABGcb2146DwurcPyrp3woxSbjHPoW8op4jt5GqPSD4qkxcgvkHXQGFdXthCNXx/0/*),sln:after(840000),sln:after(1050000),sln:after(1260000)))#eksjc05r` describes a Miniscript multisig with spending policy: `thresh(4,pk(key_1),pk(key_2),pk(key_3),pk(key_4),after(t1),after(t2),after(t3))` that starts as 4-of-4 and "decays" to 3-of-4, 2-of-4, and finally 1-of-4 at each future halvening block height. For brevity, the internal "change" descriptor accompanying the above external "receiving" descriptor is not included here, but it typically differs only in the qpub derivation steps, ending in `/1/*` for change addresses.

## Reference

Descriptors consist of several types of expressions. The top level expression is either a `SCRIPT`, or `SCRIPT#CHECKSUM` where `CHECKSUM` is an 8-character alphanumeric descriptor checksum.

`SCRIPT` expressions:
- `sh(SCRIPT)` (top level only): P2SH embed the argument.
- `wsh(SCRIPT)` (top level only): P2WSH embed the argument.
- `pk(KEY)` (anywhere): P2PK output for the given public key.
- `pkh(KEY)` (not inside `tr`): P2PKH output for the given public key (use `addr` if you only know the pubkey hash).
- `wpkh(KEY)` (top level only): P2WPKH output for the given compressed pubkey.
- `combo(KEY)` (top level only): an alias for the collection of `pk(KEY)` and `pkh(KEY)`. If the key is compressed, it also includes `wpkh(KEY)`.
- `multi(k,KEY_1,KEY_2,...,KEY_n)` (not inside `tr`): k-of-n multisig script using OP_CHECKMULTISIG.
- `sortedmulti(k,KEY_1,KEY_2,...,KEY_n)` (not inside `tr`): k-of-n multisig script with keys sorted lexicographically in the resulting script.
- `multi_a(k,KEY_1,KEY_2,...,KEY_N)` (only inside `tr`): k-of-n multisig script using OP_CHECKSIG, OP_CHECKSIGADD, and OP_NUMEQUAL.
- `sortedmulti_a(k,KEY_1,KEY_2,...,KEY_N)` (only inside `tr`): similar to `multi_a`, but the (x-only) public keys in it will be sorted lexicographically.
- `tr(KEY)` or `tr(KEY,TREE)` (top level only): P2TR output with the specified key as internal key, and optionally a tree of script paths.
- `addr(ADDR)` (top level only): the script which ADDR expands to.
- `raw(HEX)` (top level only): the script whose hex encoding is HEX.
- `rawtr(KEY)` (top level only): P2TR output with the specified key as output key. NOTE: while it's possible to use this to construct vaults, it has several downsides, like being unable to prove no hidden script path exists. Use at your own risk.

`KEY` expressions:
- Optionally, key origin information, consisting of:
  - An open bracket `[`
  - Exactly 8 hex characters for the fingerprint of the key where the derivation starts (see BIP32 for details)
  - Followed by zero or more `/NUM` or `/NUM'` path elements to indicate unhardened or hardened derivation steps between the fingerprint and the key or qpub/qprv root that follows
  - A closing bracket `]`
- Followed by the actual key, which is either:
  - Hex encoded public keys (either 66 characters starting with `02` or `03` for a compressed pubkey, or 130 characters starting with `04` for an uncompressed pubkey).
    - Inside `wpkh` and `wsh`, only compressed public keys are permitted.
    - Inside `tr` and `rawtr`, x-only pubkeys are also permitted (64 hex characters).
  - [WIF] encoded private keys may be specified instead of the corresponding public key, with the same meaning.
  - `qpub` encoded extended public key or `qprv` encoded extended private key.
    - Followed by zero or more `/NUM` unhardened and `/NUM'` hardened BIP32 derivation steps.
      - No more than one of these derivation steps may be of the form `<NUM;NUM;...;NUM>` (including hardened indicators with either or both `NUM`). If such specifiers are included, the descriptor will be parsed as multiple descriptors where the first descriptor uses all of the first `NUM` in the pair, and the second descriptor uses the second `NUM` in the pair for all `KEY` expressions, and so on.
    - Optionally followed by a single `/*` or `/*'` final step to denote all (direct) unhardened or hardened children.
    - The usage of hardened derivation steps requires providing the private key.

(Anywhere a `'` suffix is permitted to denote hardened derivation, the suffix `h` can be used instead.)

`TREE` expressions:
- any `SCRIPT` expression
- An open brace `{`, a `TREE` expression, a comma `,`, a `TREE` expression, and a closing brace `}`

`ADDR` expressions are any type of supported address:
- P2PKH addresses (base58, using the prefixes defined by the selected network). Note that P2PKH addresses in descriptors cannot be used for P2PK outputs (use the `pk` function instead).
- P2SH addresses (base58, using the prefixes defined by the selected network).
- Bech32 and bech32m addresses (of the form `hg1...` for mainnet, `phg1...` for publictest, or `shg1...` for sandbox).

## Explanation

### Single-key scripts

Quicksilver supports P2PK, P2PKH, P2WPKH, P2SH scripts, and P2WSH scripts.
Witness programs are always top-level outputs and cannot be embedded in P2SH.

To describe these, we model these as functions. The functions `pk`
(P2PK), `pkh` (P2PKH) and `wpkh` (P2WPKH) take as input a `KEY` expression, and return the
corresponding *scriptPubKey*. The functions `sh` (P2SH) and `wsh` (P2WSH)
take as input a `SCRIPT` expression, and return the script describing P2SH and P2WSH
outputs with the input as embedded script. The names of the functions do
not contain "p2" for brevity.

### Multisig

Several pieces of software use multi-signature (multisig) scripts based
on the OP_CHECKMULTISIG opcode. To support these, we introduce the
`multi(k,key_1,key_2,...,key_n)` and `sortedmulti(k,key_1,key_2,...,key_n)`
functions. They represent a *k-of-n*
multisig policy, where any *k* out of the *n* provided `KEY` expressions must
sign.

Key order is significant for `multi()`. A `multi()` expression describes a multisig script
with keys in the specified order, and in a search for TXOs, it will not match
outputs with multisig scriptPubKeys that have the same keys in a different
order. Also, to prevent a combinatorial explosion of the search space, if more
than one of the `multi()` key arguments is a BIP32 wildcard path ending in `/*`
or `*'`, the `multi()` expression only matches multisig scripts with the `i`th
child key from each wildcard path in lockstep, rather than scripts with any
combination of child keys from each wildcard path.

Key order does not matter for `sortedmulti()`. `sortedmulti()` behaves in the same way
as `multi()` does but the keys are reordered in the resulting script such that they
are lexicographically ordered.

#### Basic multisig example

For a good example of a basic M-of-N multisig between multiple participants using
vaults and PSQTs, as well as a signing flow, see [this functional test](../test/functional/vault_multisig_descriptor_psqt.py).

Disclaimers: It is important to note that this example serves as a quick-start and is kept basic for readability. A downside of the approach
outlined here is that each participant must maintain (and backup) two separate vaults: a signer and the corresponding multisig.
It should also be noted that privacy best-practices are not "by default" here - participants should take care to only use the signer to sign
transactions related to the multisig. Lastly, it is not recommended to use anything other than a Quicksilver vault to serve as your
signer(s). Other vaults, whether hardware or software, likely impose additional checks and safeguards to prevent users from signing transactions that
could lead to loss of funds, or are deemed security hazards. Conforming to various 3rd-party checks and verifications is not in the scope of this example.

The basic steps are:

  1. Every participant generates a qpub. The most straightforward way is to create a new vault which we will refer to as
     the participant's signer vault. Avoid reusing this vault for any purpose other than signing transactions from the
     corresponding multisig we are about to create. Hint: extract the vault's qpubs using `listdescriptors` and pick the one from the
     `pkh` descriptor since it is least likely to be accidentally reused
  2. Create a tracking-only vault (blank, private keys disabled). Now the multisig is created by importing the external and internal descriptors:
     `wsh(sortedmulti(<M>,QPUB1/0/*,QPUB2/0/*,…,QPUBN/0/*))` and `wsh(sortedmulti(<M>,QPUB1/1/*,QPUB2/1/*,…,QPUBN/1/*))`
     (one descriptor w/ `0` for receiving addresses and another w/ `1` for change). Every participant does this. All key origin information (master key fingerprint and all derivation steps) should be included with qpubs for proper support of hardware devices / external signers
  3. A receiving address is generated for the multisig. As a check to ensure step 2 was done correctly, every participant
     should verify they get the same addresses
  4. Funds are sent to the resulting address
  5. A sending transaction from the multisig is created using `vaultcreatefundedpsqt` (anyone can initiate this). It is simple to do
     this in the GUI by going to the Transfer tab in the multisig vault and creating an unsigned transaction (PSQT)
  6. At least `M` participants check the PSQT with their multisig using `decodepsqt` to verify the transaction is OK before signing it.
  7. (If OK) the participant signs the PSQT with their signer vault using `vaultprocesspsqt`. It is simple to do this in the GUI by
     loading the PSQT from file and signing it
  8. The signed PSQTs are collected with `combinepsqt`, finalized w/ `finalizepsqt`, and then the resulting transaction is broadcasted
     to the network. Note that any vault (eg one of the signers or multisig) is capable of doing this.
  9. Checks that balances are correct after the transaction has been included in a block

You may prefer a daisy chained signing flow where each participant signs the PSQT one after another until
the PSQT has been signed `M` times and is "complete." For the most part, the steps above remain the same, except (6, 7)
change slightly from signing the original PSQT in parallel to signing it in series. `combinepsqt` is not necessary with
this signing flow and the last (`m`th) signer can just broadcast the PSQT after signing. Note that a parallel signing flow may be
preferable in cases where there are more signers. This signing flow is also included in the test / Python example.
[The test](../test/functional/vault_multisig_descriptor_psqt.py) is meant to be documentation as much as it is a functional test, so
it is kept as simple and readable as possible.

#### Basic Miniscript-enabled "decaying" multisig example

For an example of a multisig that starts as 4-of-4 and "decays" to 3-of-4, 2-of-4, and finally 1-of-4 at each future halvening block height, see [this functional test](../test/functional/vault_miniscript_decaying_multisig_descriptor_psqt.py).

This has the same "architecture" and signing flow as the above [Basic multisig example](#basic-multisig-example). The basic steps are identical aside from the descriptor that defines this vault, which is of the form: `wsh(thresh(4,pk(QPUB1),s:pk(QPUB2),s:pk(QPUB3),s:pk(QPUB4),sln:after(t1),sln:after(t2),sln:after(t3)))`.

[The test](../test/functional/vault_miniscript_decaying_multisig_descriptor_psqt.py) is meant to be documentation as much as it is a functional test, so it is kept as simple and readable as possible.

### BIP32 derived keys and chains

Most modern vault software and hardware uses keys that are derived using
BIP32 ("HD keys"). We support these directly by permitting strings
consisting of an extended public key (commonly referred to as an *qpub*)
plus derivation path anywhere a public key is expected. The derivation
path consists of a sequence of 0 or more integers (in the range
*0..2<sup>31</sup>-1*) each optionally followed by `'` or `h`, and
separated by `/` characters. The string may optionally end with the
literal `/*` or `/*'` (or `/*h`) to refer to all unhardened or hardened
child keys in a configurable range (by default `0-1000`, inclusive).

Whenever a public key is described using a hardened derivation step, the
script cannot be computed without access to the corresponding private
key.

### Key origin identification

In order to describe scripts whose signing keys reside on another device,
it may be necessary to identify the master key and derivation path an
qpub was derived with.

For example, when following BIP44, it would be useful to describe a
change chain directly as `qpub.../44'/<coin>'/0'/1/*` where `qpub...`
corresponds with the master key `m`. Unfortunately, since there are
hardened derivation steps that follow the qpub, this descriptor does not
let you compute scripts without access to the corresponding private keys.
Instead, it should be written as `qpub.../1/*`, where qpub corresponds to
`m/44'/<coin>'/0'`. Quicksilver default descriptors use SLIP-44 coin type
 9556 on mainnet and 1 on test chains, rather than type 0.

When interacting with a hardware device, it may be necessary to include
the entire path from the master down. Standardized by
providing the master key *fingerprint* (first 32 bit of the Hash160 of
the master pubkey), plus all derivation steps. To support constructing
these, we permit providing this key origin information inside the
descriptor language, even though it does not affect the actual
scriptPubKeys it refers to.

Every public key can be prefixed by an 8-character hexadecimal
fingerprint plus optional derivation steps (hardened and unhardened)
surrounded by brackets, identifying the master and derivation path the key or qpub
that follows was derived with.

Note that the fingerprint of the parent only serves as a fast way to detect
parent and child nodes in software, and software must be willing to deal with
collisions.

### Including private keys

Often it is useful to communicate a description of scripts along with the
necessary private keys. For this reason, anywhere a public key or qpub is
supported, a private key in WIF format or qprv may be provided instead.
This is useful when private keys are necessary for hardened derivation
steps, for signing transactions, or for dumping vault descriptors
including private key material.

For example, after importing the following 2-of-3 multisig descriptor
into a vault, one could use `signrawtransactionwithvault`
to sign a transaction with the first key:
```
sh(multi(2,qprv.../84'/9556'/0'/0/0,qpub1...,qpub2...))
```
Note how the first key is a qprv private key with a specific derivation path,
while the other two are public keys.


### Specifying receiving and change descriptors in one descriptor

Since receiving and change addresses are frequently derived from the same
extended key(s) but with a single derivation index changed, it is convenient
to be able to specify a descriptor that can derive at the two different
indexes. Thus a single tuple of indexes is allowed in each derivation path
following the extended key. When this descriptor is parsed, multiple descriptors
will be produced, the first one will use the first index in the tuple for all
key expressions, the second will use the second index, the third will use the
third index, and so on..

For example, a descriptor of the form:

    multi(2,qpub.../<0;1;2>/0/*,qpub.../<2;3;4>/*)

will expand to the two descriptors

   multi(2,qpub.../0/0/*,qpub.../2/*)
   multi(2,qpub.../1/0/*,qpub.../3/*)
   multi(2,qpub.../2/0/*,qpub.../4*)

When this tuple contains only two elements, vault implementations can use the
first descriptor for receiving addresses and the second descriptor for change addresses.

### Representing existing script sets

In order to easily represent the sets of scripts commonly used by
existing Quicksilver vaults, a convenience function `combo` is
provided, which takes as input a public key, and describes a set of P2PK,
P2PKH, and P2WPKH scripts for that key. In case the key is
uncompressed, the set only includes P2PK and P2PKH scripts.

### Checksums

Descriptors can optionally be suffixed with a checksum to protect against
typos or copy-paste errors.

These checksums consist of 8 alphanumeric characters. As long as errors are
restricted to substituting characters in `0123456789()[],'/*abcdefgh@:$%{}`
for others in that set and changes in letter case, up to 4 errors will always
be detected in descriptors up to 501 characters, and up to 3 errors in longer
ones. For larger numbers of errors, or other types of errors, there is a
roughly 1 in a trillion chance of not detecting the errors.

All RPCs in Quicksilver will include the checksum in their output. Only
certain RPCs require checksums on input, including `deriveaddresses` and
`importdescriptors`. The checksum for a descriptor without one can be computed
using the `getdescriptorinfo` RPC.
