// Cuck(at)oo Cycle, a memory-hard proof-of-work
// Copyright (c) 2013-2020 John Tromp
// Host flow of lean.cu, driving lean.cl through OpenCL.
// Offered under the same two licences as lean.cu: see LICENSE.txt and README.md.
#include "lean_ocl.hpp"
#include "cuckatoo.h"
#include "graph.hpp"
#include "lean_cl.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

namespace qsgpu {
namespace {
constexpr word_t kMaxEdges{static_cast<word_t>(NEDGES >> 8)};
constexpr int kMaxSolutions{4};

std::string Failure(const char* call, cl::Int code)
{
    char out[96];
    std::snprintf(out, sizeof(out), "%s=%d", call, code);
    return out;
}
} // namespace

struct OclLean::Impl {
    Impl(const OpenClApi& api_in, const ListedDevice& device_in)
        : api(api_in), device(device_in), bits(NEDGES / 64), graph_data(kMaxEdges, kMaxEdges, kMaxSolutions, 8) {}
    const OpenClApi& api;
    ListedDevice device;
    cl::Context context{nullptr};
    cl::CommandQueue queue{nullptr};
    cl::Program program{nullptr};
    cl::Kernel count{nullptr};
    cl::Kernel kill{nullptr};
    cl::Mem alive{nullptr};
    cl::Mem nonleaf{nullptr};
    std::vector<u64> bits;
    graph<word_t> graph_data;
    size_t global{0};
    size_t local{0};
};

#define m_api m_impl->api
#define m_device m_impl->device
#define m_context m_impl->context
#define m_queue m_impl->queue
#define m_program m_impl->program
#define m_count m_impl->count
#define m_kill m_impl->kill
#define m_alive m_impl->alive
#define m_nonleaf m_impl->nonleaf
#define m_bits m_impl->bits
#define m_graph m_impl->graph_data
#define m_global m_impl->global
#define m_local m_impl->local

OclLean::OclLean(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

std::unique_ptr<OclLean> OclLean::Create(const OpenClApi& api, const ListedDevice& device, std::string& error)
{
    auto self = std::unique_ptr<OclLean>(new OclLean(std::make_unique<Impl>(api, device)));
    cl::Int rc{cl::kSuccess};
    self->m_context = api.CreateContext(nullptr, 1, &device.device, nullptr, nullptr, &rc);
    if (!self->m_context || rc != cl::kSuccess) { error = Failure("clCreateContext", rc); return nullptr; }
    self->m_queue = api.CreateCommandQueue(self->m_context, device.device, 0, &rc);
    if (!self->m_queue || rc != cl::kSuccess) { error = Failure("clCreateCommandQueue", rc); return nullptr; }
    const char* source = qsgpu_lean_cl;
    self->m_program = api.CreateProgramWithSource(self->m_context, 1, &source, nullptr, &rc);
    if (!self->m_program || rc != cl::kSuccess) { error = Failure("clCreateProgramWithSource", rc); return nullptr; }
    char options[64];
    std::snprintf(options, sizeof(options), "-DEDGEBITS=%d", EDGEBITS);
    rc = api.BuildProgram(self->m_program, 1, &device.device, options, nullptr, nullptr);
    if (rc != cl::kSuccess) {
        size_t size{0}; api.GetProgramBuildInfo(self->m_program, device.device, cl::kProgramBuildLog, 0, nullptr, &size);
        std::string log(size, '\0');
        if (size) api.GetProgramBuildInfo(self->m_program, device.device, cl::kProgramBuildLog, size, log.data(), nullptr);
        while (!log.empty() && log.back() == '\0') log.pop_back();
        std::fprintf(stderr, "qsgpusolve-opencl: kernel build failed: %s\n", log.c_str());
        error = Failure("clBuildProgram", rc); return nullptr;
    }
    self->m_count = api.CreateKernel(self->m_program, "count_node_deg", &rc);
    if (!self->m_count || rc != cl::kSuccess) { error = Failure("clCreateKernel(count_node_deg)", rc); return nullptr; }
    self->m_kill = api.CreateKernel(self->m_program, "kill_leaf_edges", &rc);
    if (!self->m_kill || rc != cl::kSuccess) { error = Failure("clCreateKernel(kill_leaf_edges)", rc); return nullptr; }
    constexpr size_t bytes{NEDGES / 8};
    self->m_alive = api.CreateBuffer(self->m_context, cl::kMemReadWrite, bytes, nullptr, &rc);
    if (!self->m_alive || rc != cl::kSuccess) { error = Failure("clCreateBuffer(alive)", rc); return nullptr; }
    self->m_nonleaf = api.CreateBuffer(self->m_context, cl::kMemReadWrite, bytes, nullptr, &rc);
    if (!self->m_nonleaf || rc != cl::kSuccess) { error = Failure("clCreateBuffer(nonleaf)", rc); return nullptr; }
    size_t count_limit{0}, kill_limit{0};
    rc = api.GetKernelWorkGroupInfo(self->m_count, device.device, cl::kKernelWorkGroupSize, sizeof(count_limit), &count_limit, nullptr);
    if (rc != cl::kSuccess) { error = Failure("clGetKernelWorkGroupInfo(count_node_deg)", rc); return nullptr; }
    rc = api.GetKernelWorkGroupInfo(self->m_kill, device.device, cl::kKernelWorkGroupSize, sizeof(kill_limit), &kill_limit, nullptr);
    if (rc != cl::kSuccess) { error = Failure("clGetKernelWorkGroupInfo(kill_leaf_edges)", rc); return nullptr; }
    self->m_local = std::min<size_t>(128, std::min(count_limit, kill_limit));
    if (self->m_local == 0) { error = "kernel work-group size is zero"; return nullptr; }
    self->m_global = 128 * self->m_local;
    const uint32_t zero{0};
    rc = api.EnqueueFillBuffer(self->m_queue, self->m_alive, &zero, sizeof(zero), 0, bytes, 0, nullptr, nullptr);
    if (rc != cl::kSuccess) { error = Failure("clEnqueueFillBuffer(startup)", rc); return nullptr; }
    rc = api.Finish(self->m_queue);
    if (rc != cl::kSuccess) { error = Failure("clFinish(startup)", rc); return nullptr; }
    return self;
}

OclLean::~OclLean()
{
    if (m_nonleaf) m_api.ReleaseMemObject(m_nonleaf);
    if (m_alive) m_api.ReleaseMemObject(m_alive);
    if (m_kill) m_api.ReleaseKernel(m_kill);
    if (m_count) m_api.ReleaseKernel(m_count);
    if (m_program) m_api.ReleaseProgram(m_program);
    if (m_queue) m_api.ReleaseCommandQueue(m_queue);
    if (m_context) m_api.ReleaseContext(m_context);
}

int OclLean::Solve(const char* header, uint32_t len, OclWord out[][kOclProofSize], int max_sols,
                   bool& fault, std::string& fault_text)
{
    fault = false; fault_text.clear();
    auto checked = [&](cl::Int rc, const char* call) {
        if (rc == cl::kSuccess) return true;
        fault = true; fault_text = Failure(call, rc); return false;
    };
    siphash_keys keys{};
    setheader(header, len, &keys);
    const cl::Ulong key_values[4]{keys.k0, keys.k1, keys.k2, keys.k3};
    for (cl::Uint i = 0; i < 4; ++i) {
        if (!checked(m_api.SetKernelArg(m_count, i, sizeof(key_values[i]), &key_values[i]), "clSetKernelArg(count,key)")) return 0;
        if (!checked(m_api.SetKernelArg(m_kill, i, sizeof(key_values[i]), &key_values[i]), "clSetKernelArg(kill,key)")) return 0;
    }
    if (!checked(m_api.SetKernelArg(m_count, 4, sizeof(m_alive), &m_alive), "clSetKernelArg(count,alive)") ||
        !checked(m_api.SetKernelArg(m_count, 5, sizeof(m_nonleaf), &m_nonleaf), "clSetKernelArg(count,nonleaf)") ||
        !checked(m_api.SetKernelArg(m_kill, 4, sizeof(m_alive), &m_alive), "clSetKernelArg(kill,alive)") ||
        !checked(m_api.SetKernelArg(m_kill, 5, sizeof(m_nonleaf), &m_nonleaf), "clSetKernelArg(kill,nonleaf)")) return 0;
    constexpr size_t bytes{NEDGES / 8}; const uint32_t zero{0};
    if (!checked(m_api.EnqueueFillBuffer(m_queue, m_alive, &zero, sizeof(zero), 0, bytes, 0, nullptr, nullptr), "clEnqueueFillBuffer(alive)")) return 0;
    for (cl::Uint round = 0; round < 128; ++round) {
        if (!checked(m_api.EnqueueFillBuffer(m_queue, m_nonleaf, &zero, sizeof(zero), 0, bytes, 0, nullptr, nullptr), "clEnqueueFillBuffer(nonleaf)")) return 0;
        const cl::Uint uorv{round & 1};
        if (!checked(m_api.SetKernelArg(m_count, 6, sizeof(uorv), &uorv), "clSetKernelArg(count,uorv)") ||
            !checked(m_api.SetKernelArg(m_kill, 6, sizeof(uorv), &uorv), "clSetKernelArg(kill,uorv)")) return 0;
        if (!checked(m_api.EnqueueNDRangeKernel(m_queue, m_count, 1, nullptr, &m_global, &m_local, 0, nullptr, nullptr), "clEnqueueNDRangeKernel(count_node_deg)") ||
            !checked(m_api.EnqueueNDRangeKernel(m_queue, m_kill, 1, nullptr, &m_global, &m_local, 0, nullptr, nullptr), "clEnqueueNDRangeKernel(kill_leaf_edges)")) return 0;
    }
    if (!checked(m_api.Finish(m_queue), "clFinish") ||
        !checked(m_api.EnqueueReadBuffer(m_queue, m_alive, cl::kTrue, 0, bytes, m_bits.data(), 0, nullptr, nullptr), "clEnqueueReadBuffer")) return 0;
    uint32_t nedges{0}; for (u64 bits : m_bits) nedges += std::popcount(~bits);
    if (nedges >= kMaxEdges) return 0;
    m_graph.reset();
    for (word_t block = 0; block < NEDGES; block += 64) {
        u64 alive = ~m_bits[block / 64];
        for (word_t nonce = block - 1; alive;) {
            const uint32_t ffs = static_cast<uint32_t>(std::countr_zero(alive)) + 1;
            nonce += ffs; alive = ffs < 64 ? alive >> ffs : 0;
            m_graph.add_compress_edge(sipnode(&keys, nonce, 0), sipnode(&keys, nonce, 1));
            if (ffs & 64) break;
        }
    }
    if (m_graph.abandoned) return 0;
    int written{0};
    for (u32 s = 0; s < m_graph.nsols && written < max_sols; ++s) {
        word_t recovered[PROOFSIZE]{}; u32 j{0}, nalive{0};
        for (word_t block = 0; block < NEDGES && j < PROOFSIZE; block += 64) {
            u64 alive = ~m_bits[block / 64];
            for (word_t nonce = block - 1; alive;) {
                const uint32_t ffs = static_cast<uint32_t>(std::countr_zero(alive)) + 1;
                nonce += ffs; alive = ffs < 64 ? alive >> ffs : 0;
                if (nalive++ == m_graph.sols[s][j]) recovered[j++] = nonce;
                if (j == PROOFSIZE || (ffs & 64)) break;
            }
        }
        if (j == PROOFSIZE && verify(recovered, &keys) == POW_OK) {
            std::memcpy(out[written++], recovered, sizeof(recovered));
        }
    }
    return written;
}
#undef m_api
#undef m_device
#undef m_context
#undef m_queue
#undef m_program
#undef m_count
#undef m_kill
#undef m_alive
#undef m_nonleaf
#undef m_bits
#undef m_graph
#undef m_global
#undef m_local
} // namespace qsgpu
