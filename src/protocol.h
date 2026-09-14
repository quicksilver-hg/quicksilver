// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_PROTOCOL_H
#define QUICKSILVER_PROTOCOL_H

#include <kernel/messagestartchars.h> // IWYU pragma: export
#include <netaddress.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <util/time.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

/** Message header.
 * (4) message start.
 * (12) message type.
 * (4) size.
 * (4) checksum.
 */
class CMessageHeader
{
public:
    static constexpr size_t MESSAGE_TYPE_SIZE = 12;
    static constexpr size_t MESSAGE_SIZE_SIZE = 4;
    static constexpr size_t CHECKSUM_SIZE = 4;
    static constexpr size_t MESSAGE_SIZE_OFFSET = std::tuple_size_v<MessageStartChars> + MESSAGE_TYPE_SIZE;
    static constexpr size_t CHECKSUM_OFFSET = MESSAGE_SIZE_OFFSET + MESSAGE_SIZE_SIZE;
    static constexpr size_t HEADER_SIZE = std::tuple_size_v<MessageStartChars> + MESSAGE_TYPE_SIZE + MESSAGE_SIZE_SIZE + CHECKSUM_SIZE;

    explicit CMessageHeader() = default;

    /** Construct a P2P message header from message-start characters, a message type and the size of the message.
     * @note Passing in a `msg_type` longer than MESSAGE_TYPE_SIZE will result in a run-time assertion error.
     */
    CMessageHeader(const MessageStartChars& pchMessageStartIn, const char* msg_type, unsigned int nMessageSizeIn);

    std::string GetMessageType() const;
    bool IsMessageTypeValid() const;

    SERIALIZE_METHODS(CMessageHeader, obj) { READWRITE(obj.pchMessageStart, obj.m_msg_type, obj.nMessageSize, obj.pchChecksum); }

    MessageStartChars pchMessageStart{};
    char m_msg_type[MESSAGE_TYPE_SIZE]{};
    uint32_t nMessageSize{std::numeric_limits<uint32_t>::max()};
    uint8_t pchChecksum[CHECKSUM_SIZE]{};
};

/**
 * Quicksilver protocol message types. When adding new message types, don't forget
 * to update ALL_NET_MESSAGE_TYPES below.
 */
