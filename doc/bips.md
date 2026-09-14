# Quicksilver BIP Compatibility

Quicksilver inherits several wire, vault, script, descriptor, and transport
standards from the BIP process. This file is a compatibility matrix, not a
claim that Quicksilver shares the upstream network's activation history, block
heights, release versions, policy, or address prefixes.

## Active or Supported Standards

| BIP | Quicksilver use |
| --- | --- |
| [BIP 9](https://github.com/bitcoin/bips/blob/master/bip-0009.mediawiki) | Versionbits deployment state machine remains available for Quicksilver consensus deployments. |
| [BIP 11](https://github.com/bitcoin/bips/blob/master/bip-0011.mediawiki), [BIP 13](https://github.com/bitcoin/bips/blob/master/bip-0013.mediawiki) | Multisig and P2SH script forms remain supported with Quicksilver address parameters. |
| [BIP 14](https://github.com/bitcoin/bips/blob/master/bip-0014.mediawiki) | Peer user-agent strings use the inherited subversion-message structure with Quicksilver identity. |
| [BIP 16](https://github.com/bitcoin/bips/blob/master/bip-0016.mediawiki), [BIP 30](https://github.com/bitcoin/bips/blob/master/bip-0030.mediawiki), [BIP 34](https://github.com/bitcoin/bips/blob/master/bip-0034.mediawiki), [BIP 65](https://github.com/bitcoin/bips/blob/master/bip-0065.mediawiki), [BIP 66](https://github.com/bitcoin/bips/blob/master/bip-0066.mediawiki), [BIP 68](https://github.com/bitcoin/bips/blob/master/bip-0068.mediawiki), [BIP 112](https://github.com/bitcoin/bips/blob/master/bip-0112.mediawiki), [BIP 113](https://github.com/bitcoin/bips/blob/master/bip-0113.mediawiki), [BIP 141](https://github.com/bitcoin/bips/blob/master/bip-0141.mediawiki), [BIP 143](https://github.com/bitcoin/bips/blob/master/bip-0143.mediawiki), [BIP 147](https://github.com/bitcoin/bips/blob/master/bip-0147.mediawiki) | Script and transaction-validation rules retained where compatible with Quicksilver consensus. Quicksilver activation heights are defined in `src/kernel/chainparams.cpp`, not by historical upstream block heights. |
| [BIP 21](https://github.com/bitcoin/bips/blob/master/bip-0021.mediawiki) | URI handling is retained with the Quicksilver URI scheme and address encodings. |
| [BIP 22](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki), [BIP 23](https://github.com/bitcoin/bips/blob/master/bip-0023.mediawiki), [BIP 145](https://github.com/bitcoin/bips/blob/master/bip-0145.mediawiki) | Getblocktemplate mining interfaces are retained and adapted for Quicksilver's feeless, proof-of-work based mining flow. |
| [BIP 31](https://github.com/bitcoin/bips/blob/master/bip-0031.mediawiki), [BIP 130](https://github.com/bitcoin/bips/blob/master/bip-0130.mediawiki), [BIP 152](https://github.com/bitcoin/bips/blob/master/bip-0152.mediawiki), [BIP 155](https://github.com/bitcoin/bips/blob/master/bip-0155.mediawiki), [BIP 159](https://github.com/bitcoin/bips/blob/master/bip-0159.mediawiki), [BIP 324](https://github.com/bitcoin/bips/blob/master/bip-0324.mediawiki), [BIP 339](https://github.com/bitcoin/bips/blob/master/bip-0339.mediawiki) | Peer protocol extensions retained where they are independent of Quicksilver chain identity and feeless policy. |
| [BIP 32](https://github.com/bitcoin/bips/blob/master/bip-0032.mediawiki), [BIP 43](https://github.com/bitcoin/bips/blob/master/bip-0043.mediawiki), [BIP 44](https://github.com/bitcoin/bips/blob/master/bip-0044.mediawiki), [BIP 84](https://github.com/bitcoin/bips/blob/master/bip-0084.mediawiki), [BIP 86](https://github.com/bitcoin/bips/blob/master/bip-0086.mediawiki) | Hierarchical deterministic vault and descriptor derivation structures are retained with Quicksilver extended-key and address parameters. |
| [BIP 173](https://github.com/bitcoin/bips/blob/master/bip-0173.mediawiki), [BIP 350](https://github.com/bitcoin/bips/blob/master/bip-0350.mediawiki) | Bech32 and Bech32m encoding rules are retained with Quicksilver human-readable parts. |
| [BIP 174](https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki), [BIP 371](https://github.com/bitcoin/bips/blob/master/bip-0371.mediawiki) | The PSBT structure is retained as Quicksilver PSQT with Quicksilver transaction and address semantics. |
| [BIP 340](https://github.com/bitcoin/bips/blob/master/bip-0340.mediawiki), [BIP 341](https://github.com/bitcoin/bips/blob/master/bip-0341.mediawiki), [BIP 342](https://github.com/bitcoin/bips/blob/master/bip-0342.mediawiki) | Schnorr, Taproot, and Tapscript validation rules are retained where enabled by Quicksilver chain parameters. |
| [BIP 379](https://github.com/bitcoin/bips/blob/master/bip-0379.md), [BIP 380](https://github.com/bitcoin/bips/blob/master/bip-0380.mediawiki), [BIP 381](https://github.com/bitcoin/bips/blob/master/bip-0381.mediawiki), [BIP 382](https://github.com/bitcoin/bips/blob/master/bip-0382.mediawiki), [BIP 383](https://github.com/bitcoin/bips/blob/master/bip-0383.mediawiki), [BIP 384](https://github.com/bitcoin/bips/blob/master/bip-0384.mediawiki), [BIP 385](https://github.com/bitcoin/bips/blob/master/bip-0385.mediawiki), [BIP 386](https://github.com/bitcoin/bips/blob/master/bip-0386.mediawiki), [BIP 387](https://github.com/bitcoin/bips/blob/master/bip-0387.mediawiki) | Output script descriptors and Miniscript support are retained for vault, RPC, and signing workflows. |

## Not Quicksilver Claims

Historical activation dates, upstream block heights, upstream release numbers,
historical address examples, filter-message behavior, and upstream test-network
launch details are not Quicksilver compatibility claims. Quicksilver chain
identity, activation points, address prefixes, network ports, and feeless policy
are defined in this repository.

[BIP 133](https://github.com/bitcoin/bips/blob/master/bip-0133.mediawiki) fee
filtering is not implemented. The command is not registered. BIP324 short-id 5
is an unused slot so later assigned ids stay put.
