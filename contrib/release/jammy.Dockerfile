# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Ubuntu 22.04 supplies CMake 3.22.1. CMakeLists.txt currently requires 3.22;
# raising cmake_minimum_required above 3.22 would remove this release target.
# Refresh this digest only with a full package build, fresh-container install,
# dependency inspection and version-stamp check.
FROM ubuntu@sha256:b8b6ee6aa931ecd9d0d952abc34dc0e5f7c6a30c6bb71b079fe399fde0329c02

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkgconf git libevent-dev libboost-dev \
    libsqlite3-dev qtbase5-dev libqrencode-dev debhelper dpkg-dev fakeroot \
    && rm -rf /var/lib/apt/lists/*

COPY contrib/release/build-jammy-debs.sh /usr/local/bin/build-jammy-debs
ENTRYPOINT ["/usr/local/bin/build-jammy-debs", "--inside"]
