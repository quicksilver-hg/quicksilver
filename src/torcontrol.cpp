// Copyright (c) 2015-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Copyright (c) 2017 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <torcontrol.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <compat/compat.h>
#include <crypto/hmac_sha256.h>
#include <logging.h>
#include <net.h>
#include <netaddress.h>
#include <netbase.h>
#include <random.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/readwritefile.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/thread.h>
#include <util/time.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>

using util::SplitString;
using util::ToString;

/** Default control ip and port */
const std::string DEFAULT_TOR_CONTROL = "127.0.0.1:" + ToString(DEFAULT_TOR_CONTROL_PORT);
/** Tor cookie size (from control-spec.txt) */
static const int TOR_COOKIE_SIZE = 32;
/** Size of client/server nonce for SAFECOOKIE */
static const int TOR_NONCE_SIZE = 32;
/** For computing serverHash in SAFECOOKIE */
static const std::string TOR_SAFE_SERVERKEY = "Tor safe cookie authentication server-to-controller hash";
/** For computing clientHash in SAFECOOKIE */
static const std::string TOR_SAFE_CLIENTKEY = "Tor safe cookie authentication controller-to-server hash";
/** Exponential backoff configuration - initial timeout in seconds */
static const float RECONNECT_TIMEOUT_START = 1.0;
/** Exponential backoff configuration - growth factor */
static const float RECONNECT_TIMEOUT_EXP = 1.5;
/** Maximum length for lines received on TorControlConnection.
 * tor-control-spec.txt mentions that there is explicitly no limit defined to line length,
 * this is belt-and-suspenders sanity limit to prevent memory exhaustion.
 */
static const int MAX_LINE_LENGTH = 100000;
static constexpr size_t MAX_REPLY_LINES = 1000;
static constexpr size_t MAX_REPLY_BYTES = 1 << 20;
static constexpr float MAX_RECONNECT_TIMEOUT = 600.0F;
static const uint16_t DEFAULT_TOR_SOCKS_PORT = 9050;

/****** Low-level TorControlConnection ********/

TorControlConnection::TorControlConnection(struct event_base* _base)
    : base(_base)
{
}

TorControlConnection::~TorControlConnection()
{
    DisconnectWithoutNotification();
}

bool TorControlConnection::AddReplyLine(const std::string& s)
{
    if (s.size() > MAX_LINE_LENGTH || message.lines.size() >= MAX_REPLY_LINES ||
        s.size() > MAX_REPLY_BYTES - message_bytes) {
        LogInfo(HgLog::TOR, "tor: Disconnecting because reply bounds exceeded\n");
        Disconnect();
        return false;
    }
    if (s.size() < 4) return true;
    message_bytes += s.size();
    message.code = ToIntegral<int>(s.substr(0, 3)).value_or(0);
    message.lines.push_back(s.substr(4));
    if (s[3] == ' ') {
        if (message.code < 600) {
            if (!reply_handlers.empty()) {
                auto handler = std::move(reply_handlers.front());
                reply_handlers.pop_front();
                handler(*this, message);
            } else {
                LogDebug(HgLog::TOR, "Received unexpected sync reply %i\n", message.code);
            }
        }
        message.Clear();
        message_bytes = 0;
    }
    return true;
}

void TorControlConnection::readcb(struct bufferevent *bev, void *ctx)
{
    TorControlConnection *self = static_cast<TorControlConnection*>(ctx);
    struct evbuffer *input = bufferevent_get_input(bev);
    size_t n_read_out = 0;
    char *line;
    assert(input);
    //  If there is not a whole line to read, evbuffer_readln returns nullptr
    while((line = evbuffer_readln(input, &n_read_out, EVBUFFER_EOL_CRLF)) != nullptr)
    {
        std::string s(line, n_read_out);
        free(line);
        if (!self->AddReplyLine(s) || self->b_conn != bev) return;
    }
    //  Check for size of buffer - protect against memory exhaustion with very long lines
    //  Do this after evbuffer_readln to make sure all full lines have been
    //  removed from the buffer. Everything left is an incomplete line.
    if (evbuffer_get_length(input) > MAX_LINE_LENGTH) {
        LogInfo(HgLog::TOR, "tor: Disconnecting because MAX_LINE_LENGTH exceeded\n");
        self->Disconnect();
    }
}

