# Support for signing transactions outside of Quicksilver

Quicksilver can be launched with `-signer=<cmd>` where `<cmd>` is a Quicksilver-aware external tool which can sign transactions and perform other functions. It can be used to communicate with a hardware signer through a Quicksilver-specific signer adapter.

## Example usage

The following example uses a local Quicksilver-aware signer adapter. Be particularly careful when running external signing tools on a computer with private keys on it.

When using a hardware signer, use a signer adapter that implements the Quicksilver PSQT format and qpub/squb derivation. Stock HWI tools do not meet this interface.

Start Quicksilver:

```sh
$ quicksilverd -signer=/path/to/quicksilver-signer
```

### Device setup

Follow the hardware manufacturer's instructions for the initial device setup and backup, using the procedures supported by its Quicksilver-aware signer adapter.

### Create vault and import keys

Get a list of signing devices / services:

```
$ quicksilver-cli enumeratesigners
{
  "signers": [
    {
      "fingerprint": "c8df832a"
    }
]
```

The master key fingerprint is used to identify a device.

Create a vault, this automatically imports the public keys:

```sh
$ quicksilver-cli createvault "hww" true true "" true true true
```

### Verify an address

Display an address on the device:

```sh
$ quicksilver-cli -rpcvault=<vault> getnewaddress
$ quicksilver-cli -rpcvault=<vault> vaultdisplayaddress <address>
```

Replace `<address>` with the result of `getnewaddress`.

### Spending

Under the hood this uses a [Partially Signed Quicksilver Transaction](psqt.md).

```sh
$ quicksilver-cli -rpcvault=<vault> sendtoaddress <address> <amount>
```

This prompts your hardware signer to sign, and fail if it's not connected. If successful
it automatically broadcasts the transaction.

```sh
{"complete": true, "txid": <txid>}
```

## Signer API

In order to be compatible with Quicksilver any signer command should conform to the specification below. This specification is subject to change. Ideally a BIP should propose a standard so that other vaults can also make use of it.

Prerequisite knowledge:
* [Output Descriptors](descriptors.md)
* Partially Signed Quicksilver Transaction ([PSQT](psqt.md))

### `enumerate` (required)

Usage:
```
$ <cmd> enumerate
[
    {
        "fingerprint": "00000000"
    }
]
```

The command MUST return an (empty) array with at least a `fingerprint` field.

A future extension could add an optional return field with device capabilities, perhaps using wildcard descriptors for the native output types a device supports. It could also restrict derivation paths to maintain compatibility with other vault software and indicate whether multisig is supported.

A future extension could add an optional return field `reachable`, in case `<cmd>` knows a signer exists but can't currently reach it.

### `signtx` (required)

Usage:
```
$ <cmd> --stdin --fingerprint <fingerprint> --chain <chain>
signtx <psqt>
```

The command reads the `signtx` request from stdin and returns a JSON object with
the signed PSQT.

The `psqt` SHOULD include BIP32 derivations. The command SHOULD fail if none of the BIP32 derivations match a key owned by the device.

The command SHOULD fail if the user cancels.

The command MAY complain if `--chain` does not match the BIP32 derivation paths.

### `getdescriptors` (optional)

Usage:

```
$ <cmd> --fingerprint <fingerprint> --chain <chain> getdescriptors --account <account>
<qpub>
```

Returns descriptors supported by the device. Example:

```
$ <cmd> --fingerprint 00000000 --chain main getdescriptors --account 0
{
  "receive": [
    "pkh([00000000/44h/9556h/0h]qpub6C.../0/*)#54vvmzuk",
    "wpkh([00000000/84h/9556h/0h]qpub6C.../0/*)#wm450r7y"
  ],
  "internal": [
    "pkh([00000000/44h/9556h/0h]qpub6C.../1/*)#9pfdxhvw",
    "wpkh([00000000/84h/9556h/0h]qpub6C..../1/*)#hdyq8wfd"
  ]
}
```

### `displayaddress` (optional)

Usage:
```
<cmd> --fingerprint <fingerprint> --chain <chain> displayaddress --desc descriptor
```

Example, display the first bech32 receive address on publictest:

```
<cmd> --fingerprint 00000000 --chain publictest displayaddress --desc "wpkh([00000000/84h/1h/0h]squbDDUZ..../0/0)"
```

The command MUST be able to figure out the address type from the descriptor.

The command MUST return an object containing `{"address": "[the address]"}`.
As a sanity check, for devices that support this, it SHOULD ask the device to derive the address.

If <descriptor> contains a master key fingerprint, the command MUST fail if it does not match the fingerprint known by the device.

If <descriptor> contains a qpub, the command MUST fail if it does not match the qpub known by the device.

The command MAY complain if `--chain` does not match the BIP32 coin type.

## How Quicksilver uses the Signer API

The `enumeratesigners` RPC simply calls `<cmd> enumerate`.

The `createvault` RPC calls:

* `<cmd> --fingerprint=00000000 getdescriptors 0`

It then imports descriptors for all supported address types, using BIP44/84/86 derivation paths.

The `vaultdisplayaddress` RPC reuses some code from `getaddressinfo` on the provided address and obtains the inferred descriptor. It then calls `<cmd> --fingerprint=00000000 displayaddress --desc=<descriptor>`.

`sendtoaddress` and `sendmany` check `inputs->bip32_derivs` to see if any inputs have the same `master_fingerprint` as the signer. If so, Quicksilver calls `<cmd> --stdin --fingerprint 00000000 --chain <chain>` and passes `signtx <psqt>` on stdin. It waits for the device to return a partially signed PSQT, tries to finalize it, and broadcasts the transaction.