namespace NetMsgType {
/**
 * The version message provides information about the transmitting node to the
 * receiving node at the beginning of a connection.
 */
inline constexpr const char* VERSION{"version"};
/**
 * The verack message acknowledges a previously-received version message,
 * informing the connecting node that it can begin to send other messages.
 */
inline constexpr const char* VERACK{"verack"};
/**
 * The addr (IP address) message relays connection information for peers on the
 * network.
 */
inline constexpr const char* ADDR{"addr"};
/**
 * The addrv2 message relays connection information for peers on the network just
 * like the addr message, but is extended to allow gossiping of longer node
 * addresses (see BIP155).
 */
inline constexpr const char* ADDRV2{"addrv2"};
/**
 * The sendaddrv2 message signals support for receiving ADDRV2 messages (BIP155).
 * It also implies that its sender can encode as ADDRV2 and would send ADDRV2
 * instead of ADDR to a peer that has signaled ADDRV2 support by sending SENDADDRV2.
 */
inline constexpr const char* SENDADDRV2{"sendaddrv2"};
/**
 * The inv message (inventory message) transmits one or more inventories of
 * objects known to the transmitting peer.
 */
inline constexpr const char* INV{"inv"};
/**
 * The getdata message requests one or more data objects from another node.
 */
inline constexpr const char* GETDATA{"getdata"};
/**
 * The getblocks message requests an inv message that provides block header
 * hashes starting from a particular point in the block chain.
 */
inline constexpr const char* GETBLOCKS{"getblocks"};
/**
 * The getheaders message requests a headers message that provides block
 * headers starting from a particular point in the block chain.
 */
inline constexpr const char* GETHEADERS{"getheaders"};
/**
 * The tx message transmits a single transaction.
 */
inline constexpr const char* TX{"tx"};
/**
 * The headers message sends one or more block headers to a node which
 * previously requested certain headers with a getheaders message.
 */
inline constexpr const char* HEADERS{"headers"};
/**
 * The block message transmits a single serialized block.
 */
inline constexpr const char* BLOCK{"block"};
/**
 * The getaddr message requests an addr message from the receiving node,
 * preferably one with lots of IP addresses of other receiving nodes.
 */
inline constexpr const char* GETADDR{"getaddr"};
/**
 * The ping message is sent periodically to help confirm that the receiving
 * peer is still connected.
 */
inline constexpr const char* PING{"ping"};
/**
 * The pong message replies to a ping message, proving to the pinging node that
 * the ponging node is still alive.
 * Defined by BIP31.
 */
inline constexpr const char* PONG{"pong"};
/**
 * The notfound message is a reply to a getdata message which requested an
 * object the receiving node does not have available for relay.
 */
inline constexpr const char* NOTFOUND{"notfound"};
/**
 * Indicates that a node prefers to receive new block announcements via a
 * "headers" message rather than an "inv".
 * Defined by BIP130.
 */
inline constexpr const char* SENDHEADERS{"sendheaders"};
/**
 * Contains a 1-byte bool and 8-byte LE version number.
 * Indicates that a node is willing to provide blocks via "cmpctblock" messages.
 * May indicate that a node prefers to receive new block announcements via a
 * "cmpctblock" message rather than an "inv", depending on message contents.
 * Defined by BIP152.
 */
inline constexpr const char* SENDCMPCT{"sendcmpct"};
/**
 * Contains a CBlockHeaderAndShortTxIDs object - providing a header and
 * list of "short txids".
 * Defined by BIP152.
 */
inline constexpr const char* CMPCTBLOCK{"cmpctblock"};
/**
 * Contains a BlockTransactionsRequest
 * Peer should respond with "blocktxn" message.
 * Defined by BIP152.
 */
inline constexpr const char* GETBLOCKTXN{"getblocktxn"};
/**
 * Contains a BlockTransactions.
 * Sent in response to a "getblocktxn" message.
 * Defined by BIP152.
 */
inline constexpr const char* BLOCKTXN{"blocktxn"};
/**
 * getcfilters requests compact filters for a range of blocks.
 * Only available with service bit NODE_COMPACT_FILTERS as described by
 * BIP 157 & 158.
 */
inline constexpr const char* GETCFILTERS{"getcfilters"};
/**
 * cfilter is a response to a getcfilters request containing a single compact
 * filter.
 */
inline constexpr const char* CFILTER{"cfilter"};
/**
 * getcfheaders requests a compact filter header and the filter hashes for a
 * range of blocks, which can then be used to reconstruct the filter headers
 * for those blocks.
 * Only available with service bit NODE_COMPACT_FILTERS as described by
 * BIP 157 & 158.
 */
inline constexpr const char* GETCFHEADERS{"getcfheaders"};
/**
 * cfheaders is a response to a getcfheaders request containing a filter header
 * and a vector of filter hashes for each subsequent block in the requested range.
 */
inline constexpr const char* CFHEADERS{"cfheaders"};
/**
 * getcfcheckpt requests evenly spaced compact filter headers, enabling
 * parallelized download and validation of the headers between them.
 * Only available with service bit NODE_COMPACT_FILTERS as described by
 * BIP 157 & 158.
 */
inline constexpr const char* GETCFCHECKPT{"getcfcheckpt"};
/**
 * cfcheckpt is a response to a getcfcheckpt request containing a vector of
 * evenly spaced filter headers for blocks on the requested chain.
 */
inline constexpr const char* CFCHECKPT{"cfcheckpt"};
/**
 * Indicates that a node prefers to relay transactions via wtxid, rather than
 * txid.
 * Defined by BIP339.
 */
inline constexpr const char* WTXIDRELAY{"wtxidrelay"};
}; // namespace NetMsgType

/** All known message types (see above). Keep this in the same order as the list of messages above. */
inline const std::array ALL_NET_MESSAGE_TYPES{std::to_array<std::string>({
    NetMsgType::VERSION,
    NetMsgType::VERACK,
    NetMsgType::ADDR,
    NetMsgType::ADDRV2,
    NetMsgType::SENDADDRV2,
    NetMsgType::INV,
    NetMsgType::GETDATA,
    NetMsgType::GETBLOCKS,
    NetMsgType::GETHEADERS,
    NetMsgType::TX,
    NetMsgType::HEADERS,
    NetMsgType::BLOCK,
    NetMsgType::GETADDR,
    NetMsgType::PING,
    NetMsgType::PONG,
    NetMsgType::NOTFOUND,
    NetMsgType::SENDHEADERS,
    NetMsgType::SENDCMPCT,
    NetMsgType::CMPCTBLOCK,
    NetMsgType::GETBLOCKTXN,
    NetMsgType::BLOCKTXN,
    NetMsgType::GETCFILTERS,
    NetMsgType::CFILTER,
    NetMsgType::GETCFHEADERS,
    NetMsgType::CFHEADERS,
    NetMsgType::GETCFCHECKPT,
    NetMsgType::CFCHECKPT,
    NetMsgType::WTXIDRELAY,
})};