void TorControlConnection::eventcb(struct bufferevent *bev, short what, void *ctx)
{
    TorControlConnection *self = static_cast<TorControlConnection*>(ctx);
    if (what & BEV_EVENT_CONNECTED) {
        LogDebug(HgLog::TOR, "Successfully connected!\n");
        self->connected(*self);
    } else if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
        if (what & BEV_EVENT_ERROR) {
            LogDebug(HgLog::TOR, "Error connecting to Tor control socket\n");
        } else {
            LogDebug(HgLog::TOR, "End of stream\n");
        }
        self->Disconnect();
    }
}

bool TorControlConnection::Connect(const std::string& tor_control_center, const ConnectionCB& _connected, const ConnectionCB& _disconnected)
{
    DisconnectWithoutNotification();
    connected = _connected;
    disconnected = _disconnected;
    connection_active = true;

    const std::optional<CService> control_service{Lookup(tor_control_center, DEFAULT_TOR_CONTROL_PORT, fNameLookup)};
    if (!control_service.has_value()) {
        LogInfo(HgLog::TOR, "tor: Failed to look up control center %s\n", tor_control_center);
        Disconnect();
        return false;
    }

    struct sockaddr_storage control_address;
    socklen_t control_address_len = sizeof(control_address);
    if (!control_service.value().GetSockAddr(reinterpret_cast<struct sockaddr*>(&control_address), &control_address_len)) {
        LogInfo(HgLog::TOR, "tor: Error parsing socket address %s\n", tor_control_center);
        Disconnect();
        return false;
    }

    // Create a new socket, set up callbacks and enable notification bits
    b_conn = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!b_conn) {
        Disconnect();
        return false;
    }
    bufferevent_setcb(b_conn, TorControlConnection::readcb, nullptr, TorControlConnection::eventcb, this);
    bufferevent_enable(b_conn, EV_READ|EV_WRITE);

    // Finally, connect to tor_control_center
    if (bufferevent_socket_connect(b_conn, reinterpret_cast<struct sockaddr*>(&control_address), control_address_len) < 0) {
        LogInfo(HgLog::TOR, "tor: Error connecting to address %s\n", tor_control_center);
        Disconnect();
        return false;
    }
    return true;
}

void TorControlConnection::Disconnect()
{
    const bool notify = connection_active;
    const auto callback = disconnected;
    DisconnectWithoutNotification();
    if (notify && callback) callback(*this);
}

void TorControlConnection::DisconnectWithoutNotification()
{
    connection_active = false;
    if (b_conn)
        bufferevent_free(b_conn);
    b_conn = nullptr;
    message.Clear();
    message_bytes = 0;
    reply_handlers.clear();
}

bool TorControlConnection::Command(const std::string &cmd, const ReplyHandlerCB& reply_handler)
{
    if (!b_conn)
        return false;
    struct evbuffer *buf = bufferevent_get_output(b_conn);
    if (!buf)
        return false;
    evbuffer_add(buf, cmd.data(), cmd.size());
    evbuffer_add(buf, "\r\n", 2);
    reply_handlers.push_back(reply_handler);
    return true;
}

/****** General parsing utilities ********/

/* Split reply line in the form 'AUTH METHODS=...' into a type
 * 'AUTH' and arguments 'METHODS=...'.
 * Grammar is implicitly defined in https://spec.torproject.org/control-spec by
 * the server reply formats for PROTOCOLINFO (S3.21) and AUTHCHALLENGE (S3.24).
 */
