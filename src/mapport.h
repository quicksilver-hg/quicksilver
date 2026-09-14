// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_MAPPORT_H
#define QUICKSILVER_MAPPORT_H

static constexpr bool DEFAULT_NATPMP = false;

void StartMapPort(bool enable);
void InterruptMapPort();
void StopMapPort();

#endif // QUICKSILVER_MAPPORT_H
