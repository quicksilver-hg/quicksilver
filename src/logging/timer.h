// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_LOGGING_TIMER_H
#define QUICKSILVER_LOGGING_TIMER_H

#include <logging.h>
#include <util/macros.h>
#include <util/time.h>
#include <util/types.h>

#include <chrono>
#include <optional>
#include <string>


namespace HgLog {

//! RAII-style object that outputs timing information to logs.
template <typename TimeType>
class Timer
{
public:
    //! log_category is mandatory: it becomes the [tag] every line is rendered
    //! with. log_level decides whether the line is gated by -debug= (Debug) or
    //! is unconditional (Info) -- it is deliberately independent of the
    //! category, so an unconditional timer still names its own surface.
    Timer(
        std::string prefix,
        std::string end_msg,
        HgLog::LogFlags log_category,
        HgLog::Level log_level = HgLog::Level::Debug,
        bool msg_on_completion = true)
        : m_prefix(std::move(prefix)),
          m_title(std::move(end_msg)),
          m_log_category(log_category),
          m_log_level(log_level),
          m_message_on_completion(msg_on_completion)
    {
        this->Log(strprintf("%s started", m_title));
        m_start_t = std::chrono::steady_clock::now();
    }

    ~Timer()
    {
        if (m_message_on_completion) {
            this->Log(strprintf("%s completed", m_title));
        } else {
            this->Log("completed");
        }
    }

    void Log(const std::string& msg)
    {
        const std::string full_msg = this->LogMsg(msg);

        if (m_log_level == HgLog::Level::Info) {
            LogInfo(m_log_category, "%s\n", full_msg);
        } else {
            LogDebug(m_log_category, "%s\n", full_msg);
        }
    }

    std::string LogMsg(const std::string& msg)
    {
        const auto end_time{std::chrono::steady_clock::now()};
        if (!m_start_t) {
            return strprintf("%s: %s", m_prefix, msg);
        }
        const auto duration{end_time - *m_start_t};

        if constexpr (std::is_same<TimeType, std::chrono::microseconds>::value) {
            return strprintf("%s: %s (%iμs)", m_prefix, msg, Ticks<std::chrono::microseconds>(duration));
        } else if constexpr (std::is_same<TimeType, std::chrono::milliseconds>::value) {
            return strprintf("%s: %s (%.2fms)", m_prefix, msg, Ticks<MillisecondsDouble>(duration));
        } else if constexpr (std::is_same<TimeType, std::chrono::seconds>::value) {
            return strprintf("%s: %s (%.2fs)", m_prefix, msg, Ticks<SecondsDouble>(duration));
        } else {
            static_assert(ALWAYS_FALSE<TimeType>, "Error: unexpected time type");
        }
    }

private:
    std::optional<std::chrono::steady_clock::time_point> m_start_t{};

    //! Log prefix; usually the name of the function this was created in.
    const std::string m_prefix;

    //! A descriptive message of what is being timed.
    const std::string m_title;

    //! The category this timer logs under; rendered as the line's [tag].
    const HgLog::LogFlags m_log_category;

    //! Info logs unconditionally; Debug is gated by -debug=<category>.
    const HgLog::Level m_log_level;

    //! Whether to output the message again on completion.
    const bool m_message_on_completion;
};

} // namespace HgLog


#define LOG_TIME_MICROS_WITH_CATEGORY(end_msg, log_category) \
    HgLog::Timer<std::chrono::microseconds> UNIQUE_NAME(logging_timer)(__func__, end_msg, log_category)
#define LOG_TIME_MILLIS_WITH_CATEGORY(end_msg, log_category) \
    HgLog::Timer<std::chrono::milliseconds> UNIQUE_NAME(logging_timer)(__func__, end_msg, log_category)
#define LOG_TIME_MILLIS_WITH_CATEGORY_MSG_ONCE(end_msg, log_category, log_level) \
    HgLog::Timer<std::chrono::milliseconds> UNIQUE_NAME(logging_timer)(__func__, end_msg, log_category, log_level, /* msg_on_completion=*/false)
#define LOG_TIME_SECONDS_WITH_CATEGORY(end_msg, log_category) \
    HgLog::Timer<std::chrono::seconds> UNIQUE_NAME(logging_timer)(__func__, end_msg, log_category, HgLog::Level::Info)


#endif // QUICKSILVER_LOGGING_TIMER_H