std::pair<std::string,std::string> SplitTorReplyLine(const std::string &s)
{
    size_t ptr=0;
    std::string type;
    while (ptr < s.size() && s[ptr] != ' ') {
        type.push_back(s[ptr]);
        ++ptr;
    }
    if (ptr < s.size())
        ++ptr; // skip ' '
    return make_pair(type, s.substr(ptr));
}

/** Parse reply arguments in the form 'METHODS=COOKIE,SAFECOOKIE COOKIEFILE=".../control_auth_cookie"'.
 * Returns a map of keys to values, or an empty map if there was an error.
 * Grammar is implicitly defined in https://spec.torproject.org/control-spec by
 * the server reply formats for PROTOCOLINFO (S3.21), AUTHCHALLENGE (S3.24),
 * and ADD_ONION (S3.27). See also sections 2.1 and 2.3.
 */
std::map<std::string,std::string> ParseTorReplyMapping(const std::string &s)
{
    std::map<std::string,std::string> mapping;
    size_t ptr=0;
    while (ptr < s.size()) {
        std::string key, value;
        while (ptr < s.size() && s[ptr] != '=' && s[ptr] != ' ') {
            key.push_back(s[ptr]);
            ++ptr;
        }
        if (ptr == s.size()) // unexpected end of line
            return std::map<std::string,std::string>();
        if (s[ptr] == ' ') // The remaining string is an OptArguments
            break;
        ++ptr; // skip '='
        if (ptr < s.size() && s[ptr] == '"') { // Quoted string
            ++ptr; // skip opening '"'
            bool escape_next = false;
            while (ptr < s.size() && (escape_next || s[ptr] != '"')) {
                // Repeated backslashes must be interpreted as pairs
                escape_next = (s[ptr] == '\\' && !escape_next);
                value.push_back(s[ptr]);
                ++ptr;
            }
            if (ptr == s.size()) // unexpected end of line
                return std::map<std::string,std::string>();
            ++ptr; // skip closing '"'
            /**
             * Unescape value. Per https://spec.torproject.org/control-spec section 2.1.1:
             *
             *   For future-proofing, controller implementers MAY use the following
             *   rules to be compatible with buggy Tor implementations and with
             *   future ones that implement the spec as intended:
             *
             *     Read \n \t \r and \0 ... \377 as C escapes.
             *     Treat a backslash followed by any other character as that character.
             */
            std::string escaped_value;
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\') {
                    // This will always be valid, because if the QuotedString
                    // ended in an odd number of backslashes, then the parser
                    // would already have returned above, due to a missing
                    // terminating double-quote.
                    ++i;
                    if (value[i] == 'n') {
                        escaped_value.push_back('\n');
                    } else if (value[i] == 't') {
                        escaped_value.push_back('\t');
                    } else if (value[i] == 'r') {
                        escaped_value.push_back('\r');
                    } else if ('0' <= value[i] && value[i] <= '7') {
                        size_t j;
                        // Octal escape sequences have a limit of three octal digits,
                        // but terminate at the first character that is not a valid
                        // octal digit if encountered sooner.
                        for (j = 1; j < 3 && (i+j) < value.size() && '0' <= value[i+j] && value[i+j] <= '7'; ++j) {}
                        // Tor restricts first digit to 0-3 for three-digit octals.
                        // A leading digit of 4-7 would therefore be interpreted as
                        // a two-digit octal.
                        if (j == 3 && value[i] > '3') {
                            j--;
                        }
                        const auto end{i + j};
                        uint8_t val{0};
                        while (i < end) {
                            val *= 8;
                            val += value[i++] - '0';
                        }
                        escaped_value.push_back(char(val));
                        // Account for automatic incrementing at loop end
                        --i;
                    } else {
                        escaped_value.push_back(value[i]);
                    }
                } else {
                    escaped_value.push_back(value[i]);
                }
            }
            value = escaped_value;
        } else { // Unquoted value. Note that values can contain '=' at will, just no spaces
            while (ptr < s.size() && s[ptr] != ' ') {
                value.push_back(s[ptr]);
                ++ptr;
            }
        }
        if (ptr < s.size() && s[ptr] == ' ')
            ++ptr; // skip ' ' after key=value
        mapping[key] = value;
    }
    return mapping;
}

