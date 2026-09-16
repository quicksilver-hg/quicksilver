# TOR SUPPORT IN QUICKSILVER

It is possible to run Quicksilver as a Tor onion service, and connect to such services.

The following directions assume you have a Tor proxy running on port 9050. Many distributions default to having a SOCKS proxy listening on port 9050, but others may not. In particular, the Tor Browser Bundle defaults to listening on port 9150.

**Running the desktop?** None of the setup below is required: the Quicksilver
desktop starts and supervises a Tor of its own. See
[section 0](#0-the-desktop-starts-its-own-tor). The two quick starts that follow
are for `quicksilverd` operators, and for anyone who would rather run their own
Tor.

## Which parts do you actually need?

This page and [Bootstrapping](bootstrapping.md) describe the same Tor with
different amounts of it turned on, which is easy to read as three conflicting
recipes. There are only two jobs, and they have separate requirements:

| Goal | What it needs | What it does not need |
|---|---|---|
| **Reach the onion seed and sync** — what a new node needs | A SOCKS proxy, normally `127.0.0.1:9050`, given to the node as `-proxy=` or `-onion=` | The control port, cookie authentication, group membership, any torrc edit |
| **Host an onion service of your own** — accept inbound Tor peers | All of the above, plus Tor's control port, its authentication, and read access to the cookie | — |

A stock `tor` package on most distributions gives you the first with no
configuration at all. The torrc edits in the quick starts below exist for the
second. If a node is syncing happily and you have no need to accept inbound
connections, you are finished after installing Tor and passing `-proxy=`.

`getnetworkinfo` tells the two apart: `networks[].reachable` for `onion` covers
the first, and a `.onion` entry in `localaddresses` shows the second is
working.

## Debian and Ubuntu quick start

Installing the `tor` package is not the whole setup. Stock packages commonly
leave the control port commented out, so Quicksilver cannot create its onion
service until Tor's control API and authentication are configured. Also set the
onion SOCKS proxy explicitly so outbound onion peers do not depend on proxy
autodetection.

1. In `/etc/tor/torrc`, add or uncomment:

   ```text
   ControlPort 9051
   CookieAuthentication 1
   CookieAuthFileGroupReadable 1
   ```

2. Restart Tor with `sudo systemctl restart tor`.
3. Give the account running Quicksilver read access to Tor's authentication
   cookie (normally through the `debian-tor` group; see the authentication
   section below).
4. Start Quicksilver with `-onion=127.0.0.1:9050`, or enable the separate Tor
   SOCKS5 proxy at `127.0.0.1:9050` in **Controls > Options > Network** and
   restart Quicksilver.

The two ports have different jobs: `9050` is the SOCKS5 proxy used to reach
onion peers; `9051` is the authenticated control API used to create the local
onion service. A working SOCKS port does not prove that the control port is
enabled.

## Windows quick start

Nothing in the Debian and Ubuntu section above applies on Windows: there is no
`/etc/tor/torrc`, no distribution packaging and no Tor service group. The two
things Quicksilver needs are the same, but they are configured differently.

1. Install the [Tor Expert Bundle](https://www.torproject.org/download/tor/) and
   unpack it somewhere you can write to. Run `tor.exe` under the **same Windows
   account** that runs Quicksilver; that is what makes the control port's
   authentication cookie readable, and it replaces the Tor-group step entirely.

2. Edit the torrc that Tor is actually reading — the Expert Bundle ships one at
   `Data\Tor\torrc` inside the folder you unpacked — and add:

   ```text
   ControlPort 9051
   CookieAuthentication 1
   ```

   `CookieAuthFileGroupReadable` and `DataDirectoryGroupReadable` are Unix
   permission settings; leave them out on Windows. Restart `tor.exe`.

3. Start Quicksilver with `-onion=127.0.0.1:9050`, or enable the separate Tor
   SOCKS5 proxy at `127.0.0.1:9050` in **Controls > Options > Network** and
   restart Quicksilver.

Tor Browser can be used instead of the Expert Bundle, but its SOCKS5 proxy
listens on `127.0.0.1:9150`, not `9050`, and it only runs while the browser is
open. Its control port is not enabled by default either, so a node relying on it
can reach onion peers without ever being able to create its own onion service.

## 0. The desktop starts its own Tor

The Quicksilver desktop starts and supervises a Tor of its own. Nothing on this
page needs doing for a desktop first run: no Tor service, no torrc, no control
port, no cookie group membership.

The desktop writes `<datadir>/tor/torrc`, starts `tor` with it, and asks Tor to
choose free ports for both its control port and its SOCKS port. Nothing binds
9050 or 9051, so a machine that is already running its own Tor sees no conflict.
The node's Tor exits with the node.

- `-bundledtor=0` turns this off and uses a Tor you run yourself, exactly as the
  rest of this document describes. The daemon (`quicksilverd`) defaults to 0:
  someone running a daemon can install and configure Tor; someone who downloaded
  a desktop cannot be assumed to.
- `-bundledtorpath=<path>` names the `tor` executable. By default the desktop
  looks for one beside its own executable, then on `PATH`.

On Windows, a source checkout can fetch the pinned Tor Expert Bundle and print
the resulting executable path with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File contrib\tor\fetch-tor.ps1
```

The bypass applies only to that PowerShell process; it does not change the
user's or machine's execution policy. The script verifies the archive's pinned
SHA-256 before extracting it under `build\tor`. See the
[Windows build guide](build-windows-msvc.md#4-tor-desktop-only) for details.

**Where things are:** `<datadir>/tor/torrc` (regenerated at every start — edits
are lost), `<datadir>/tor/data` (Tor's own state), `<datadir>/tor/tor.log`
(Tor's log, and the first place to look when startup fails),
`<datadir>/tor/control_port` (the port Tor chose this run).

**macOS** keeps the operator-installed-Tor path: there is no Mac in the project's
test fleet, and shipping untested process supervision for a platform that cannot
be gated is worse than shipping none.

**Running the bundled Tor does not make the desktop Tor-only.** It changes how
the desktop *reaches* peers; it does not change what the desktop *offers*. With
`-listen` at its default the node still binds the network's P2P port on all
interfaces, still accepts inbound clearnet connections, and still advertises the
machine's own routable addresses — global IPv6 included — beside its onion
address.

This catches people out because the equivalent daemon setup does not do it:
`-proxy=` switches address discovery off as a parameter interaction, and the
bundled Tor is wired in through the control port rather than `-proxy`, so that
interaction never fires. `getnetworkinfo` → `localaddresses` shows what is
actually being advertised.

To stop advertising, without giving up the bundled Tor:

```bash
quicksilver-qt -discover=0                    # advertise nothing about this host
quicksilver-qt -discover=0 -onlynet=onion     # and dial only onion peers
```

⚠ Do **not** reach for `-listen=0` here. It switches off `-listenonion`, and the
bundled Tor only starts when `-listenonion` is on, so the node ends up with no
Tor and no route to the onion seed. A desktop that genuinely wants `-listen=0`
must run its own Tor: `-bundledtor=0` with `-onion=` or `-proxy=`.

Section 4 below covers the rest of the privacy configuration.

## Compatibility

- Quicksilver only supports Tor version 3 hidden services (Tor v3). Tor v2
  addresses are ignored by Quicksilver and neither relayed nor stored.

- Tor removed v2 support beginning with version 0.4.6.

## How to see information about your Tor configuration via Quicksilver

There are several ways to see your local onion address in Quicksilver:
- in the "Local addresses" output of CLI `-netinfo`
- in the "localaddresses" output of RPC `getnetworkinfo`
- in the debug log (grep for "AddLocal"; the Tor address ends in `.onion`)

You may set the `-debug=tor` config logging option to have additional
information in the debug log about your Tor configuration.

CLI `-addrinfo` returns the number of addresses known to your node per
network. This can be useful to see how many onion peers your node knows,
e.g. for `-onlynet=onion`.

You can use the `getnodeaddresses` RPC to fetch a number of onion peers known to your node; run `quicksilver-cli help getnodeaddresses` for details.

## 1. Run Quicksilver behind a Tor proxy

The first step is running Quicksilver behind a Tor proxy. This will already anonymize all
outgoing connections, but more is possible.

    -proxy=ip:port  Set the proxy server. If SOCKS5 is selected (default), this proxy
                    server will be used to try to reach .onion addresses as well.
                    You need to use -noonion or -onion=0 to explicitly disable
                    outbound access to onion services.

    -onion=ip:port  Set the proxy server to use for Tor onion services. You do not
                    need to set this if it's the same as -proxy. You can use -onion=0
                    to explicitly disable access to onion services.
                    ------------------------------------------------------------------
                    Note: Only the -proxy option sets the proxy for DNS requests;
                    with -onion they will not route over Tor, so use -proxy if you
                    have privacy concerns.
                    ------------------------------------------------------------------

    -listen         When using -proxy, listening is disabled by default. If you want
                    to manually configure an onion service (see section 3), you'll
                    need to enable it explicitly.

    -connect=X      When behind a Tor proxy, you can specify .onion addresses instead
    -addnode=X      of IP addresses or hostnames in these parameters. It requires
    -seednode=X     SOCKS5. In Tor mode, such addresses can also be exchanged with
                    other P2P nodes.

    -onlynet=onion  Make automatic outbound connections only to .onion addresses.
                    Inbound and manual connections are not affected by this option.
                    It can be specified multiple times to allow multiple networks,
                    e.g. onlynet=onion, onlynet=i2p, onlynet=cjdns.

In a typical situation, this suffices to run behind a Tor proxy:

    quicksilverd -proxy=127.0.0.1:9050

## 2. Automatically create a Quicksilver onion service

Quicksilver makes use of Tor's control socket API to create and destroy
ephemeral onion services programmatically. This means that if Tor is running and
proper authentication has been configured, Quicksilver automatically creates an
onion service to listen on. The goal is to increase the number of available
onion nodes.

This feature is enabled by default if Quicksilver is listening (`-listen`) and
it requires a Tor connection to work. It can be explicitly disabled with
`-listenonion=0`. If it is not disabled, it can be configured using the
`-torcontrol` and `-torpassword` settings.

To see verbose Tor information in the quicksilverd debug log, pass `-debug=tor`.

### Control Port

You may need to set up the Tor Control Port. On Linux distributions there may be
some or all of the following settings in `/etc/tor/torrc`, generally commented
out by default (if not, add them):

```
ControlPort 9051
CookieAuthentication 1
CookieAuthFileGroupReadable 1
DataDirectoryGroupReadable 1
```

Add or uncomment those, save, and restart Tor (usually `systemctl restart tor`
or `sudo systemctl restart tor` on most systemd-based systems, including recent
Debian and Ubuntu, or just restart the computer).

### Authentication

Connecting to Tor's control socket API requires one of two authentication
methods to be configured: cookie authentication or quicksilverd's `-torpassword`
configuration option.

#### Cookie authentication

For cookie authentication, the user running quicksilverd must have read access to
the `CookieAuthFile` specified in the Tor configuration. In some cases this is
preconfigured and the creation of an onion service is automatic. Don't forget to
use the `-debug=tor` quicksilverd configuration option to enable Tor debug logging.

If a permissions problem is seen in the debug log, e.g. `tor: Authentication
cookie /run/tor/control.authcookie could not be opened (check permissions)`, it
can be resolved by adding both the user running Tor and the user running
quicksilverd to the same Tor group and setting permissions appropriately.

On Debian-derived systems, the Tor group will likely be `debian-tor` and one way
to verify could be to list the groups and grep for a "tor" group name:

```
getent group | cut -d: -f1 | grep -i tor
```

You can also check the group of the cookie file. On most Linux systems, the Tor
auth cookie will usually be `/run/tor/control.authcookie`:

```
TORGROUP=$(stat -c '%G' /run/tor/control.authcookie)
```

Once you have determined the `${TORGROUP}` and selected the `${USER}` that will
run quicksilverd, add that user to the group. This needs root, so run it under
`sudo` (drop the `sudo` if you are already root):

```
sudo usermod -a -G "${TORGROUP}" "${USER}"
```

`${USER}` is your own login name. Set it explicitly first if quicksilverd will
run as a different account, for example `USER=quicksilver`.

Then restart the computer (or log out) and log in as the `${USER}` that will run
quicksilverd.

#### `torpassword` authentication

For the `-torpassword=password` option, the password is the clear text form that
was used when generating the hashed password for the `HashedControlPassword`
option in the Tor configuration file.

The hashed password can be obtained with the command `tor --hash-password
password` (refer to the [Tor Dev
Manual](https://2019.www.torproject.org/docs/tor-manual.html.en) for more
details).


## 3. Manually create a Quicksilver onion service

You can also manually configure your node to be reachable from the Tor network.
Add these lines to your `/etc/tor/torrc` (or equivalent config file):

    HiddenServiceDir /var/lib/tor/quicksilver-service/
    HiddenServicePort 9555 127.0.0.1:9556

The directory can be different of course, but virtual port numbers should be equal to
your quicksilverd's P2P listen port (9555 by default), and target addresses and ports
should be equal to binding address and port for inbound Tor connections (127.0.0.1:9556 by default).

    -externalip=X   You can tell Quicksilver about its publicly reachable addresses using
                    this option, and this can be an onion address. Given the above
                    configuration, you can find your onion address in
                    /var/lib/tor/quicksilver-service/hostname. For connections
                    coming from unroutable addresses (such as 127.0.0.1, where the
                    Tor proxy typically runs), onion addresses are given
                    preference for your node to advertise itself with.

                    You can set multiple local addresses with -externalip. The
                    one that will be rumoured to a particular peer is the most
                    compatible one and also using heuristics, e.g. the address
                    with the most incoming connections, etc.

    -listen         You'll need to enable listening for incoming connections, as this
                    is off by default behind a proxy.

    -discover       When -externalip is specified, no attempt is made to discover local
                    IPv4 or IPv6 addresses. If you want to run a dual stack, reachable
                    from both Tor and IPv4 (or IPv6), you'll need to either pass your
                    other addresses using -externalip, or explicitly enable -discover.
                    Note that both addresses of a dual-stack system may be easily
                    linkable using traffic analysis.

In a typical situation, where you're only reachable via Tor, this should suffice:

    quicksilverd -proxy=127.0.0.1:9050 -externalip=7zvj7a2imdgkdbg4f2dryd5rgtrn7upivr5eeij4cicjh65pooxeshid.onion -listen

(obviously, replace the .onion address with your own). It should be noted that you still
listen on all devices and another node could establish a clearnet connection, when knowing
your address. To mitigate this, additionally bind the address of your Tor proxy:

    quicksilverd ... -bind=127.0.0.1:9556=onion

If you don't care too much about hiding your node, and want to be reachable on IPv4
as well, use `discover` instead:

    quicksilverd ... -discover

and open port 9555 on your firewall (or use port mapping, i.e., `-natpmp`).

If you only want to use Tor to reach .onion addresses, but not use it as a proxy
for normal IPv4/IPv6 communication, use:

    quicksilverd -onion=127.0.0.1:9050 -externalip=7zvj7a2imdgkdbg4f2dryd5rgtrn7upivr5eeij4cicjh65pooxeshid.onion -discover

## 4. Privacy recommendations

- Do not add anything but Quicksilver ports to the onion service created in section 3.
  If you run a web service too, create a new onion service for that.
  Otherwise it is trivial to link them, which may reduce privacy. Onion
  services created automatically (as in section 2) always have only one port
  open.
