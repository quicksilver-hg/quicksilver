// Cuck(at)oo Cycle, a memory-hard proof-of-work
// Copyright (c) 2013-2020 John Tromp
// Host flow of lean.cu, driving lean.cl through OpenCL.
// Offered under the same two licences as lean.cu: see LICENSE.txt and README.md.
#ifndef QUICKSILVER_CUCKATOO_VENDOR_LEAN_OCL_HPP
#define QUICKSILVER_CUCKATOO_VENDOR_LEAN_OCL_HPP

#include <crypto/cuckatoo/gpu/opencl_devices.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qsgpu {
#if EDGEBITS > 32
using OclWord = uint64_t;
#elif EDGEBITS > 16
using OclWord = uint32_t;
#else
using OclWord = uint16_t;
#endif
inline constexpr int kOclProofSize{42};
class OclLean {
public:
    static std::unique_ptr<OclLean> Create(const OpenClApi&, const ListedDevice&, std::string& error);
    ~OclLean();
    int Solve(const char* header, uint32_t len, OclWord out[][kOclProofSize], int max_sols,
              bool& fault, std::string& fault_text);
private:
    struct Impl;
    explicit OclLean(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace qsgpu
#endif
