// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/bench/registry.h>

#include <map>

namespace cuckatoo {
namespace bench {

namespace {
//! Function-local static: the generated translation units register from static
//! initialisers, so the map must be constructed on first use rather than
//! depending on translation-unit initialisation order.
std::map<uint8_t, SolverVTable>& Table()
{
    static std::map<uint8_t, SolverVTable> t;
    return t;
}
} // namespace

bool Register(uint8_t edgebits, const SolverVTable& vt)
{
    return Table().emplace(edgebits, vt).second;
}

const SolverVTable* Lookup(uint8_t edgebits)
{
    const auto it = Table().find(edgebits);
    return it == Table().end() ? nullptr : &it->second;
}

std::vector<uint8_t> Sizes()
{
    std::vector<uint8_t> out;
    out.reserve(Table().size());
    for (const auto& entry : Table()) out.push_back(entry.first);
    return out;  // std::map iterates ascending
}

} // namespace bench
} // namespace cuckatoo
