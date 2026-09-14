# 1. Multisig Tutorial

Currently, it is possible to create a multisig vault using Quicksilver only.

Although there is already a brief explanation about the multisig in the [Descriptors documentation](descriptors.md#multisig), this tutorial proposes to use sandbox, keeping the workflow local and reproducible and explaining some functions in more detail.

This tutorial uses [jq](https://github.com/stedolan/jq) JSON processor to process the results from RPC and stores the relevant values in bash variables. This makes the tutorial reproducible and easier to follow step by step.

Before starting this tutorial, start the Quicksilver node in sandbox mode.

```bash
./build/bin/quicksilverd -sandbox -daemon
```

This tutorial also uses the default WPKH derivation path to get the qpubs.

At the time of writing, there is no way to extract a specific path from vaults in Quicksilver. For this, an external signer/qpub can be used.

## 1.1 Basic Multisig Workflow

### 1.1 Create the Vaults

For a 2-of-3 multisig, create 3 vaults. These vaults contain HD seed and private keys, which will be used to sign the PSQTs and derive the qpub.

These three vaults should not be used directly for privacy reasons (public key reuse). They should only be used to sign transactions for the (tracking-only) multisig vault.

```bash
for ((n=1;n<=3;n++))
do
 ./build/bin/quicksilver-cli -sandbox createvault "participant_${n}"
done
```

Extract the qpub of each vault. To do this, the `listdescriptors` RPC is used. By default, Quicksilver single-sig vaults are created using path `m/44'/1'/0'` for PKH, `m/84'/1'/0'` for WPKH and `m/86'/1'/0'` for P2TR based accounts. Each of them uses the chain 0 for external addresses and chain 1 for internal ones, as shown in the example below.

```
wpkh([1004658e/84'/1'/0']squb6aEwRreoxUEcWeY5fqVztRkrznBuetusAAf5vYsA9wouKRtqBLhr2ZxYpAVWcnYKeZzMi5LEoU2hnBM6UVYfbdXbNHMYEC2ebjhS6bVyiw1/0/*)#ceax5x5k

wpkh([1004658e/84'/1'/0']squb6aEwRreoxUEcWeY5fqVztRkrznBuetusAAf5vYsA9wouKRtqBLhr2ZxYpAVWcnYKeZzMi5LEoU2hnBM6UVYfbdXbNHMYEC2ebjhS6bVyiw1/1/*)#fdc8fnyw
```

The suffix (after #) is the checksum. Descriptors can optionally be suffixed with a checksum to protect against typos or copy-paste errors.
All RPCs in Quicksilver will include the checksum in their output.

```bash
declare -A qpubs

for ((n=1;n<=3;n++))
do
 qpubs["internal_qpub_${n}"]=$(./build/bin/quicksilver-cli -sandbox -rpcvault="participant_${n}" listdescriptors | jq '.descriptors | [.[] | select(.desc | startswith("wpkh") and contains("/1/*"))][0] | .desc' | grep -Po '(?<=\().*(?=\))')

 qpubs["external_qpub_${n}"]=$(./build/bin/quicksilver-cli -sandbox -rpcvault="participant_${n}" listdescriptors | jq '.descriptors | [.[] | select(.desc | startswith("wpkh") and contains("/0/*") )][0] | .desc' | grep -Po '(?<=\().*(?=\))')
done
```

`jq` is used to extract the qpub from the `wpkh` descriptor.

The following command can be used to verify if the qpub was generated correctly.

```bash
for x in "${!qpubs[@]}"; do printf "[%s]=%s\n" "$x" "${qpubs[$x]}" ; done
```

As previously mentioned, this step extracts the `m/84'/1'/0'` account instead, since there is no way to extract a specific path in Quicksilver at the time of writing.

### 1.2 Define the Multisig Descriptors

Define the external and internal multisig descriptors, add the checksum and then, join both in a JSON array.

```bash
external_desc="wsh(sortedmulti(2,${qpubs["external_qpub_1"]},${qpubs["external_qpub_2"]},${qpubs["external_qpub_3"]}))"
internal_desc="wsh(sortedmulti(2,${qpubs["internal_qpub_1"]},${qpubs["internal_qpub_2"]},${qpubs["internal_qpub_3"]}))"

external_desc_sum=$(./build/bin/quicksilver-cli -sandbox getdescriptorinfo $external_desc | jq '.descriptor')
internal_desc_sum=$(./build/bin/quicksilver-cli -sandbox getdescriptorinfo $internal_desc | jq '.descriptor')

multisig_ext_desc="{\"desc\": $external_desc_sum, \"active\": true, \"internal\": false, \"timestamp\": \"now\"}"
multisig_int_desc="{\"desc\": $internal_desc_sum, \"active\": true, \"internal\": true, \"timestamp\": \"now\"}"

multisig_desc="[$multisig_ext_desc, $multisig_int_desc]"
```

`external_desc` and `internal_desc` specify the output type (`wsh`, in this case) and the qpubs involved. They also use BIP 67 (`sortedmulti`), so the vault can be recreated without worrying about the order of qpubs. Conceptually, descriptors describe a list of scriptPubKey (along with information for spending from it)

Note that at least two descriptors are usually used, one for internal derivation paths and one for external ones.

After creating the descriptors, it is necessary to add the checksum, which is required by the `importdescriptors` RPC.

The checksum for a descriptor without one can be computed using the `getdescriptorinfo` RPC. The response has the `descriptor` field, which is the descriptor with the checksum added.

There are other fields that can be added to the descriptors:

* `active`: Sets the descriptor to be the active one for the corresponding output type (`wsh`, in this case).
* `internal`: Indicates whether matching outputs should be treated as something other than incoming payments (e.g. change).
* `timestamp`: Sets the time from which to start rescanning the blockchain for the descriptor, in UNIX epoch time.

Documentation for these and other parameters can be found by typing `./build/bin/quicksilver-cli help importdescriptors`.

`multisig_desc` concatenates external and internal descriptors in a JSON array and then it will be used to create the multisig vault.

### 1.3 Create the Multisig Vault

To create the multisig vault, first create an empty one (no keys, HD seed and private keys disabled).

Then import the descriptors created in the previous step using the `importdescriptors` RPC.

After that, `getvaultinfo` can be used to check if the vault was created successfully.

```bash
./build/bin/quicksilver-cli -sandbox -named createvault vault_name="multisig_vault_01" disable_private_keys=true blank=true

./build/bin/quicksilver-cli  -sandbox -rpcvault="multisig_vault_01" importdescriptors "$multisig_desc"

./build/bin/quicksilver-cli  -sandbox -rpcvault="multisig_vault_01" getvaultinfo
```

Once the vaults have already been created and this tutorial needs to be repeated or resumed, it is not necessary to recreate them, just load them with the command below:

```bash
for ((n=1;n<=3;n++)); do ./build/bin/quicksilver-cli -sandbox loadvault "participant_${n}"; done
```

### 1.4 Fund the vault

The vault can receive test coins by generating a new address and funding it from the local sandbox node.

```bash
receiving_address=$(./build/bin/quicksilver-cli -sandbox -rpcvault="multisig_vault_01" getnewaddress)

./build/bin/quicksilver-cli -sandbox generatetoaddress 101 "$receiving_address"
```

To copy the receiving address onto the clipboard, use the following command.

```bash
echo -n "$receiving_address" | xclip -sel clip
```

The `getbalances` RPC may be used to check the balance. Coins with `trusted` status can be spent.

```bash
./build/bin/quicksilver-cli -sandbox -rpcvault="multisig_vault_01" getbalances
```

### 1.5 Create a PSQT

Unlike singlesig vaults, multisig vaults cannot create and sign transactions directly because they require the signatures of the co-signers. Instead they create a Partially Signed Quicksilver Transaction (PSQT).

PSQT is a data format that allows Quicksilver vaults and tools to exchange information about a Quicksilver transaction and the signatures necessary to complete it.

For simplicity, the destination address is taken from the `participant_1` vault in the code above, but it can be any valid Quicksilver address.

The `vaultcreatefundedpsqt` RPC is used to create and fund a transaction in the PSQT format. It is the first step in creating the PSQT.

```bash
balance=$(./build/bin/quicksilver-cli -sandbox -rpcvault="multisig_vault_01" getbalance)

amount=$(echo "$balance * 0.8" | bc -l | sed -e 's/^\./0./' -e 's/^-\./-0./')

destination_addr=$(./build/bin/quicksilver-cli -sandbox -rpcvault="participant_1" getnewaddress)

funded_psqt=$(./build/bin/quicksilver-cli -sandbox -named -rpcvault="multisig_vault_01" vaultcreatefundedpsqt outputs="{\"$destination_addr\": $amount}" | jq -r '.psqt')
```

There is also the `createpsqt` RPC, which serves the same purpose, but it has no access to the vault or to the UTXO set. It is functionally the same as `createrawtransaction` and just drops the raw transaction into an otherwise blank PSQT. In most cases, `vaultcreatefundedpsqt` solves the problem.

The `send` RPC can also return a PSQT if more signatures are needed to sign the transaction.

### 1.6 Decode or Analyze the PSQT

Optionally, the PSQT can be decoded to a JSON format using `decodepsqt` RPC.

The `analyzepsqt` RPC analyzes and provides information about the current status of a PSQT and its inputs, e.g. missing signatures.

```bash
./build/bin/quicksilver-cli -sandbox decodepsqt $funded_psqt

./build/bin/quicksilver-cli -sandbox analyzepsqt $funded_psqt
```

### 1.7 Update the PSQT

In the code above, two PSQTs are created. One signed by `participant_1` vault and other, by the `participant_2` vault.

The `vaultprocesspsqt` is used by the vault to sign a PSQT.

```bash
psqt_1=$(./build/bin/quicksilver-cli -sandbox -rpcvault="participant_1" vaultprocesspsqt $funded_psqt | jq '.psqt')

psqt_2=$(./build/bin/quicksilver-cli -sandbox -rpcvault="participant_2" vaultprocesspsqt $funded_psqt | jq '.psqt')
```

### 1.8 Combine the PSQT

The PSQT, if signed separately by the co-signers, must be combined into one transaction before being finalized. This is done by `combinepsqt` RPC.

```bash
combined_psqt=$(./build/bin/quicksilver-cli -sandbox combinepsqt "[$psqt_1, $psqt_2]")
```

There is an RPC called `joinpsqts`, but it has a different purpose than `combinepsqt`. `joinpsqts` joins the inputs from multiple distinct PSQTs into one PSQT.

In the example above, the PSQTs are the same, but signed by different participants. If the user tries to merge them using `joinpsqts`, the error `Input txid:pos exists in multiple PSQTs` is returned. To be able to merge different PSQTs into one, they must have different inputs and outputs.

### 1.9 Finalize and Broadcast the PSQT

The `finalizepsqt` RPC is used to produce a network serialized transaction which can be broadcast with `sendrawtransaction`.

It checks that all inputs have complete scriptSigs and scriptWitnesses and, if so, encodes them into network serialized transactions.

```bash
finalized_psqt_hex=$(./build/bin/quicksilver-cli -sandbox finalizepsqt $combined_psqt | jq -r '.hex')

./build/bin/quicksilver-cli -sandbox sendrawtransaction $finalized_psqt_hex
```

### 1.10 Alternative Workflow (PSQT sequential signatures)

Instead of each vault signing the original PSQT and combining them later, the vaults can also sign the PSQTs sequentially. This is less scalable than the previously presented parallel workflow, but it works.

After that, the rest of the process is the same: the PSQT is finalized and transmitted to the network.

```bash
psqt_1=$(./build/bin/quicksilver-cli -sandbox -rpcvault="participant_1" vaultprocesspsqt $funded_psqt | jq -r '.psqt')

psqt_2=$(./build/bin/quicksilver-cli -sandbox -rpcvault="participant_2" vaultprocesspsqt $psqt_1 | jq -r '.psqt')

finalized_psqt_hex=$(./build/bin/quicksilver-cli -sandbox finalizepsqt $psqt_2 | jq -r '.hex')

./build/bin/quicksilver-cli -sandbox sendrawtransaction $finalized_psqt_hex
```
