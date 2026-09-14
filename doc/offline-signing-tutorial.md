# Offline Signing Tutorial

This tutorial will describe how to use two instances of Quicksilver, one online and one offline, to greatly increase security by not having private keys reside on a networked device.

Maintaining an air-gap between private keys and any network connections drastically reduces the opportunity for those keys to be exfiltrated from the user.

This workflow uses [Partially Signed Quicksilver Transactions](psqt.md) (PSQTs) to transfer the transaction to and from the offline vault for signing using the private keys.

> [!NOTE]
> This tutorial demonstrates the process using `sandbox` mode. Omit the `-sandbox` flag in the provided commands when working with `mainnet`.

## Overview
In this tutorial we have two hosts, both running the same current Quicksilver release:

* `offline` host which is disconnected from all networks (internet, Tor, wifi, bluetooth etc.) and does not have, or need, a copy of the blockchain.
* `online` host which is a regular online node with a synced blockchain.

We are going to first create an `offline_vault` on the offline host. We will then create a `tracking_vault` on the online host using public key descriptors exported from the `offline_vault`. Next we will receive some coins into the vault. In order to spend these coins we'll create an unsigned PSQT using the `tracking_vault`, sign the PSQT using the private keys in the `offline_vault`, and finally broadcast the signed PSQT using the online host.