/** nServices flags */
enum ServiceFlags : uint64_t {
    // NOTE: When adding here, be sure to update serviceFlagToStr too
    // Nothing
    NODE_NONE = 0,
    // NODE_NETWORK means that the node is capable of serving the complete block chain. It is currently
    // set by all Quicksilver non pruned nodes, and is unset by SPV clients or other light clients.
    NODE_NETWORK = (1 << 0),
    // NODE_WITNESS indicates that a node can be asked for blocks and transactions including
    // witness data.
    NODE_WITNESS = (1 << 3),
    // NODE_COMPACT_FILTERS means the node will service basic block filter requests.
    // See BIP157 and BIP158 for details on how this is implemented.
    NODE_COMPACT_FILTERS = (1 << 6),
    // NODE_NETWORK_LIMITED means the same as NODE_NETWORK with the limitation of only
    // serving the last 288 (2 day) blocks
    // See BIP159 for details on how this is implemented.
    NODE_NETWORK_LIMITED = (1 << 10),

    // NODE_P2P_V2 means the node supports BIP324 transport
    NODE_P2P_V2 = (1 << 11),

    // Bits 24-31 are reserved for temporary experiments. Just pick a bit that
    // isn't getting used, or one not being used much. Remember that service
    // bits are just unauthenticated advertisements, so your code must be robust
    // against collisions and other cases where nodes may be advertising a service
    //  they do not actually support. Other service bits should be allocated.
};

/**
 * Convert service flags (a bitmask of NODE_*) to human readable strings.
 * It supports unknown service flags which will be returned as "UNKNOWN[...]".
 * @param[in] flags multiple NODE_* bitwise-OR-ed together
 */
std::vector<std::string> serviceFlagsToStr(uint64_t flags);

/**
 * State independent service flags.
 * If the return value is changed, contrib/seeds/makeseeds.py
 * should be updated appropriately to filter for nodes with
 * desired service flags (compatible with our new flags).
 */
constexpr ServiceFlags SeedsServiceFlags() { return ServiceFlags(NODE_NETWORK | NODE_WITNESS); }

/**
 * Checks if a peer with the given service flags may be capable of having a
 * robust address-storage DB.
 */
static inline bool MayHaveUsefulAddressDB(ServiceFlags services)
{
    return (services & NODE_NETWORK) || (services & NODE_NETWORK_LIMITED);
}

/** A CService with information about it as peer */
class CAddress : public CService
{
    static constexpr std::chrono::seconds TIME_INIT{100000000};