TorController::TorController(struct event_base* _base, const std::string& tor_control_center, const CService& target):
    base(_base),
    m_tor_control_center(tor_control_center), conn(base), reconnect(true), reconnect_timeout(RECONNECT_TIMEOUT_START),
    m_target(target)
{
    reconnect_ev = event_new(base, -1, 0, reconnect_cb, this);
    if (!reconnect_ev)
        LogInfo(HgLog::TOR, "tor: Failed to create event for reconnection: out of memory?\n");
    // Start connection attempts immediately
    if (!conn.Connect(m_tor_control_center, std::bind(&TorController::connected_cb, this, std::placeholders::_1),
         std::bind(&TorController::disconnected_cb, this, std::placeholders::_1) )) {
        LogInfo(HgLog::TOR, "tor: Initiating connection to Tor control port %s failed\n", m_tor_control_center);
    }
    // Read service private key if cached
    std::pair<bool,std::string> pkf = ReadBinaryFile(GetPrivateKeyFile());
    if (pkf.first) {
        LogDebug(HgLog::TOR, "Reading cached private key from %s\n", fs::PathToString(GetPrivateKeyFile()));
        private_key = pkf.second;
    }
}

int64_t TorController::NextReconnectDelayMillis(float& timeout)
{
    timeout = std::min(timeout, MAX_RECONNECT_TIMEOUT);
    const int64_t delay = static_cast<int64_t>(timeout * 1000.0F);
    timeout = std::min(timeout * RECONNECT_TIMEOUT_EXP, MAX_RECONNECT_TIMEOUT);
    return delay;
}

std::string TorController::PasswordAuthCommand(std::string_view password)
{
    std::string escaped;
    escaped.reserve(password.size());
    for (const char ch : password) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return "AUTHENTICATE \"" + escaped + "\"";
}

TorController::~TorController()
{
    if (reconnect_ev) {
        event_free(reconnect_ev);
        reconnect_ev = nullptr;
    }
    if (service.IsValid()) {
        RemoveLocal(service);
    }
}

std::optional<Proxy> TorController::SocksProxyFromListener(const std::string& listener)
{
    if (listener.starts_with(ADDR_PREFIX_UNIX)) {
        // Unix listeners are only usable where this build has Unix sockets.
        if (!IsUnixSocketPath(listener)) return std::nullopt;
        return Proxy(listener, /*_randomize_credentials=*/true);
    }
    const CService resolved{LookupNumeric(listener, DEFAULT_TOR_SOCKS_PORT)};
    if (!resolved.IsValid()) return std::nullopt;
    return Proxy(resolved, /*_randomize_credentials=*/true);
}

