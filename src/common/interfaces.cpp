// Copyright (c) 2021-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/handler.h>

#include <boost/signals2/connection.hpp>
#include <memory>
#include <utility>

namespace common {
namespace {
class CleanupHandler : public interfaces::Handler
{
public:
    explicit CleanupHandler(std::function<void()> cleanup) : m_cleanup(std::move(cleanup)) {}
    ~CleanupHandler() override { if (!m_cleanup) return; m_cleanup(); m_cleanup = nullptr; }
    void disconnect() override { if (!m_cleanup) return; m_cleanup(); m_cleanup = nullptr; }
    std::function<void()> m_cleanup;
};

class SignalHandler : public interfaces::Handler
{
public:
    explicit SignalHandler(boost::signals2::connection connection) : m_connection(std::move(connection)) {}

    void disconnect() override { m_connection.disconnect(); }

    boost::signals2::scoped_connection m_connection;
};
} // namespace
} // namespace common

namespace interfaces {
std::unique_ptr<Handler> MakeCleanupHandler(std::function<void()> cleanup)
{
    return std::make_unique<common::CleanupHandler>(std::move(cleanup));
}

std::unique_ptr<Handler> MakeSignalHandler(boost::signals2::connection connection)
{
    return std::make_unique<common::SignalHandler>(std::move(connection));
}
} // namespace interfaces
