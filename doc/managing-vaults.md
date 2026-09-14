# Managing the Vault

## 1. Backing Up and Restoring The Vault

### 1.1 Creating the Vault

Quicksilver does not create a default vault automatically.
Vaults can be created with the `createvault` RPC or with the `Create Vault…` item in the GUI's `Vault` menu.

In the GUI, a `Create vault` button is displayed on the main screen when there is no vault loaded, beside `Open existing`. Alternatively, use `Vault` -> `Create Vault…`.

The following command, for example, creates a vault. More information about this command may be found by running `quicksilver-cli help createvault`.

```
$ quicksilver-cli createvault "vault-01"
```

By default, vaults are created in the `vaults` folder of the data directory, which varies by operating system, as shown below. The user can change the default by using the `-datadir` or `-vaultdir` initialization parameters.

| Operating System | Default vault directory                                    |
| -----------------|:------------------------------------------------------------|
| Linux            | `/home/<user>/.quicksilver/vaults`                         |
| Windows          | `C:\Users\<user>\AppData\Local\Quicksilver\vaults`         |
| macOS            | `/Users/<user>/Library/Application Support/Quicksilver/vaults` |

### 1.2 Encrypting the Vault

The `vault.dat` file is not encrypted by default and is, therefore, vulnerable if an attacker gains access to the device where the vault or the backups are stored.

Vault encryption may prevent unauthorized access. However, this significantly increases the risk of losing coins due to forgotten passphrases. There is no way to recover a passphrase. This tradeoff should be well thought out by the user.

Vault encryption may also not protect against more sophisticated attacks. An attacker can, for example, obtain the password by installing a keylogger on the user's machine.

After encrypting the vault or changing the passphrase, a new backup needs to be created immediately. The reason is that the keypool is flushed and a new HD seed is generated after encryption. Any coins received by the new seed cannot be recovered from the previous backups.

The vault's private key may be encrypted with `Controls` -> `Encrypt Vault…` in the GUI, or with the following command:

```
$ quicksilver-cli -rpcvault="vault-01" encryptvault "passphrase"
```

Once encrypted, the passphrase can be changed with the `vaultpassphrasechange` command, or in the GUI with `Controls` -> `Change Passphrase…`.

```
$ quicksilver-cli -rpcvault="vault-01" vaultpassphrasechange "oldpassphrase" "newpassphrase"
```

The argument passed to `-rpcvault` is the name of the vault to be encrypted.

Only the vault's private key is encrypted. All other vault information, such as transactions, is still visible.

The vault's private key can also be encrypted in the `createvault` command via the `passphrase` argument:

```
$ quicksilver-cli -named createvault vault_name="vault-01" passphrase="passphrase"
```

Note that if the passphrase is lost, all the coins in the vault will also be lost forever.

### 1.3 Unlocking the Vault

If the vault is encrypted and the user tries any operation related to private keys, such as sending coins, an error message will be displayed.

```
$ quicksilver-cli -rpcvault="vault-01" sendtoaddress "hg1qw508d6qejxtdg4y5r3zarvary0c5xw7kdl70ut" 0.01
error code: -13
error message:
Error: Please enter the vault passphrase with vaultpassphrase first.
```

To unlock the vault and allow it to run these operations, the `vaultpassphrase` RPC is required.

This command takes the passphrase and an argument called `timeout`, which specifies the time in seconds that the vault decryption key is stored in memory. After this period expires, the user needs to execute this RPC again.

```
$ quicksilver-cli -rpcvault="vault-01" vaultpassphrase "passphrase" 120
```

In the GUI, there is no specific menu item to unlock the vault. When the user sends coins, the passphrase will be prompted automatically.

### 1.4 Backing Up the Vault

To backup the vault, the `backupvault` RPC or the `Backup Vault…` GUI menu item must be used to ensure the file is in a safe state when the copy is made.

In the RPC, the destination parameter must include the name of the file. Otherwise, the command will return an error message like "Error: Vault backup failed!".

```
$ quicksilver-cli -rpcvault="vault-01" backupvault /home/node01/Backups/backup-01.dat
```

In the GUI, the vault is selected in the `Vault:` drop-down list on the application toolbar down the left-hand side. If this list is not present, the vault can be loaded with `Vault` -> `Open Vault` if necessary. Then, the backup can be done in `Vault` -> `Backup Vault…`.

This backup file can be stored on one or multiple offline devices, which must be reliable enough to work in an emergency and be malware free. Backup files can be regularly tested to avoid problems in the future.

If the computer has malware, it can compromise the vault when recovering the backup file. One way to minimize this is to not connect the backup to an online device.

If both the vault and all backups are lost for any reason, the coins related to this vault will become permanently inaccessible.

### 1.5 Backup Frequency

Quicksilver vaults use deterministic key derivation. A single backup of the vault seed is enough to recover the coins at any time, because every address is derived from that seed.

It is still recommended to make regular backups (once a week) or after a significant number of new transactions to maintain the metadata, such as labels. Metadata cannot be retrieved from a blockchain rescan, so if the backup is too old, the metadata will be lost forever.

### 1.6 Restoring the Vault From a Backup

To restore a vault, the `restorevault` RPC or the `Vault` -> `Restore Vault…` GUI menu item must be used.

```
$ quicksilver-cli restorevault "restored-vault" /home/node01/Backups/backup-01.dat
```

After that, `getvaultinfo` can be used to check if the vault has been fully restored.

```
$ quicksilver-cli -rpcvault="restored-vault" getvaultinfo
```

The restored vault can also be loaded in the GUI via `Vault` -> `Open Vault`.

## Vault Passphrase

Understanding vault security is crucial for safely storing your coins. A key aspect is the vault passphrase, used for encryption. Let's explore its nuances, role, encryption process, and limitations.

- **Not the Seed:**
The vault passphrase and the seed are two separate components in vault security. The seed, or HD seed, functions as a master key for deriving private and public keys in a hierarchical deterministic (HD) vault. In contrast, the passphrase serves as an additional layer of security specifically designed to secure the private keys within the vault. The passphrase serves as a safeguard, demanding an additional layer of authentication to access funds in the vault.

- **Protection Against Unauthorized Access:**
The passphrase serves as a protective measure, securing your funds in situations where an unauthorized user gains access to your unlocked computer or device while your vault application is active. Without the passphrase, they would be unable to access your vault's funds or execute transactions. However, it's essential to be aware that someone with access can potentially compromise the security of your passphrase by installing a keylogger.

- **Doesn't Encrypt Metadata or Public Keys:**
It's important to note that the passphrase primarily secures the private keys and access to funds within the vault. It does not encrypt metadata associated with transactions or public keys. Information about your transaction history and the public keys involved may still be visible.

- **Risk of Fund Loss if Forgotten or Lost:**
If the vault passphrase is too complex and is subsequently forgotten or lost, there is a risk of losing access to the funds permanently. A forgotten passphrase will result in the inability to unlock the vault and access the funds.