void TorController::get_socks_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    // NOTE: We can only get here if -onion is unset
    std::string socks_location;
    if (reply.code == 250) {
        for (const auto& line : reply.lines) {
            if (line.starts_with("net/listeners/socks=")) {
                const std::string port_list_str = line.substr(20);
                std::vector<std::string> port_list = SplitString(port_list_str, ' ');

                for (auto& portstr : port_list) {
                    if (portstr.empty()) continue;
                    if ((portstr[0] == '"' || portstr[0] == '\'') && portstr.size() >= 2 && (*portstr.rbegin() == portstr[0])) {
                        portstr = portstr.substr(1, portstr.size() - 2);
                        if (portstr.empty()) continue;
                    }
                    socks_location = portstr;
                    if (portstr.starts_with("127.0.0.1:")) {
                        // Prefer localhost - ignore other ports
                        break;
                    }
                }
            }
        }
        if (!socks_location.empty()) {
            LogDebug(HgLog::TOR, "Get SOCKS port command yielded %s\n", socks_location);
        } else {
            LogInfo(HgLog::TOR, "tor: Get SOCKS port command returned nothing\n");
        }
    } else if (reply.code == 510) {  // 510 Unrecognized command
        LogInfo(HgLog::TOR, "tor: Get SOCKS port command failed with unrecognized command (You probably should upgrade Tor)\n");
    } else {
        LogInfo(HgLog::TOR, "tor: Get SOCKS port command failed; error code %d\n", reply.code);
    }

    std::optional<Proxy> onion_proxy;
    if (socks_location.empty()) {
        // Tor reported no listener (or is too old to answer): assume its default port.
        onion_proxy = Proxy(LookupNumeric("127.0.0.1", DEFAULT_TOR_SOCKS_PORT), true);
    } else {
        onion_proxy = SocksProxyFromListener(socks_location);
        if (!onion_proxy) {
            // Tor named a listener we cannot use. Substituting 127.0.0.1:9050 would mark
            // onion reachable through a proxy that may not exist: leave it unconfigured.
            LogWarning(HgLog::TOR, "tor: cannot use the SOCKS listener Tor reported (%s); onion proxy not configured\n", socks_location);
            return;
        }
    }

    LogDebug(HgLog::TOR, "Configuring onion proxy for %s\n", onion_proxy->ToString());
    SetProxy(NET_ONION, *onion_proxy);

    const auto onlynets = gArgs.GetArgs("-onlynet");

    const bool onion_allowed_by_onlynet{
        onlynets.empty() ||
        std::any_of(onlynets.begin(), onlynets.end(), [](const auto& n) {
            return ParseNetwork(n) == NET_ONION;
        })};

    if (onion_allowed_by_onlynet) {
        // If NET_ONION is reachable, then the below is a noop.
        //
        // If NET_ONION is not reachable, then none of -proxy or -onion was given.
        // Since we are here, then -torcontrol and -torpassword were given.
        g_reachable_nets.Add(NET_ONION);
    }
}

void TorController::add_onion_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        LogDebug(HgLog::TOR, "ADD_ONION successful\n");
        for (const std::string &s : reply.lines) {
            std::map<std::string,std::string> m = ParseTorReplyMapping(s);
            std::map<std::string,std::string>::iterator i;
            if ((i = m.find("ServiceID")) != m.end())
                service_id = i->second;
            if ((i = m.find("PrivateKey")) != m.end())
                private_key = i->second;
        }
        if (service_id.empty()) {
            LogInfo(HgLog::TOR, "tor: Error parsing ADD_ONION parameters:\n");
            for (const std::string &s : reply.lines) {
                LogInfo(HgLog::TOR, "    %s\n", SanitizeString(s));
            }
            return;
        }
        service = LookupNumeric(std::string(service_id+".onion"), Params().GetDefaultPort());
        LogInfo(HgLog::TOR, "Got tor service ID %s, advertising service %s\n", service_id, service.ToStringAddrPort());
        if (WriteBinaryFile(GetPrivateKeyFile(), private_key)) {
            LogDebug(HgLog::TOR, "Cached service private key to %s\n", fs::PathToString(GetPrivateKeyFile()));
        } else {
            LogInfo(HgLog::TOR, "tor: Error writing service private key to %s\n", fs::PathToString(GetPrivateKeyFile()));
        }
        AddLocal(service, LOCAL_MANUAL);
        // ... onion requested - keep connection open
    } else if (reply.code == 510) { // 510 Unrecognized command
        LogInfo(HgLog::TOR, "tor: Add onion failed with unrecognized command (You probably need to upgrade Tor)\n");
    } else {
        LogInfo(HgLog::TOR, "tor: Add onion failed; error code %d\n", reply.code);
    }
}

