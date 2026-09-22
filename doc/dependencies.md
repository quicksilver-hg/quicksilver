# Dependencies

These are the dependencies used by Quicksilver.
You can find installation instructions in the `build-*.md` file for your platform.
"Runtime" and "Version Used" are both in reference to the release binaries.

## Compiler

Quicksilver requires one of the following compilers.

| Dependency | Minimum required |
| --- | --- |
| [Clang](https://clang.llvm.org) | [16.0] |
| [GCC](https://gcc.gnu.org) | [11.1] |

## Required

| Dependency | Releases | Version used | Minimum required | Runtime |
| --- | --- | --- | --- | --- |
| CMake | [link](https://cmake.org/) | N/A | [3.22] | No |
| [Boost](../depends/packages/boost.mk) | [link](https://www.boost.org/users/download/) | [1.81.0] | [1.73.0] | No |
| [libevent](../depends/packages/libevent.mk) | [link](https://github.com/libevent/libevent/releases) | [2.1.12-stable] | [2.1.8] | No |
| glibc | [link](https://www.gnu.org/software/libc/) | N/A | [2.31] | Yes |
| Linux Kernel (if building that platform) | [link](https://www.kernel.org/) | N/A | [3.17.0] | Yes |

## Optional

| Dependency | Releases | Version used | Minimum required | Runtime |
| --- | --- | --- | --- | --- |
| [Fontconfig](../depends/packages/fontconfig.mk) (gui) | [link](https://www.freedesktop.org/wiki/Software/fontconfig/) | [2.12.6] | 2.6 | Yes |
| [FreeType](../depends/packages/freetype.mk) (gui) | [link](https://freetype.org) | [2.11.0] | 2.3.0 | Yes |
| [qrencode](../depends/packages/qrencode.mk) (gui) | [link](https://fukuchi.org/works/qrencode/) | [4.1.1] | N/A | No |
| [Qt](../depends/packages/qt.mk) (gui) | [link](https://download.qt.io/official_releases/qt/) | [5.15.16] | [5.11.3] | No |
| [ZeroMQ](../depends/packages/zeromq.mk) (notifications) | [link](https://github.com/zeromq/libzmq/releases) | [4.3.5] | 4.0.0 | No |
| [SQLite](../depends/packages/sqlite.mk) (vault) | [link](https://sqlite.org) | [3.46.1] | [3.7.17] | No |
| Python (scripts, tests) | [link](https://www.python.org) | N/A | [3.10] | No |
| [systemtap](../depends/packages/systemtap.mk) ([tracing](tracing.md)) | [link](https://sourceware.org/systemtap/) | [4.8]| N/A | No |
| Tor ([tor.md](tor.md)) (gui) | [link](https://www.torproject.org/download/tor/) | N/A | [0.2.7] | Yes |

Tor is a **runtime** dependency, and nothing links against it: the desktop
starts and supervises `tor` as a child process. It is supplied
by the platform package manager on Unix, and by `contrib/tor/fetch-tor.ps1` —
which pins a SHA-256 — on Windows. The version floor is whatever supports
`ControlPort auto`, which Tor has since 0.2.7; every packaged Tor since 2015
satisfies it, so there is no practical floor to police. `quicksilverd` defaults
to `-bundledtor=0`. It needs a Tor proxy to reach the onion seed unless it is
given an explicit peer, and needs Tor's control port additionally to host an
onion service (see [Bootstrapping](bootstrapping.md)).
