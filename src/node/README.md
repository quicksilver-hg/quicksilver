# src/node/

The [`src/node/`](./) directory contains code that needs to access node state
(state in `CChain`, `CBlockIndex`, `CCoinsView`, `CTxRelayPool`, and similar
classes).

Keep operations on shared node state here. Vault and GUI code use interfaces
in [`src/interfaces/`](../interfaces/) for node services and include node
headers where they need shared types.