void TorController::auth_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        LogDebug(HgLog::TOR, "Authentication successful\n");

        // Now that we know Tor is running setup the proxy for onion addresses
        // if -onion isn't set to something else.
        if (gArgs.GetArg("-onion", "") == "") {
            _conn.Command("GETINFO net/listeners/socks", std::bind(&TorController::get_socks_cb, this, std::placeholders::_1, std::placeholders::_2));
        }

        // Finally - now create the service
        if (private_key.empty()) { // No private key, generate one
            private_key = "NEW:ED25519-V3"; // Explicitly request key type - see issue #9214
        }
        // Request onion service, redirect port.
        // Note that the 'virtual' port is always the default port to avoid decloaking nodes using other ports.
        _conn.Command(strprintf("ADD_ONION %s Port=%i,%s", private_key, Params().GetDefaultPort(), m_target.ToStringAddrPort()),
            std::bind(&TorController::add_onion_cb, this, std::placeholders::_1, std::placeholders::_2));
    } else {
        LogInfo(HgLog::TOR, "tor: Authentication failed\n");
    }
}

/** Compute Tor SAFECOOKIE response.
 *
 *    ServerHash is computed as:
 *      HMAC-SHA256("Tor safe cookie authentication server-to-controller hash",
 *                  CookieString | ClientNonce | ServerNonce)
 *    (with the HMAC key as its first argument)
 *
 *    After a controller sends a successful AUTHCHALLENGE command, the
 *    next command sent on the connection must be an AUTHENTICATE command,
 *    and the only authentication string which that AUTHENTICATE command
 *    will accept is:
 *
 *      HMAC-SHA256("Tor safe cookie authentication controller-to-server hash",
 *                  CookieString | ClientNonce | ServerNonce)
 *
 */
static std::vector<uint8_t> ComputeResponse(const std::string &key, const std::vector<uint8_t> &cookie,  const std::vector<uint8_t> &clientNonce, const std::vector<uint8_t> &serverNonce)
{
    CHMAC_SHA256 computeHash((const uint8_t*)key.data(), key.size());
    std::vector<uint8_t> computedHash(CHMAC_SHA256::OUTPUT_SIZE, 0);
    computeHash.Write(cookie.data(), cookie.size());
    computeHash.Write(clientNonce.data(), clientNonce.size());
    computeHash.Write(serverNonce.data(), serverNonce.size());
    computeHash.Finalize(computedHash.data());
    return computedHash;
}

void TorController::authchallenge_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        LogDebug(HgLog::TOR, "SAFECOOKIE authentication challenge successful\n");
        std::pair<std::string,std::string> l = SplitTorReplyLine(reply.lines[0]);
        if (l.first == "AUTHCHALLENGE") {
            std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
            if (m.empty()) {
                LogInfo(HgLog::TOR, "tor: Error parsing AUTHCHALLENGE parameters: %s\n", SanitizeString(l.second));
                return;
            }
            std::vector<uint8_t> serverHash = ParseHex(m["SERVERHASH"]);
            std::vector<uint8_t> serverNonce = ParseHex(m["SERVERNONCE"]);
            LogDebug(HgLog::TOR, "AUTHCHALLENGE ServerHash %s ServerNonce %s\n", HexStr(serverHash), HexStr(serverNonce));
            if (serverNonce.size() != 32) {
                LogInfo(HgLog::TOR, "tor: ServerNonce is not 32 bytes, as required by spec\n");
                return;
            }

            std::vector<uint8_t> computedServerHash = ComputeResponse(TOR_SAFE_SERVERKEY, cookie, clientNonce, serverNonce);
            if (computedServerHash != serverHash) {
                LogInfo(HgLog::TOR, "tor: ServerHash %s does not match expected ServerHash %s\n", HexStr(serverHash), HexStr(computedServerHash));
                return;
            }

            std::vector<uint8_t> computedClientHash = ComputeResponse(TOR_SAFE_CLIENTKEY, cookie, clientNonce, serverNonce);
            _conn.Command("AUTHENTICATE " + HexStr(computedClientHash), std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
        } else {
            LogInfo(HgLog::TOR, "tor: Invalid reply to AUTHCHALLENGE\n");
        }
    } else {
        LogInfo(HgLog::TOR, "tor: SAFECOOKIE authentication challenge failed\n");
    }
}