### Requirements
- [jq](https://jqlang.github.io/jq/) installation - This tutorial uses jq to process certain fields from JSON RPC responses, but this convenience is optional.

### Create and Prepare the `offline_vault`

1. On the offline machine create a vault named `offline_vault` secured by a vault `passphrase`. This vault will contain private keys and must remain unconnected to any networks at all times.

```sh
[offline]$ ./build/bin/quicksilver-cli -sandbox -named createvault \
                vault_name="offline_vault" \
                passphrase="** enter passphrase **"

{
  "name": "offline_vault"
}
```

> [!NOTE]
> The use of a passphrase is crucial to encrypt the vault.dat file. This encryption ensures that even if an unauthorized individual gains access to the offline host, they won't be able to access the vault's contents. Further details about securing your vault can be found in [Managing the Vault](managing-vaults.md#12-encrypting-the-vault)

2. Export the public key-only descriptors from the offline host to a JSON file named `descriptors.json`. We use `jq` here to extract the `.descriptors` field from the full RPC response.

```sh
[offline]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="offline_vault" listdescriptors \
             | jq -r '.descriptors' \
             >> /path/to/descriptors.json
```

> [!NOTE]
> The `descriptors.json` file will be transferred to the online machine (e.g. using a USB flash drive) where it can be imported to create a related tracking-only vault.

### Create the online `tracking_vault`

1. On the online machine create a blank tracking-only vault which has private keys disabled and is named `tracking_vault`. This is achieved by using the `createvault` options: `disable_private_keys=true, blank=true`.

The `tracking_vault` vault will be used to track and validate incoming transactions, create unsigned PSQTs when spending coins, and broadcast signed and finalized PSQTs.

> [!NOTE]
> `disable_private_keys` indicates that the vault should refuse to import private keys, i.e. will be a dedicated tracking-only vault.

```sh
[online]$ ./build/bin/quicksilver-cli -sandbox -named createvault \
              vault_name="tracking_vault" \
              disable_private_keys=true \
              blank=true

{
  "name": "tracking_vault"
}
```

2. Import the `offline_vault`s public key descriptors to the online `tracking_vault` using the `descriptors.json` file created on the offline vault.

```sh
[online]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="tracking_vault" importdescriptors "$(cat /path/to/descriptors.json)"

[
  {
    "success": true
  },
  {
    "success": true
  },
  {
    "success": true
  },
  {
    "success": true
  },
  {
    "success": true
  },
  {
    "success": true
  },
  {
    "success": true
  },
  {
    "success": true
  }
]
```
> [!NOTE]
> Multiple success values indicate that multiple descriptors, for different address types, have been successfully imported. This allows generating different address types on the `tracking_vault`.

### Fund the `offline_vault`

At this point, it's important to understand that both the `offline_vault` and online `tracking_vault` share the same public keys. As a result, they generate the same addresses. Transactions can be created using either vault, but valid signatures can only be added by the `offline_vault` as only it has the private keys.

1. Generate an address to receive coins. You can use _either_ the `offline_vault` or the online `tracking_vault` to generate this address, as they will produce the same addresses. For the sake of this guide, we'll use the online `tracking_vault` to generate the address.

```sh
[online]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="tracking_vault" getnewaddress

shg1qtu5qgc6ddhmqm5yqjvhg83qgk2t4ewajvq6dee
```

2. Fund the address from your local sandbox vault or mining setup, then generate enough blocks for the output to mature if you are spending coinbase funds.

3. Confirm that coins were received using the online `tracking_vault`. Note that the transaction may take a few moments before being received on your local node, depending on its connectivity. Just re-run the command periodically until the transaction is received.

```sh
[online]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="tracking_vault" listunspent

[
  {
    "txid": "0f3953dfc3eb8e753cd1633151837c5b9953992914ff32b7de08c47f1f29c762",
    "vout": 1,
    "address": "shg1qtu5qgc6ddhmqm5yqjvhg83qgk2t4ewajvq6dee",
    "label": "",
    "output_script": "00145f2804634d6df60dd080932e83c408b2975cbbb2",
    "amount": 0.01000000,
    "confirmations": 4,
    "spendable": true,
    "solvable": true,
    "desc": "wpkh([306c734f/84h/1h/0h/0/0]025932ccee7590158f7e08bb36290d135d30a0b045163da896e1cd7645ec4223a9)#xytvyr4a",
    "parent_descs": [
      "wpkh([306c734f/84h/1h/0h]squb6aNVMECCwscNEpXCyiLvq8LFfYxBbCNrNMM3Wm9UpEDB5idVTnojZ3QWrK7mchkquZNPuKJsJ9e3hFYnhBq9iUHsMzf43ai5dueYU6TadC7/0/*)#smftyqe2"
    ],
    "safe": true
  }
]
```

### Create and Export an Unsigned PSQT

1. Get a destination address for the transaction. In this tutorial we'll be sending funds to the address `shg1q9k5w0nhnhyeh78snpxh0t5t7c3lxdeg3av54vy`, but if you don't need the coins for further testing you could send the coins back to the faucet.

2. Create a funded but unsigned PSQT to the destination address with the online `tracking_vault` by using `send [{"address":amount},...]` and export the unsigned PSQT to a file `funded_psqt.txt` for easy portability to the `offline_vault` for signing:

```sh
[online]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="tracking_vault" send \
              '{"shg1q9k5w0nhnhyeh78snpxh0t5t7c3lxdeg3av54vy": 0.009}' \
              | jq -r '.psqt' \
              >> /path/to/funded_psqt.txt

[online]$ cat /path/to/funded_psqt.txt

cHNxdP8BAHECAAAAAWLHKR9/xAjetzL/FCmZU5lbfINRMWPRPHWO68PfUzkPAQAAAAD9////AoA4AQAAAAAAFgAULajnzvO5M38eEwmu9dF+xH5m5RGs0g0AAAAAABYAFMaT0f/Wp2DCZzL6dkJ3GhWj4Y9vAAAAAAABAHECAAAAAY+dRPEBrGopkw4ugSzS9npzJDEIrE/bq1XXI0KbYnYrAQAAAAD+////ArKaXgAAAAAAFgAUwEc4LdoxSjbWo/2Ue+HS+QjwfiBAQg8AAAAAABYAFF8oBGNNbfYN0ICTLoPECLKXXLuyYW8CAAEBH0BCDwAAAAAAFgAUXygEY01t9g3QgJMug8QIspdcu7IiBgJZMszudZAVj34IuzYpDRNdMKCwRRY9qJbhzXZF7EIjqRgwbHNPVAAAgAEAAIAAAACAAAAAAAAAAAAAACICA7BlBnyAR4F2UkKuSX9MFhYCsn6j//z9i7lHDm1O0CU0GDBsc09UAACAAQAAgAAAAIABAAAAAAAAAAA=
```
> [!NOTE]
> Leaving the `input` array empty in the above `vaultcreatefundedpsqt` command is permitted and will cause the vault to automatically select appropriate inputs for the transaction.

### Decode and Analyze the Unsigned PSQT

Decode and analyze the unsigned PSQT on the `offline_vault` using the `funded_psqt.txt` file:

```sh
[offline]$ ./build/bin/quicksilver-cli -sandbox decodepsqt $(cat /path/to/funded_psqt.txt)

{
    ...
}

[offline]$ ./build/bin/quicksilver-cli -sandbox analyzepsqt $(cat /path/to/funded_psqt.txt)

{
  "inputs": [
    {
      "has_utxo": true,
      "is_final": false,
      "next": "signer",
      "missing": {
        "signatures": [
          "5f2804634d6df60dd080932e83c408b2975cbbb2"
        ]
      }
    }
  ],
  "estimated_vsize": 141,
  "next": "signer"
}
```

Notice that the analysis of the PSQT shows that "signatures" are missing and should be provided by the private key corresponding to the public key hash (hash160) "5f2804634d6df60dd080932e83c408b2975cbbb2". Quicksilver public RPC output does not include a fee field; transactions must be exact-value and feeless.

### Process and Sign the PSQT

1. Unlock the `offline_vault` with the Passphrase:

Use the vaultpassphrase command to unlock the `offline_vault` with the passphrase. You should specify the passphrase and a timeout (in seconds) for how long you want the vault to remain unlocked.

```sh
[offline]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="offline_vault" vaultpassphrase "** enter passphrase **" 60
```

2. Process, sign and finalize the PSQT on the `offline_vault` using the `vaultprocesspsqt` command, saving the output to a file `final_psqt.txt`.

 ```sh
[offline]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="offline_vault" vaultprocesspsqt \
                $(cat /path/to/funded_psqt.txt) \
                | jq -r .hex \
                >> /path/to/final_psqt.txt
 ```

### Broadcast the Signed and Finalized PSQT
Broadcast the funded, signed and finalized PSQT `final_psqt.txt` using `sendrawtransaction` with an online node:

```sh
[online]$ ./build/bin/quicksilver-cli -sandbox sendrawtransaction $(cat /path/to/final_psqt.txt)

c2430a0e46df472b04b0ca887bbcd5c4abf7b2ce2eb71de981444a80e2b96d52
```

### Confirm Vault Balance

Confirm the updated balance of the offline vault using the `tracking_vault`.

```sh
[online]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="tracking_vault" getbalances

{
  "mine": {
    "trusted": 0.00085900,
    "untrusted_pending": 0.00000000,
    "immature": 0.00000000
  },
  "lastprocessedblock": {
    "hash": "0000003065c0669fff27edb4a71928cb48e5a6cfcdf06f491a83fd86822d18a6",
    "height": 159592
  }
}
```


You can also show transactions related to the vault using `listtransactions`

```sh
[online]$ ./build/bin/quicksilver-cli -sandbox -rpcvault="tracking_vault" listtransactions

{
    ...
}
```
