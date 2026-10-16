// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef QUICKSILVER_CRYPTO_CUCKATOO_GPU_LISTING_FORMAT_H
#define QUICKSILVER_CRYPTO_CUCKATOO_GPU_LISTING_FORMAT_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace qsgpu {
struct ListingFields {
    std::string backend;
    std::string id;
    std::string type;
    bool integrated{false};
    uint64_t mem{0};
    std::string status;
    std::string reason;
    std::string name;
};

inline std::string FormatPciId(uint32_t domain, uint32_t bus, uint32_t device, uint32_t function)
{
    char out[32];
    std::snprintf(out, sizeof(out), "pci:%04x:%02x:%02x.%x", domain, bus, device, function);
    return out;
}

inline std::string ReasonToken(std::string_view reason)
{
    std::string out;
    out.reserve(reason.size() < 200 ? reason.size() : 200);
    for (const unsigned char c : reason) {
        if (out.size() == 200) break;
        out.push_back(c <= ' ' ? '_' : static_cast<char>(c));
    }
    return out;
}

inline std::string FormatListingLine(const ListingFields& fields)
{
    char mem[32];
    std::snprintf(mem, sizeof(mem), "%llu", static_cast<unsigned long long>(fields.mem));
    std::string name{fields.name};
    for (char& c : name) if (c == '\r' || c == '\n') c = ' ';
    std::string out{"device backend="};
    out += fields.backend + " id=" + fields.id + " type=" + fields.type;
    out += fields.integrated ? " integrated=1 mem=" : " integrated=0 mem=";
    out += mem;
    out += " status=" + fields.status;
    if (fields.status != "ok") out += " reason=" + ReasonToken(fields.reason);
    out += " name=" + name;
    return out;
}
} // namespace qsgpu

#endif // QUICKSILVER_CRYPTO_CUCKATOO_GPU_LISTING_FORMAT_H
