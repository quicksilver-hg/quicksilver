# JSON-RPC Interface

The headless daemon `quicksilverd` has the JSON-RPC API enabled by default, the GUI
`quicksilver-qt` has it disabled by default. This can be changed with the `-server`
option. The GUI can execute RPC methods regardless, on the **Console** tab of its
**Node window** (`Ctrl+Shift+C`, or `Ctrl+Shift+D` and pick the tab): that path
calls the node in process rather than over HTTP, so it needs neither `-server`
nor the credentials described below.

## Endpoints

There are two JSON-RPC endpoints on the server:

1. `/`
2. `/vault/<vaultname>/`

### `/` endpoint

This endpoint is always active.
It can always service non-vault requests and can service vault requests when
exactly one vault is loaded.

### `/vault/<vaultname>/` endpoint

This endpoint is only activated when the vault component has been compiled in.
It can service both vault and non-vault requests.
It MUST be used for vault requests when two or more vaults are loaded.

This is the endpoint used by quicksilver-cli when a `-rpcvault=` parameter is passed in.

Best practice would dictate using the `/vault/<vaultname>/` endpoint for ALL
requests when multiple vaults are in use.

### Examples

```sh
# Get block count from the / endpoint when rpcuser=alice and rpcport=9554
$ curl --user alice --data-binary '{"jsonrpc": "2.0", "id": "0", "method": "getblockcount", "params": []}' -H 'content-type: application/json' localhost:9554/

# Get balance from the /vault/vaultname endpoint when rpcuser=alice, rpcport=9554 and rpcvault=desc-vault
$ curl --user alice --data-binary '{"jsonrpc": "2.0", "id": "0", "method": "getbalance", "params": []}' -H 'content-type: application/json' localhost:9554/vault/desc-vault

```

## Parameter passing

