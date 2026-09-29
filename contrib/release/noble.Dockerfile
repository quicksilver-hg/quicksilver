# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Refresh this digest only with a full package build, fresh-container install,
# dependency inspection and version-stamp check.
FROM ubuntu@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkgconf git libevent-dev libboost-dev \
    libsqlite3-dev qtbase5-dev libqrencode-dev debhelper dpkg-dev fakeroot \
    && rm -rf /var/lib/apt/lists/*

COPY contrib/release/build-debs.sh /usr/local/bin/build-debs
ENTRYPOINT ["/usr/local/bin/build-debs", "--inside", "noble"]
