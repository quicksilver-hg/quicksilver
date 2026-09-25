# Libraries

| Name                     | Description |
|--------------------------|-------------|
| *libquicksilver_cli*         | RPC client functionality used by *quicksilver-cli* executable |
| *libquicksilver_common*      | Home for common functionality shared by different executables and libraries. Similar to *libquicksilver_util*, but higher-level (see [Dependencies](#dependencies)). |
| *libquicksilver_consensus*   | Consensus functionality used by *libquicksilver_node* and *libquicksilver_vault*. |
| *libquicksilver_crypto*      | Hardware-optimized functions for data encryption, hashing, message authentication, and key derivation. |
| *libquicksilver_kernel*      | Consensus engine and support library used for validation by *libquicksilver_node*. |
| *libquicksilver_qt*      | GUI functionality used by the *quicksilver* executable. |
| *libquicksilver_node*        | P2P and RPC server functionality used by *quicksilver-daemon* and *quicksilver* executables. |
| *libquicksilver_util*        | Home for common functionality shared by different executables and libraries. Similar to *libquicksilver_common*, but lower-level (see [Dependencies](#dependencies)). |
| *libquicksilver_vault*      | Vault functionality used by *quicksilver-daemon* and *quicksilver-vault* executables. |
| *libquicksilver_vault_tool* | Lower-level vault functionality used by *quicksilver-vault* executable. |
| *libquicksilver_zmq*         | [ZeroMQ](../zmq.md) functionality used by *quicksilver-daemon* and *quicksilver* executables. |

## Conventions

- Most libraries are internal libraries and have APIs which are completely unstable! There are few or no restrictions on backwards compatibility or rules about external dependencies. An exception is *libquicksilver_kernel*, which, at some future point, will have a documented external interface.

- Generally each library should have a corresponding source directory and namespace. Source code organization is a work in progress, so it is true that some namespaces are applied inconsistently, and if you look at [`add_library(quicksilver_* ...)`](../../src/CMakeLists.txt) lists you can see that many libraries pull in files from outside their source directory. But when working with libraries, it is good to follow a consistent pattern like:

  - *libquicksilver_node* code lives in `src/node/` in the `node::` namespace
  - *libquicksilver_vault* code lives in `src/vault/` in the `vault::` namespace
  - *libquicksilver_util* code lives in `src/util/` in the `util::` namespace
  - *libquicksilver_consensus* code lives in `src/consensus/` in the `Consensus::` namespace

## Dependencies

- Libraries should minimize what other libraries they depend on, and only reference symbols following the arrows shown in the dependency graph below:

<table><tr><td>

```mermaid

%%{ init : { "flowchart" : { "curve" : "basis" }}}%%

graph TD;

quicksilver-cli[quicksilver-cli]-->libquicksilver_cli;

quicksilver-daemon[quicksilver-daemon]-->libquicksilver_node;
quicksilver-daemon[quicksilver-daemon]-->libquicksilver_vault;

quicksilver[quicksilver]-->libquicksilver_node;
quicksilver[quicksilver]-->libquicksilver_qt;
quicksilver[quicksilver]-->libquicksilver_vault;

quicksilver-vault[quicksilver-vault]-->libquicksilver_vault;
quicksilver-vault[quicksilver-vault]-->libquicksilver_vault_tool;

libquicksilver_cli-->libquicksilver_util;
libquicksilver_cli-->libquicksilver_common;

libquicksilver_consensus-->libquicksilver_crypto;

libquicksilver_common-->libquicksilver_consensus;
libquicksilver_common-->libquicksilver_crypto;
libquicksilver_common-->libquicksilver_util;

libquicksilver_kernel-->libquicksilver_consensus;
libquicksilver_kernel-->libquicksilver_crypto;
libquicksilver_kernel-->libquicksilver_util;

libquicksilver_node-->libquicksilver_consensus;
libquicksilver_node-->libquicksilver_crypto;
libquicksilver_node-->libquicksilver_kernel;
libquicksilver_node-->libquicksilver_common;
libquicksilver_node-->libquicksilver_util;

libquicksilver_qt-->libquicksilver_common;
libquicksilver_qt-->libquicksilver_util;

libquicksilver_util-->libquicksilver_crypto;

libquicksilver_vault-->libquicksilver_common;
libquicksilver_vault-->libquicksilver_crypto;
libquicksilver_vault-->libquicksilver_util;

libquicksilver_vault_tool-->libquicksilver_vault;
libquicksilver_vault_tool-->libquicksilver_util;

classDef bold stroke-width:2px, font-weight:bold, font-size: smaller;
class quicksilver,quicksilver-daemon,quicksilver-cli,quicksilver-vault bold
```
</td></tr><tr><td>

**Dependency graph**. Arrows show linker symbol dependencies. *Crypto* lib depends on nothing. *Util* lib is depended on by everything. *Kernel* lib depends only on consensus, crypto, and util.

</td></tr></table>

- The graph shows what _linker symbols_ (functions and variables) from each library other libraries can call and reference directly, but it is not a call graph. For example, there is no arrow connecting *libquicksilver_vault* and *libquicksilver_node* libraries, because these libraries are intended to be modular and not depend on each other's internal implementation details. But vault code is still able to call node code indirectly through the `interfaces::Chain` abstract class in [`interfaces/chain.h`](../../src/interfaces/chain.h) and node code calls vault code through the `interfaces::ChainClient` and `interfaces::Chain::Notifications` abstract classes in the same file. In general, defining abstract classes in [`src/interfaces/`](../../src/interfaces/) can be a convenient way of avoiding unwanted direct dependencies or circular dependencies between libraries.

- *libquicksilver_crypto* should be a standalone dependency that any library can depend on, and it should not depend on any other libraries itself.

- *libquicksilver_consensus* should only depend on *libquicksilver_crypto*, and all other libraries besides *libquicksilver_crypto* should be allowed to depend on it.

- *libquicksilver_util* should be a standalone dependency that any library can depend on, and it should not depend on other libraries except *libquicksilver_crypto*. It provides basic utilities that fill in gaps in the C++ standard library and provide lightweight abstractions over platform-specific features. Since the util library is distributed with the kernel and is usable by kernel applications, it shouldn't contain functions that external code shouldn't call, like higher level code targeted at the node or vault. (*libquicksilver_common* is a better place for higher level code, or code that is meant to be used by internal applications only.)

- *libquicksilver_common* is a home for miscellaneous shared code used by different Quicksilver applications. It should not depend on anything other than *libquicksilver_util*, *libquicksilver_consensus*, and *libquicksilver_crypto*.

- *libquicksilver_kernel* should only depend on *libquicksilver_util*, *libquicksilver_consensus*, and *libquicksilver_crypto*.

- The only thing that should depend on *libquicksilver_kernel* internally should be *libquicksilver_node*. GUI and vault libraries *libquicksilver_qt* and *libquicksilver_vault* in particular should not depend on *libquicksilver_kernel* and the unneeded functionality it would pull in, like block validation. To the extent that GUI and vault code need scripting and signing functionality, they should be able to get it from *libquicksilver_consensus*, *libquicksilver_common*, *libquicksilver_crypto*, and *libquicksilver_util*, instead of *libquicksilver_kernel*.

- GUI, node, and vault code internal implementations should all be independent of each other, and the *libquicksilver_qt*, *libquicksilver_node*, *libquicksilver_vault* libraries should never reference each other's symbols. They should only call each other through [`src/interfaces/`](../../src/interfaces/) abstract interfaces.