    /** CAddress disk records store a 32-bit version word:
     *  - The low bits (masked by DISK_VERSION_IGNORE_MASK) store DISK_VERSION_INIT and are ignored
     *    on deserialization.
     *  - The high bits (masked by ~DISK_VERSION_IGNORE_MASK) must be DISK_VERSION_ADDRV2. Any other
     *    value is rejected. Network serialization has no embedded version word; V1 vs V2 is chosen
     *    by the stream params.
     */
    static constexpr uint32_t DISK_VERSION_INIT{220000};
    static constexpr uint32_t DISK_VERSION_IGNORE_MASK{0b00000000'00000111'11111111'11111111};
    /** High-bit marker written in disk serialized addresses to indicate V2 encoding. */
    static constexpr uint32_t DISK_VERSION_ADDRV2{1 << 29};
    static_assert((DISK_VERSION_INIT & ~DISK_VERSION_IGNORE_MASK) == 0, "DISK_VERSION_INIT must be covered by DISK_VERSION_IGNORE_MASK");
    static_assert((DISK_VERSION_ADDRV2 & DISK_VERSION_IGNORE_MASK) == 0, "DISK_VERSION_ADDRV2 must not be covered by DISK_VERSION_IGNORE_MASK");

public:
    CAddress() : CService{} {};
    CAddress(CService ipIn, ServiceFlags nServicesIn) : CService{ipIn}, nServices{nServicesIn} {};
    CAddress(CService ipIn, ServiceFlags nServicesIn, NodeSeconds time) : CService{ipIn}, nTime{time}, nServices{nServicesIn} {};

    enum class Format {
        Disk,
        Network,
    };
    struct SerParams : CNetAddr::SerParams {
        const Format fmt;
        SER_PARAMS_OPFUNC
    };
    static constexpr SerParams V1_NETWORK{{CNetAddr::Encoding::V1}, Format::Network};
    static constexpr SerParams V2_NETWORK{{CNetAddr::Encoding::V2}, Format::Network};
    static constexpr SerParams V2_DISK{{CNetAddr::Encoding::V2}, Format::Disk};

    SERIALIZE_METHODS(CAddress, obj)
    {
        bool use_v2;
        auto& params = SER_PARAMS(SerParams);
        if (params.fmt == Format::Disk) {
            // Disk serialization is always V2. The stored version word carries DISK_VERSION_INIT in
            // the ignored low bits and DISK_VERSION_ADDRV2 in the high bits.
            uint32_t stored_format_version = DISK_VERSION_INIT | DISK_VERSION_ADDRV2;
            READWRITE(stored_format_version);
            stored_format_version &= ~DISK_VERSION_IGNORE_MASK;
            if (stored_format_version != DISK_VERSION_ADDRV2) {
                throw std::ios_base::failure("Unsupported CAddress disk format version");
            }
            use_v2 = true;
        } else {
            assert(params.fmt == Format::Network);
            // In the network serialization format, the encoding (v1 or v2) is determined directly by
            // the value of enc in the stream params, as no explicitly encoded version
            // exists in the stream.
            use_v2 = params.enc == Encoding::V2;
        }

        READWRITE(Using<LossyChronoFormatter<uint32_t>>(obj.nTime));
        // nServices is serialized as CompactSize in V2; as uint64_t in V1.
        if (use_v2) {
            uint64_t services_tmp;
            SER_WRITE(obj, services_tmp = obj.nServices);
            READWRITE(Using<CompactSizeFormatter<false>>(services_tmp));
            SER_READ(obj, obj.nServices = static_cast<ServiceFlags>(services_tmp));
        } else {
            READWRITE(Using<CustomUintFormatter<8>>(obj.nServices));
        }
        // Invoke V1/V2 serializer for CService parent object.
        const auto ser_params{use_v2 ? CNetAddr::V2 : CNetAddr::V1};
        READWRITE(ser_params(AsBase<CService>(obj)));
    }

    //! Always included in serialization. The behavior is unspecified if the value is not representable as uint32_t.
    NodeSeconds nTime{TIME_INIT};
    //! Serialized as uint64_t in V1, and as CompactSize in V2.
    ServiceFlags nServices{NODE_NONE};

    friend bool operator==(const CAddress& a, const CAddress& b)
    {
        return a.nTime == b.nTime &&
               a.nServices == b.nServices &&
               static_cast<const CService&>(a) == static_cast<const CService&>(b);
    }
};

/** getdata message type flags */
const uint32_t MSG_WITNESS_FLAG = 1 << 30;
const uint32_t MSG_TYPE_MASK = 0xffffffff >> 2;

/** getdata / inv message types.
 * These numbers are defined by the protocol. When adding a new value, be sure
 * to mention it in the respective BIP.
 */
enum GetDataMsg : uint32_t {
    UNDEFINED = 0,
    MSG_TX = 1,
    MSG_BLOCK = 2,
    MSG_WTX = 5,                                      //!< Defined in BIP 339
    // The following can only occur in getdata. Invs always use TX/WTX or BLOCK.
    MSG_CMPCT_BLOCK = 4,                              //!< Defined in BIP152
    MSG_WITNESS_BLOCK = MSG_BLOCK | MSG_WITNESS_FLAG, //!< Defined in BIP144
    MSG_WITNESS_TX = MSG_TX | MSG_WITNESS_FLAG,       //!< Defined in BIP144
};

/** inv message data */
class CInv
{
public:
    CInv();
    CInv(uint32_t typeIn, const uint256& hashIn);

    SERIALIZE_METHODS(CInv, obj) { READWRITE(obj.type, obj.hash); }

    friend bool operator<(const CInv& a, const CInv& b);

    std::string GetMessageType() const;
    std::string ToString() const;

    // Single-message helper methods
    bool IsMsgTx() const { return type == MSG_TX; }
    bool IsMsgBlk() const { return type == MSG_BLOCK; }
    bool IsMsgWtx() const { return type == MSG_WTX; }
    bool IsMsgCmpctBlk() const { return type == MSG_CMPCT_BLOCK; }
    bool IsMsgWitnessBlk() const { return type == MSG_WITNESS_BLOCK; }

    // Combined-message helper methods
    bool IsGenTxMsg() const
    {
        return type == MSG_TX || type == MSG_WTX || type == MSG_WITNESS_TX;
    }
    bool IsGenBlkMsg() const
    {
        return type == MSG_BLOCK || type == MSG_CMPCT_BLOCK || type == MSG_WITNESS_BLOCK;
    }

    uint32_t type;
    uint256 hash;
};

/** Convert a TX/WITNESS_TX/WTX CInv to a GenTxid. */
GenTxid ToGenTxid(const CInv& inv);

#endif // QUICKSILVER_PROTOCOL_H