The JSON-RPC server supports both _by-position_ and _by-name_ [parameter
structures](https://www.jsonrpc.org/specification#parameter_structures)
described in the JSON-RPC specification. For extra convenience, to avoid the
need to name every parameter value, all RPC methods accept a named parameter
called `args`, which can be set to an array of initial positional values that
are combined with named values.

Examples:

```sh
# "params": ["myvault", false, false, "", false, false, true]
quicksilver-cli createvault myvault false false "" false false true

# "params": {"vault_name": "myvault", "load_on_startup": true}
quicksilver-cli -named createvault vault_name=myvault load_on_startup=true

# "params": {"args": ["myvault"], "load_on_startup": true}
quicksilver-cli -named createvault myvault load_on_startup=true
```

## Versioning

The RPC interface might change from one major version of Quicksilver to the
next. This makes the RPC interface implicitly versioned on the major version.
The version tuple can be retrieved by e.g. the `getnetworkinfo` RPC in
`version`.

The release notes document public RPC changes in each major release.

## JSON-RPC 2.0

The server implements [JSON-RPC v2.0](https://www.jsonrpc.org/specification).
Every request must include `"jsonrpc": "2.0"`; markerless requests and other
protocol versions are rejected. Responses include the same marker and exactly
one of `result` or `error`. Notifications omit the `id` field and return HTTP
status `204` with no response body.

## Security

The RPC interface allows other programs to control Quicksilver,
including the ability to spend funds from your vaults, affect consensus
verification, read private data, and otherwise perform operations that
can cause loss of money, data, or privacy.  This section suggests how
you should use and configure Quicksilver to reduce the risk that its
RPC interface will be abused.

- **Securing the executable:** Anyone with physical or remote access to
  the computer, container, or virtual machine running Quicksilver can
  compromise either the whole program or just the RPC interface.  This
  includes being able to record any passphrases you enter for unlocking
  your encrypted vaults or changing settings so that your Quicksilver
  program tells you that certain transactions have multiple
  confirmations even when they aren't part of the best block chain.  For
  this reason, you should not use Quicksilver for security sensitive
  operations on systems you do not exclusively control, such as shared
  computers or virtual private servers.

- **Securing local network access:** By default, the RPC interface can
  only be accessed by a client running on the same computer and only
  after the client provides a valid authentication credential (username
  and passphrase).  Any program on your computer with access to the file
  system and local network can obtain this level of access.
  Additionally, other programs on your computer can attempt to provide
  an RPC interface on the same port as used by Quicksilver in order to
  trick you into revealing your authentication credentials.  For this
  reason, it is important to only use Quicksilver for
  security-sensitive operations on a computer whose other programs you
  trust.

- **Securing remote network access:** You may optionally allow other
  computers to remotely control Quicksilver by setting the `rpcallowip`
  and `rpcbind` configuration parameters.  These settings are only meant
  for enabling connections over secure private networks or connections
  that have been otherwise secured (e.g. using a VPN or port forwarding
  with SSH or stunnel).  **Do not enable RPC connections over the public
  Internet.**  Although Quicksilver's RPC interface does use
  authentication, it does not use encryption, so your login credentials
  are sent as clear text that can be read by anyone on your network
  path.  Additionally, the RPC interface has not been hardened to
  withstand arbitrary Internet traffic, so changing the above settings
  to expose it to the Internet (even using something like a Tor onion
  service) could expose you to unconsidered vulnerabilities.  See
  `quicksilverd -help` for more information about these settings and other
  settings described in this document.

    Related, if you use Quicksilver inside a Docker container, you may
    need to expose the RPC port to the host system.  The default way to
    do this in Docker also exposes the port to the public Internet.
    Instead, expose it only on the host system's localhost, for example:
    `-p 127.0.0.1:9554:9554`

- **Secure authentication:** By default, when no `rpcpassword` is specified, Quicksilver generates unique
  login credentials each time it restarts and puts them into a file
  readable only by the user that started Quicksilver, allowing any of
  that user's RPC clients with read access to the file to login
  automatically.  The file is `.cookie` in the Quicksilver
  configuration directory, and using these credentials is the preferred
  RPC authentication method.  If you need to generate static login
  credentials for your programs, you can use the script in the
  `share/rpcauth` directory in the Quicksilver source tree.  As a final
  fallback, you can directly use manually-chosen `rpcuser` and
  `rpcpassword` configuration parameters---but you must ensure that you
  choose a strong and unique passphrase (and still don't use insecure
  networks, as mentioned above).

- **Secure string handling:** The RPC interface does not guarantee any
  escaping of data beyond what's necessary to encode it as JSON,
  although it does usually provide serialized data using a hex
  representation of the bytes. If you use RPC data in your programs or
  provide its data to other programs, you must ensure any problem strings
  are properly escaped. For example, the `createvault` RPC accepts
  arguments such as `vault_name` which is a string and could be used
  for a path traversal attack without application level checks. Multiple
  websites have been manipulated because they displayed decoded hex strings
  that included HTML `<script>` tags. For this reason, and others, it is
  recommended to display all serialized data in hex form only.

## RPC consistency guarantees

State that can be queried via RPCs is guaranteed to be at least up-to-date with
the chain state immediately prior to the call's execution. However, the state
returned by RPCs that reflect the relay pool may not be up-to-date with the
current relay pool state.

### Transaction Pool

The relay pool state returned via an RPC is consistent with itself and with the
chain state at the time of the call. Thus, the relay pool state only encompasses
transactions that are considered mine-able by the node at the time of the RPC.

The relay pool state returned via an RPC reflects all effects of relay pool and
chain state related RPCs that returned prior to this call.

### Vault

The vault state returned via an RPC is consistent with itself and with the
chain state at the time of the call.

Vault RPCs will return the latest chain state consistent with prior non-vault
RPCs. The effects of all blocks (and transactions in blocks) at the time of the
call is reflected in the state of all vault transactions. For example, if a
block contains transactions that conflicted with relay pool transactions, the
vault would reflect the removal of these relay pool transactions in the state.

However, the vault may not be up-to-date with the current state of the relay pool
or the state of the relay pool by an RPC that returned before this RPC. For
example, a vault transaction that was replaced in the relay pool prior to
this RPC may not yet be reflected as such in this RPC response.

## Limitations

There is a known issue in the JSON-RPC interface that can cause a node to crash if
too many http connections are being opened at the same time because the system runs
out of available file descriptors. To prevent this from happening you might
want to increase the number of maximum allowed file descriptors in your system
and try to prevent opening too many connections to your JSON-RPC interface at the
same time if this is under your control. It is hard to give general advice
since this depends on your system but if you make several hundred requests at
once you are definitely at risk of encountering this issue.