void TorController::protocolinfo_cb(TorControlConnection& _conn, const TorControlReply& reply)
{
    if (reply.code == 250) {
        std::set<std::string> methods;
        std::string cookiefile;
        /*
         * 250-AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE="/home/x/.tor/control_auth_cookie"
         * 250-AUTH METHODS=NULL
         * 250-AUTH METHODS=HASHEDPASSWORD
         */
        for (const std::string &s : reply.lines) {
            std::pair<std::string,std::string> l = SplitTorReplyLine(s);
            if (l.first == "AUTH") {
                std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
                std::map<std::string,std::string>::iterator i;
                if ((i = m.find("METHODS")) != m.end()) {
                    std::vector<std::string> m_vec = SplitString(i->second, ',');
                    methods = std::set<std::string>(m_vec.begin(), m_vec.end());
                }
                if ((i = m.find("COOKIEFILE")) != m.end())
                    cookiefile = i->second;
            } else if (l.first == "VERSION") {
                std::map<std::string,std::string> m = ParseTorReplyMapping(l.second);
                std::map<std::string,std::string>::iterator i;
                if ((i = m.find("Tor")) != m.end()) {
                    LogDebug(HgLog::TOR, "Connected to Tor version %s\n", i->second);
                }
            }
        }
        for (const std::string &s : methods) {
            LogDebug(HgLog::TOR, "Supported authentication method: %s\n", s);
        }
        // Prefer NULL, otherwise SAFECOOKIE. If a password is provided, use HASHEDPASSWORD
        /* Authentication:
         *   cookie:   hex-encoded ~/.tor/control_auth_cookie
         *   password: "password"
         */
        std::string torpassword = gArgs.GetArg("-torpassword", "");
        if (!torpassword.empty()) {
            if (methods.count("HASHEDPASSWORD")) {
                LogDebug(HgLog::TOR, "Using HASHEDPASSWORD authentication\n");
                _conn.Command(PasswordAuthCommand(torpassword), std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
            } else {
                LogInfo(HgLog::TOR, "tor: Password provided with -torpassword, but HASHEDPASSWORD authentication is not available\n");
            }
        } else if (methods.count("NULL")) {
            LogDebug(HgLog::TOR, "Using NULL authentication\n");
            _conn.Command("AUTHENTICATE", std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
        } else if (methods.count("SAFECOOKIE")) {
            // Cookie: hexdump -e '32/1 "%02x""\n"'  ~/.tor/control_auth_cookie
            LogDebug(HgLog::TOR, "Using SAFECOOKIE authentication, reading cookie authentication from %s\n", cookiefile);
            std::pair<bool,std::string> status_cookie = ReadBinaryFile(fs::PathFromString(cookiefile), TOR_COOKIE_SIZE);
            if (status_cookie.first && status_cookie.second.size() == TOR_COOKIE_SIZE) {
                // _conn.Command("AUTHENTICATE " + HexStr(status_cookie.second), std::bind(&TorController::auth_cb, this, std::placeholders::_1, std::placeholders::_2));
                cookie = std::vector<uint8_t>(status_cookie.second.begin(), status_cookie.second.end());
                clientNonce = std::vector<uint8_t>(TOR_NONCE_SIZE, 0);
                GetRandBytes(clientNonce);
                _conn.Command("AUTHCHALLENGE SAFECOOKIE " + HexStr(clientNonce), std::bind(&TorController::authchallenge_cb, this, std::placeholders::_1, std::placeholders::_2));
            } else {
                if (status_cookie.first) {
                    LogInfo(HgLog::TOR, "tor: Authentication cookie %s is not exactly %i bytes, as is required by the spec\n", cookiefile, TOR_COOKIE_SIZE);
                } else {
                    LogInfo(HgLog::TOR, "tor: Authentication cookie %s could not be opened (check permissions)\n", cookiefile);
                }
            }
        } else if (methods.count("HASHEDPASSWORD")) {
            LogInfo(HgLog::TOR, "tor: The only supported authentication mechanism left is password, but no password provided with -torpassword\n");
        } else {
            LogInfo(HgLog::TOR, "tor: No supported authentication method\n");
        }
    } else {
        LogInfo(HgLog::TOR, "tor: Requesting protocol info failed\n");
    }
}

void TorController::connected_cb(TorControlConnection& _conn)
{
    reconnect_timeout = RECONNECT_TIMEOUT_START;
    // First send a PROTOCOLINFO command to figure out what authentication is expected
    if (!_conn.Command("PROTOCOLINFO 1", std::bind(&TorController::protocolinfo_cb, this, std::placeholders::_1, std::placeholders::_2)))
        LogInfo(HgLog::TOR, "tor: Error sending initial protocolinfo command\n");
}

void TorController::disconnected_cb(TorControlConnection& _conn)
{
    // Stop advertising service when disconnected
    if (service.IsValid())
        RemoveLocal(service);
    service = CService();
    if (!reconnect)
        return;

    LogDebug(HgLog::TOR, "Not connected to Tor control port %s, trying to reconnect\n", m_tor_control_center);

    // Single-shot timer for reconnect. Use exponential backoff.
    struct timeval time = MillisToTimeval(NextReconnectDelayMillis(reconnect_timeout));
    if (reconnect_ev)
        event_add(reconnect_ev, &time);
}

void TorController::Reconnect()
{
    /* Try to reconnect and reestablish if we get booted - for example, Tor
     * may be restarting.
     */
    if (!conn.Connect(m_tor_control_center, std::bind(&TorController::connected_cb, this, std::placeholders::_1),
         std::bind(&TorController::disconnected_cb, this, std::placeholders::_1) )) {
        LogInfo(HgLog::TOR, "tor: Re-initiating connection to Tor control port %s failed\n", m_tor_control_center);
    }
}

fs::path TorController::GetPrivateKeyFile()
{
    return gArgs.GetDataDirNet() / "onion_v3_private_key";
}

void TorController::reconnect_cb(evutil_socket_t fd, short what, void *arg)
{
    TorController *self = static_cast<TorController*>(arg);
    self->Reconnect();
}

/****** Thread ********/
static struct event_base *gBase;
static std::thread torControlThread;

static void TorControlThread(CService onion_service_target)
{
    TorController ctrl(gBase, gArgs.GetArg("-torcontrol", DEFAULT_TOR_CONTROL), onion_service_target);

    event_base_dispatch(gBase);
}

void StartTorControl(CService onion_service_target)
{
    assert(!gBase);
#ifdef WIN32
    evthread_use_windows_threads();
#else
    evthread_use_pthreads();
#endif
    gBase = event_base_new();
    if (!gBase) {
        LogInfo(HgLog::TOR, "tor: Unable to create event_base\n");
        return;
    }

    torControlThread = std::thread(&util::TraceThread, "torcontrol", [onion_service_target] {
        TorControlThread(onion_service_target);
    });
}

void InterruptTorControl()
{
    if (gBase) {
        LogInfo(HgLog::TOR, "tor: Thread interrupt\n");
        event_base_once(gBase, -1, EV_TIMEOUT, [](evutil_socket_t, short, void*) {
            event_base_loopbreak(gBase);
        }, nullptr, nullptr);
    }
}

void StopTorControl()
{
    if (gBase) {
        torControlThread.join();
        event_base_free(gBase);
        gBase = nullptr;
    }
}

CService DefaultOnionServiceTarget(uint16_t port)
{
    struct in_addr onion_service_target;
    onion_service_target.s_addr = htonl(INADDR_LOOPBACK);
    return {onion_service_target, port};
}
