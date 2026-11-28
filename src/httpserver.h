// Copyright (c) 2015-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_HTTPSERVER_H
#define QUICKSILVER_HTTPSERVER_H

#include <functional>
#include <cassert>
#include <condition_variable>
#include <sync.h>
#include <unordered_map>
#include <span>
#include <string>

namespace util {
class SignalInterrupt;
} // namespace util

/**
 * The default value for `-rpcthreads`. This number of threads will be created at startup.
 */
static const int DEFAULT_HTTP_THREADS=16;

/**
 * The default value for `-rpcworkqueue`. This is the maximum depth of the work queue,
 * we don't allocate this number of work queue items upfront.
 */
static const int DEFAULT_HTTP_WORKQUEUE=64;

static const int DEFAULT_HTTP_SERVER_TIMEOUT=30;

struct evhttp_request;
struct evhttp_connection;
struct event_base;
class CService;
class HTTPRequest;

namespace http_detail {
class HTTPRequestTracker
{
    mutable Mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::unordered_map<const evhttp_connection*, size_t> m_tracker GUARDED_BY(m_mutex);
    bool m_closing GUARDED_BY(m_mutex){false};

    void RemoveConnectionInternal(const decltype(m_tracker)::iterator it) EXCLUSIVE_LOCKS_REQUIRED(m_mutex)
    {
        m_tracker.erase(it);
        if (m_tracker.empty()) m_cv.notify_all();
    }
public:
    bool AddRequest(const evhttp_connection* conn) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        if (m_closing) return false;
        ++m_tracker[conn];
        return true;
    }
    void RemoveRequest(const evhttp_connection* conn) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        auto it{m_tracker.find(conn)};
        if (it != m_tracker.end() && it->second > 0 && --it->second == 0) RemoveConnectionInternal(it);
    }
    void RemoveConnection(const evhttp_connection* conn) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        auto it{m_tracker.find(conn)};
        if (it != m_tracker.end()) RemoveConnectionInternal(it);
    }
    void Close() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        WITH_LOCK(m_mutex, m_closing = true);
    }
    void Open() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        assert(m_tracker.empty());
        m_closing = false;
    }
    size_t CountActiveConnections() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        return WITH_LOCK(m_mutex, return m_tracker.size());
    }
    void WaitUntilEmpty() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        WAIT_LOCK(m_mutex, lock);
        m_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_tracker.empty(); });
    }
};
} // namespace http_detail

/** Initialize HTTP server.
 * Call this before RegisterHTTPHandler or EventBase().
 */
bool InitHTTPServer(const util::SignalInterrupt& interrupt);
/** Start HTTP server.
 * This is separate from InitHTTPServer to give users race-condition-free time
 * to register their handlers between InitHTTPServer and StartHTTPServer.
 */
void StartHTTPServer();
/** Interrupt HTTP server threads */
void InterruptHTTPServer();
/** Stop HTTP server */
void StopHTTPServer();

/** Change logging level for libevent. */
void UpdateHTTPServerLogging(bool enable);

/** Handler for requests to a certain HTTP path */
typedef std::function<void(HTTPRequest* req, const std::string &)> HTTPRequestHandler;
/** Register handler for prefix.
 * If multiple handlers match a prefix, the first-registered one will
 * be invoked.
 */
void RegisterHTTPHandler(const std::string &prefix, bool exactMatch, const HTTPRequestHandler &handler);
/** Unregister handler for prefix */
void UnregisterHTTPHandler(const std::string &prefix, bool exactMatch);

/** Return evhttp event base. This can be used by submodules to
 * queue timers or custom events.
 */
struct event_base* EventBase();

/** In-flight HTTP request.
 * Thin C++ wrapper around evhttp_request.
 */
class HTTPRequest
{
private:
    struct evhttp_request* req;
    const util::SignalInterrupt& m_interrupt;
    bool replySent;

public:
    explicit HTTPRequest(struct evhttp_request* req, const util::SignalInterrupt& interrupt, bool replySent = false);
    ~HTTPRequest();
    // Owns the libevent request and its single reply: a copy would reply twice.
    HTTPRequest(const HTTPRequest&) = delete;
    HTTPRequest& operator=(const HTTPRequest&) = delete;

    enum RequestMethod {
        UNKNOWN,
        GET,
        POST,
        HEAD,
        PUT
    };

    /** Get requested URI.
     */
    std::string GetURI() const;

    /** Get CService (address:port) for the origin of the http request.
     */
    CService GetPeer() const;

    /** Get request method.
     */
    RequestMethod GetRequestMethod() const;

    /**
     * Get the request header specified by hdr, or an empty string.
     * Return a pair (isPresent,string).
     */
    std::pair<bool, std::string> GetHeader(const std::string& hdr) const;

    /**
     * Read request body.
     *
     * @note As this consumes the underlying buffer, call this only once.
     * Repeated calls will return an empty string.
     */
    std::string ReadBody();

    /**
     * Write output header.
     *
     * @note call this before calling WriteReply.
     */
    void WriteHeader(const std::string& hdr, const std::string& value);

    /**
     * Write HTTP reply.
     * nStatus is the HTTP status code to send.
     * reply is the body of the reply. Keep it empty to send a standard message.
     *
     * @note Can be called only once. As this will give the request back to the
     * main thread, do not call any other HTTPRequest methods after calling this.
     */
    void WriteReply(int nStatus, std::string_view reply = "")
    {
        WriteReply(nStatus, std::as_bytes(std::span{reply}));
    }
    void WriteReply(int nStatus, std::span<const std::byte> reply);
};

/** Event handler closure.
 */
class HTTPClosure
{
public:
    virtual void operator()() = 0;
    virtual ~HTTPClosure() = default;
};

/** Event class. This can be used either as a cross-thread trigger or as a timer.
 */
class HTTPEvent
{
public:
    /** Create a new event.
     * deleteWhenTriggered deletes this event object after the event is triggered (and the handler called)
     * handler is the handler to call when the event is triggered.
     */
    HTTPEvent(struct event_base* base, bool deleteWhenTriggered, const std::function<void()>& handler);
    ~HTTPEvent();
    // Owns the libevent event: a copy would free it twice.
    HTTPEvent(const HTTPEvent&) = delete;
    HTTPEvent& operator=(const HTTPEvent&) = delete;

    /** Trigger the event. If tv is 0, trigger it immediately. Otherwise trigger it after
     * the given time has elapsed.
     */
    void trigger(struct timeval* tv);

    bool deleteWhenTriggered;
    std::function<void()> handler;
private:
    struct event* ev{nullptr};
};

#endif // QUICKSILVER_HTTPSERVER_H
