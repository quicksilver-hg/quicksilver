// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <algorithm>
#include <cstring>
#include <string>

#if (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <pthread.h>
#include <pthread_np.h>
#endif

#include <util/threadnames.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

//! Set the thread's name at the process level. Does not affect the
//! internal name.
static void SetThreadName(const char* name)
{
#if defined(PR_SET_NAME)
    // Only the first 15 characters are used (16 - NUL terminator)
    ::prctl(PR_SET_NAME, name, 0, 0, 0);
#elif (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
    pthread_set_name_np(pthread_self(), name);
#else
    // Prevent warnings for unused parameters...
    (void)name;
#endif
}

/**
 * The name of the thread. We use char array instead of std::string to avoid
 * complications with running a destructor when the thread exits. Avoid adding
 * other thread_local variables.
 * @see https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=278701
 */
static thread_local char g_thread_name[128]{'\0'};
std::string util::ThreadGetInternalName() { return g_thread_name; }
//! Set the in-memory internal name for this thread. Does not affect the process
//! name.
static void SetInternalName(const std::string& name)
{
    const size_t copy_bytes{std::min(sizeof(g_thread_name) - 1, name.length())};
    std::memcpy(g_thread_name, name.data(), copy_bytes);
    g_thread_name[copy_bytes] = '\0';
}

std::string util::OsThreadName(std::string_view name)
{
    std::string result{"qs-"};
    constexpr size_t MAX_OS_NAME_BYTES{15};
    constexpr size_t TAIL_BYTES{5};
    const size_t available{MAX_OS_NAME_BYTES - result.size()};
    if (name.size() <= available) {
        result.append(name);
    } else {
        result.append(name.substr(0, available - TAIL_BYTES - 1));
        result.push_back('~');
        result.append(name.substr(name.size() - TAIL_BYTES));
    }
    return result;
}

void util::ThreadRename(const std::string& name)
{
    SetThreadName(OsThreadName(name).c_str());
    SetInternalName(name);
}

void util::ThreadSetInternalName(const std::string& name)
{
    SetInternalName(name);
}
