# Debian packaging

These files build Debian/Ubuntu packages for Quicksilver. Debian tooling
expects the packaging directory to be `debian/` at the source root, so build
from the repository root with a symlink:

    ln -sfn contrib/debian debian
    dpkg-buildpackage -us -uc -b

This produces two packages:

- `quicksilver`       - graphical Qt client and agent runtime
- `quicksilver-devtools` - headless daemon and command-line tools

The `quicksilver` package is the user-facing desktop artifact and does not
pull in the daemon or command-line tools. Install `quicksilver-devtools`
separately when developer or service tooling is needed.

Build dependencies are declared in `control`. Install them with
`sudo apt build-dep .` (after the symlink) or read them from `control`.

The `.desktop` launcher, AppStream metainfo, and menu icons are installed by
the project's CMake rules (`src/qt/CMakeLists.txt`), so they land in the staged
tree automatically during the build - the `.install` files only route them
into the `quicksilver` package.

The maintainer address (`quicksilver.maintainers@gmail.com`) is a shared role
mailbox, and the homepage is the public repository
(`https://github.com/quicksilver-hg/quicksilver`).
