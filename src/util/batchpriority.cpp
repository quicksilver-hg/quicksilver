// Copyright (c) 2023 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <util/syserror.h>

#ifndef WIN32
#include <sched.h>
#ifdef SCHED_BATCH
#include <pthread.h>
#endif
#endif

void ScheduleBatchPriority()
{
#ifdef SCHED_BATCH
    const static sched_param param{};
    const int rc = pthread_setschedparam(pthread_self(), SCHED_BATCH, &param);
    if (rc != 0) {
        LogInfo(HgLog::INIT, "Failed to pthread_setschedparam: %s\n", SysErrorString(rc));
    }
#endif
}
