# Internal c++ interfaces

The following interfaces are defined here:

* [`Chain`](chain.h) — used by vault to access blockchain and relay pool state.

* [`ChainClient`](chain.h) — used by node to start & stop `Chain` clients.

* [`Node`](node.h) — used by GUI to start & stop the Quicksilver node.

* [`Vault`](vault.h) — used by GUI to access vaults.

* [`Handler`](handler.h) — returned by `handleEvent` methods on interfaces above and used to manage lifetimes of event handlers.

* [`Init`](init.h) — created when a process starts, and used to access the interfaces above.

The interfaces above define boundaries between major components of Quicksilver code (node, vault, and gui), so they can be tested, developed, and understood independently. These interfaces are not currently designed to be stable or to be used externally.
