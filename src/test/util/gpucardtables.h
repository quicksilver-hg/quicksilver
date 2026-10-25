// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_TEST_UTIL_GPUCARDTABLES_H
#define QUICKSILVER_TEST_UTIL_GPUCARDTABLES_H

#include <crypto/cuckatoo/gpu_devices.h>
#include <crypto/cuckatoo/multi_device.h>

//! Clears the two process-global card tables -- the device stats table
//! (multi_device.cpp) and the discovery cache (gpu_devices.cpp) -- before and
//! after every case in the suite that carries it.
//!
//! Both are process-global std::maps, so whatever one case leaves in them the
//! next case reads. That cost F-550 three sightings and two sessions to
//! root-cause: multi_device_tests left a faulted row behind and a later case
//! read it, which in CI looks like an intermittent rather than a dependency.
//! F-555 is the structural answer: every suite that touches either table takes
//! this fixture, so a case nobody has written yet is isolated too. Resetting on
//! the way out as well means a suite carrying it also leaves the tables clean
//! for whatever runs next, whichever order ctest picks.
//!
//! It is a base, not a replacement: a suite with its own setup inherits from
//! both (see MiningServiceTestSetup and agent_headerchain_tests).
struct GpuCardTablesTestSetup {
    GpuCardTablesTestSetup() { Reset(); }
    ~GpuCardTablesTestSetup() { Reset(); }

    static void Reset()
    {
        cuckatoo::ResetGpuDeviceStatsForTesting();
        cuckatoo::ResetGpuDiscoveryForTesting();
    }
};

#endif // QUICKSILVER_TEST_UTIL_GPUCARDTABLES_H
