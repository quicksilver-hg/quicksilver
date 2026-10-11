Quicksilver
=============

Intro
-----
Quicksilver is a feeless peer-to-peer currency. Users hold the keys to their
own money and transact directly with each other. A P2P network checks for
double-spending. There is no central server.


Setup
-----
To start Quicksilver after installation, open the Start Menu and select
"Quicksilver (64-bit)".

Quicksilver reaches the network through Tor. The installer puts the Tor
Project's tor.exe beside quicksilver.exe (its licenses are in
LICENSE-tor.txt); nothing needs installing or configuring. Windows Defender
Firewall may ask whether to allow Quicksilver the first time the node starts.

To use the public test network instead of the main one, switch networks on the
Network page.

This is a fresh chain. Early public networks are expected to be much smaller
than mature chains, but disk, CPU, memory, and network requirements will grow
with usage. A node that cannot reach Tor needs an explicit peer configured
with -addnode, -connect, or -seednode.
