// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Bundled Tor: produce a running Tor and report its control port, or fail
// honestly. This unit locates the binary, writes a torrc, spawns a contained
// child, waits for the control port to appear, and tears the process down at
// shutdown. It knows nothing about onion services, proxies or the P2P layer --
// those stay in torcontrol.cpp, which already authenticates with the cookie
// whose path Tor reports, discovers the SOCKS listener itself, and installs the
// onion proxy. That is why this unit stays small enough to reason about.
//
// The desktop needs it because the seed is onion-only: without a Tor, a first
// run on a machine that has never installed one cannot find the network at all.
// quicksilver-daemon does NOT default to it -- a person running a daemon can install
// Tor; a desktop downloader cannot be assumed to.
#ifndef QUICKSILVER_TOR_BUNDLED_TOR_H
#define QUICKSILVER_TOR_BUNDLED_TOR_H

#include <util/fs.h>
#include <util/result.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

//! quicksilver-daemon default. The GUI soft-sets this to true; see
//! QuicksilverApplication::parameterSetup().
static constexpr bool DEFAULT_BUNDLED_TOR{false};

namespace tor {

//! The three paths a generated torrc names, as strings, so that the generator
//! is a pure function a test can drive with either platform's path shapes.
struct TorrcPaths {
    std::string data_dir;
    std::string control_port_file;
    std::string log_file;
};

/**
 * Generate the torrc for a bundled Tor.
 *
 * Every port is `auto`. That is load-bearing, not tidiness: the ruling is to
 * always spawn our own Tor and never adopt one already on the box, which is only
 * cheap because nothing binds a fixed 9050/9051. A machine already running Tor
 * sees no conflict and needs no diagnosis. A fixed port would reintroduce
 * exactly the failure this design exists to remove -- a first run that cannot
 * reach the network, for a reason the user cannot see.
 *
 * `owning_pid` becomes __OwningControllerProcess, which makes Tor exit once this
 * process is gone. The process group covers an ordered shutdown; this covers the
 * one it cannot -- an abruptly killed parent on POSIX, where a process group does
 * not die with the process that created it.
 */
std::string BuildTorrc(const TorrcPaths& paths, int64_t owning_pid);

/**
 * True when `value` can be written into a torrc unquoted and read back
 * unchanged. Tor takes an unquoted value literally to end of line, so a Windows
 * backslash is safe; `#` starts a comment, `"` starts a quoted value, a line
 * break ends the directive early, and a trailing space is stripped -- which
 * would silently name a different directory.
 */
bool IsTorrcSafeValue(const std::string& value);

//! The three states of a control-port file that another process is writing
//! while we read it.
enum class ControlPortParse {
    kIncomplete,  //!< absent, empty, or no terminating newline yet: keep waiting
    kMalformed,   //!< a complete line, and it is not a control port we can use
    kOk,          //!< `endpoint` is set to "host:port"
};

/**
 * Parse the contents of Tor's ControlPortWriteToFile output.
 *
 * The newline requirement is the whole point of the kIncomplete state: Tor's
 * write is not atomic, so a reader that accepts "PORT=127.0.0.1:4" while the
 * rest of "1234" is still in flight connects to the wrong port, or to nothing.
 */
ControlPortParse ParseControlPortFile(const std::string& contents, std::string& endpoint);

/**
 * Locate the tor executable: `override_path` if given, else `binary_name`
 * beside our own executable, else the first `binary_name` on `path_env`.
 *
 * Every platform fact is an argument -- the search separator, the executable
 * name, and the predicate that decides whether a candidate is usable -- so the
 * whole search is testable on one platform. An `override_path` that is not
 * usable returns nullopt rather than falling through: the operator named a file,
 * and quietly running a different one is worse than saying so.
 */
std::optional<fs::path> FindTorBinary(const fs::path& override_path,
                                      const fs::path& exe_dir,
                                      const std::string& path_env,
                                      char path_separator,
                                      const std::string& binary_name,
                                      const std::function<bool(const fs::path&)>& usable);

/**
 * Start Tor under `datadir` and return its control endpoint as "host:port".
 * On failure the returned error is the sentence shown to the user, and no Tor
 * is left running.
 */
util::Result<std::string> StartBundledTor(const fs::path& datadir, const fs::path& override_path);

//! Kill the bundled Tor and its group. Idempotent; safe when none was started.
void StopBundledTor();

} // namespace tor

#endif // QUICKSILVER_TOR_BUNDLED_TOR_H
