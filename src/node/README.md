# src/node/

The [`src/node/`](./) directory contains code that needs to access node state
(state in `CChain`, `CBlockIndex`, `CCoinsView`, `CTxRelayPool`, and similar
classes).

Code in [`src/node/`](./) is meant to be segregated from code in
[`src/vault/`](../vault/) and [`src/qt/`](../qt/), to ensure vault and GUI
code changes don't interfere with node operation, to allow vault and GUI code
to run in separate processes, and to perhaps eventually allow vault and GUI
code to be maintained in separate source repositories.

As a rule of thumb, code in one of the [`src/node/`](./),
[`src/vault/`](../vault/), or [`src/qt/`](../qt/) directories should avoid
calling code in the other directories directly, and only invoke it indirectly
through the more limited [`src/interfaces/`](../interfaces/) classes.

This directory is at the moment
sparsely populated. Eventually more substantial files like
[`src/validation.cpp`](../validation.cpp) and
[`src/txrelaypool.cpp`](../txrelaypool.cpp) might be moved there.
