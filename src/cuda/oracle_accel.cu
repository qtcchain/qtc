// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <cuda/oracle_accel.h>

#include <crypto/sha256.h>
#include <cuda/cuda_context.h>
#include <cuda_runtime.h>
#include <matmul/noise.h>
#include <span.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <mutex>
#include <string>
#include <vector>

namespace qtc::cuda {
namespace {

using Element = matmul::field::Element;
constexpr uint32_t MODULUS = matmul::field::MODULUS;
constexpr uint32_t ORACLE_THREADS = 256;
constexpr uint32_t ORACLE_SCAN_THREADS = 256;
// Oracle v2 (QTC O5): one SHA-256 of seed||LE32(block) yields eight 31-bit lanes.
constexpr uint32_t ORACLE_LANES = 8;

struct OracleSeedBytes {
    uint8_t data[32];
};

struct DeviceNonceSeedPreHashPassRecord {
    Element offset;
    uint8_t seed_a[32];
    uint8_t seed_b[32];
    uint8_t sigma[32];
};

struct DeviceNonceSeedPreHashMidstates {
    uint32_t seed[8];
    uint32_t header[8];
};

struct OracleProfileState {
    std::atomic<bool> pool_initialized{false};
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> allocation_events{0};
    std::atomic<uint64_t> reuse_events{0};
    std::mutex mutex;
    double last_encode_noise_us{0.0};
    double last_submit_wait_us{0.0};
    double last_gpu_generation_ms{0.0};
    std::string reason{"cuda_oracle_ready"};
};

struct OracleWorkspace {
    struct HostStageBuffer {
        Element* pinned{nullptr};
        size_t capacity{0};
        bool pinned_disabled{false};
        std::vector<Element> fallback;

        ~HostStageBuffer() { cudaFreeHost(pinned); }

        bool Ensure(size_t required, std::string& error)
        {
            if (required == 0) {
                fallback.clear();
                return true;
            }

            if (!pinned_disabled && pinned != nullptr && capacity >= required) {
                return true;
            }

            if (!pinned_disabled) {
                cudaFreeHost(pinned);
                pinned = nullptr;
                capacity = 0;

                Element* candidate{nullptr};
                const cudaError_t alloc_error = cudaMallocHost(&candidate, required * sizeof(Element));
                if (alloc_error == cudaSuccess) {
                    pinned = candidate;
                    capacity = required;
                    fallback.clear();
                    return true;
                }

                pinned_disabled = true;
                error = "cudaMallocHost failed:" + std::string(cudaGetErrorString(alloc_error)) +
                    "; falling back to pageable host memory";
            }

            try {
                fallback.resize(required);
            } catch (const std::bad_alloc&) {
                error = "host staging allocation failed";
                return false;
            }
            return true;
        }

        Element* data()
        {
            return pinned != nullptr ? pinned : fallback.data();
        }
    };

    int device_index{-1};
    cudaStream_t stream{nullptr};
    Element* out_e_l{nullptr};
    Element* out_e_r{nullptr};
    Element* out_f_l{nullptr};
    Element* out_f_r{nullptr};
    Element* out_fill{nullptr};
    Element* out_scan_flags{nullptr};
    Element* out_scan_pass_count{nullptr};
    DeviceNonceSeedPreHashPassRecord* out_scan_pass_records{nullptr};
    DeviceNonceSeedPreHashMidstates* out_scan_midstates{nullptr};
    OracleSeedBytes* batch_seed_el{nullptr};
    OracleSeedBytes* batch_seed_er{nullptr};
    OracleSeedBytes* batch_seed_fl{nullptr};
    OracleSeedBytes* batch_seed_fr{nullptr};
    Element** batch_out_e_l{nullptr};
    Element** batch_out_e_r{nullptr};
    Element** batch_out_f_l{nullptr};
    Element** batch_out_f_r{nullptr};
    size_t noise_capacity{0};
    size_t fill_capacity{0};
    size_t scan_flags_capacity{0};
    size_t scan_pass_count_capacity{0};
    size_t scan_pass_records_capacity{0};
    size_t scan_midstates_capacity{0};
    size_t batch_seed_el_capacity{0};
    size_t batch_seed_er_capacity{0};
    size_t batch_seed_fl_capacity{0};
    size_t batch_seed_fr_capacity{0};
    size_t batch_out_e_l_capacity{0};
    size_t batch_out_e_r_capacity{0};
    size_t batch_out_f_l_capacity{0};
    size_t batch_out_f_r_capacity{0};
    HostStageBuffer host_e_l;
    HostStageBuffer host_e_r;
    HostStageBuffer host_f_l;
    HostStageBuffer host_f_r;
    HostStageBuffer host_fill;
    HostStageBuffer host_scan_flags;

    void ReleaseScanBuffers()
    {
        cudaFree(out_scan_flags);
        cudaFree(out_scan_pass_count);
        cudaFree(out_scan_pass_records);
        cudaFree(out_scan_midstates);
        out_scan_flags = nullptr;
        out_scan_pass_count = nullptr;
        out_scan_pass_records = nullptr;
        out_scan_midstates = nullptr;
        scan_flags_capacity = 0;
        scan_pass_count_capacity = 0;
        scan_pass_records_capacity = 0;
        scan_midstates_capacity = 0;
    }

    void ReleaseOutputs()
    {
        ReleaseScanBuffers();
        cudaFree(batch_seed_el);
        cudaFree(batch_seed_er);
        cudaFree(batch_seed_fl);
        cudaFree(batch_seed_fr);
        cudaFree(batch_out_e_l);
        cudaFree(batch_out_e_r);
        cudaFree(batch_out_f_l);
        cudaFree(batch_out_f_r);
        cudaFree(out_fill);
        cudaFree(out_f_r);
        cudaFree(out_f_l);
        cudaFree(out_e_r);
        cudaFree(out_e_l);

        batch_seed_el = nullptr;
        batch_seed_er = nullptr;
        batch_seed_fl = nullptr;
        batch_seed_fr = nullptr;
        batch_out_e_l = nullptr;
        batch_out_e_r = nullptr;
        batch_out_f_l = nullptr;
        batch_out_f_r = nullptr;
        out_fill = nullptr;
        out_f_r = nullptr;
        out_f_l = nullptr;
        out_e_r = nullptr;
        out_e_l = nullptr;
        noise_capacity = 0;
        fill_capacity = 0;
        batch_seed_el_capacity = 0;
        batch_seed_er_capacity = 0;
        batch_seed_fl_capacity = 0;
        batch_seed_fr_capacity = 0;
        batch_out_e_l_capacity = 0;
        batch_out_e_r_capacity = 0;
        batch_out_f_l_capacity = 0;
        batch_out_f_r_capacity = 0;
    }

    void ReleaseStream()
    {
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
            stream = nullptr;
        }
    }

    ~OracleWorkspace()
    {
        if (device_index >= 0) {
            cudaSetDevice(device_index);
        }
        ReleaseStream();
        ReleaseOutputs();
    }
};

thread_local OracleWorkspace g_workspace;
OracleProfileState g_profile;

struct ScanScratchReleaseGuard {
    OracleWorkspace& workspace;

    ~ScanScratchReleaseGuard()
    {
        workspace.ReleaseScanBuffers();
    }
};

struct DeviceInputPoolSlot {
    MatMulGeneratedInputsDevice inputs;
    size_t storage_capacity_words{0};
    bool in_use{false};
};

struct DeviceInputPoolContext {
    std::mutex mutex;
    std::vector<std::unique_ptr<DeviceInputPoolSlot>> slots;
    uint32_t next_slot{0};
};

DeviceInputPoolContext& GetDeviceInputPoolContext()
{
    static DeviceInputPoolContext context;
    return context;
}

OracleSeedBytes ToInternalSeedBytes(const uint256& seed)
{
    OracleSeedBytes out{};
    std::memcpy(out.data, seed.data(), sizeof(out.data));
    return out;
}

uint256 FromInternalSeedBytes(const uint8_t bytes[32])
{
    return uint256{Span<const unsigned char>{bytes, 32}};
}

void ResetWorkspaceForDevice(OracleWorkspace& workspace, int device_index)
{
    if (workspace.device_index == device_index) {
        return;
    }

    if (workspace.device_index >= 0) {
        cudaSetDevice(workspace.device_index);
    }
    workspace.ReleaseStream();
    workspace.ReleaseOutputs();
    workspace.device_index = device_index;
}

bool EnsureWorkspaceStream(OracleWorkspace& workspace, std::string& error)
{
    if (workspace.stream != nullptr) {
        return true;
    }

    const cudaError_t stream_error = cudaStreamCreateWithFlags(&workspace.stream, cudaStreamNonBlocking);
    if (stream_error != cudaSuccess) {
        error = "cudaStreamCreateWithFlags failed:" + std::string(cudaGetErrorString(stream_error));
        workspace.stream = nullptr;
        return false;
    }
    return true;
}

bool EnsureDeviceBuffer(Element*& buffer, size_t& capacity, size_t required, std::string& error)
{
    if (capacity >= required && buffer != nullptr) {
        return true;
    }

    cudaFree(buffer);
    buffer = nullptr;
    capacity = 0;

    if (required == 0) {
        return true;
    }

    const cudaError_t alloc_error = cudaMalloc(&buffer, required * sizeof(Element));
    if (alloc_error != cudaSuccess) {
        error = "cudaMalloc failed:" + std::string(cudaGetErrorString(alloc_error));
        return false;
    }

    capacity = required;
    return true;
}

template <typename T>
bool EnsureTypedDeviceBuffer(T*& buffer, size_t& capacity, size_t required, std::string& error)
{
    if (capacity >= required && buffer != nullptr) {
        return true;
    }

    cudaFree(buffer);
    buffer = nullptr;
    capacity = 0;

    if (required == 0) {
        return true;
    }

    const cudaError_t alloc_error = cudaMalloc(reinterpret_cast<void**>(&buffer), required * sizeof(T));
    if (alloc_error != cudaSuccess) {
        error = "cudaMalloc failed:" + std::string(cudaGetErrorString(alloc_error));
        return false;
    }

    capacity = required;
    return true;
}

bool EnsureOutputBuffers(OracleWorkspace& workspace,
                         size_t noise_words,
                         std::string& error)
{
    const bool reused = workspace.out_e_l != nullptr &&
        workspace.out_e_r != nullptr &&
        workspace.out_f_l != nullptr &&
        workspace.out_f_r != nullptr &&
        workspace.noise_capacity >= noise_words;

    if (reused) {
        g_profile.pool_initialized.store(true, std::memory_order_relaxed);
        g_profile.reuse_events.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    cudaFree(workspace.out_e_l);
    cudaFree(workspace.out_e_r);
    cudaFree(workspace.out_f_l);
    cudaFree(workspace.out_f_r);
    workspace.out_e_l = nullptr;
    workspace.out_e_r = nullptr;
    workspace.out_f_l = nullptr;
    workspace.out_f_r = nullptr;
    workspace.noise_capacity = 0;

    if (noise_words != 0) {
        cudaError_t alloc_error = cudaMalloc(&workspace.out_e_l, noise_words * sizeof(Element));
        if (alloc_error == cudaSuccess) {
            alloc_error = cudaMalloc(&workspace.out_e_r, noise_words * sizeof(Element));
        }
        if (alloc_error == cudaSuccess) {
            alloc_error = cudaMalloc(&workspace.out_f_l, noise_words * sizeof(Element));
        }
        if (alloc_error == cudaSuccess) {
            alloc_error = cudaMalloc(&workspace.out_f_r, noise_words * sizeof(Element));
        }
        if (alloc_error != cudaSuccess) {
            cudaFree(workspace.out_e_l);
            cudaFree(workspace.out_e_r);
            cudaFree(workspace.out_f_l);
            cudaFree(workspace.out_f_r);
            workspace.out_e_l = nullptr;
            workspace.out_e_r = nullptr;
            workspace.out_f_l = nullptr;
            workspace.out_f_r = nullptr;
            error = "cudaMalloc failed:" + std::string(cudaGetErrorString(alloc_error));
            return false;
        }
        workspace.noise_capacity = noise_words;
    }

    g_profile.pool_initialized.store(true, std::memory_order_relaxed);
    g_profile.allocation_events.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ValidateInputGenerationRequest(const MatMulInputGenerationRequest& request,
                                    std::string& error,
                                    uint32_t& noise_words)
{
    if (request.n == 0 || request.b == 0 || request.r == 0) {
        error = "invalid dimensions for GPU input generation";
        return false;
    }
    if (request.r > request.n) {
        error = "noise rank exceeds matrix dimension";
        return false;
    }
    if ((request.n % request.b) != 0) {
        error = "matrix dimension must be divisible by transcript block size";
        return false;
    }

    const uint64_t noise_words64 = static_cast<uint64_t>(request.n) * request.r;
    if (noise_words64 > std::numeric_limits<uint32_t>::max()) {
        error = "input generation dimensions exceed supported bounds";
        return false;
    }

    noise_words = static_cast<uint32_t>(noise_words64);
    return true;
}

void UpdateProfile(double encode_noise_us,
                   double submit_wait_us,
                   const char* reason)
{
    {
        std::lock_guard<std::mutex> lock(g_profile.mutex);
        g_profile.last_encode_noise_us = encode_noise_us;
        g_profile.last_submit_wait_us = submit_wait_us;
        g_profile.last_gpu_generation_ms = submit_wait_us / 1000.0;
        g_profile.reason = reason;
    }
    g_profile.samples.fetch_add(1, std::memory_order_relaxed);
}

bool EnsureGeneratedInputsDeviceBuffers(DeviceInputPoolSlot& slot,
                                        int device_index,
                                        uint32_t n,
                                        uint32_t b,
                                        uint32_t r,
                                        uint32_t noise_words,
                                        std::string& error,
                                        bool& allocated)
{
    auto& inputs = slot.inputs;
    if (inputs.device_index != device_index && inputs.device_index >= 0) {
        cudaSetDevice(inputs.device_index);
        if (inputs.ready_event != nullptr) {
            cudaEventDestroy(reinterpret_cast<cudaEvent_t>(inputs.ready_event));
            inputs.ready_event = nullptr;
        }
        cudaFree(inputs.storage);
        inputs.storage = nullptr;
        inputs.noise_e_l = nullptr;
        inputs.noise_e_r = nullptr;
        inputs.noise_f_l = nullptr;
        inputs.noise_f_r = nullptr;
        slot.storage_capacity_words = 0;
    }

    if (inputs.device_index != device_index) {
        cudaSetDevice(device_index);
    }

    inputs.device_index = device_index;
    inputs.n = n;
    inputs.b = b;
    inputs.r = r;
    inputs.noise_words = noise_words;

    const size_t total_words = static_cast<size_t>(noise_words) * 4U;
    if (total_words == 0) {
        return true;
    }

    if (inputs.storage == nullptr || slot.storage_capacity_words < total_words) {
        cudaFree(inputs.storage);
        inputs.storage = nullptr;
        inputs.noise_e_l = nullptr;
        inputs.noise_e_r = nullptr;
        inputs.noise_f_l = nullptr;
        inputs.noise_f_r = nullptr;

        const cudaError_t alloc_error = cudaMalloc(&inputs.storage, total_words * sizeof(Element));
        if (alloc_error != cudaSuccess) {
            error = "cudaMalloc failed:" + std::string(cudaGetErrorString(alloc_error));
            return false;
        }
        allocated = true;
        slot.storage_capacity_words = total_words;
    }

    inputs.noise_e_l = inputs.storage;
    inputs.noise_e_r = inputs.noise_e_l + noise_words;
    inputs.noise_f_l = inputs.noise_e_r + noise_words;
    inputs.noise_f_r = inputs.noise_f_l + noise_words;
    return true;
}

bool EnsureGeneratedInputsReadyEvent(MatMulGeneratedInputsDevice& inputs, std::string& error)
{
    if (inputs.ready_event != nullptr) {
        return true;
    }

    cudaEvent_t event_handle{nullptr};
    const cudaError_t event_error = cudaEventCreateWithFlags(&event_handle, cudaEventDisableTiming);
    if (event_error != cudaSuccess) {
        error = "cudaEventCreateWithFlags failed:" + std::string(cudaGetErrorString(event_error));
        return false;
    }

    inputs.ready_event = reinterpret_cast<void*>(event_handle);
    return true;
}

std::shared_ptr<const MatMulGeneratedInputsDevice> AcquireGeneratedInputsDevice(int device_index,
                                                                                uint32_t n,
                                                                                uint32_t b,
                                                                                uint32_t r,
                                                                                uint32_t noise_words,
                                                                                std::string& error)
{
    auto& context = GetDeviceInputPoolContext();
    std::unique_lock<std::mutex> lock(context.mutex);

    DeviceInputPoolSlot* slot_ptr{nullptr};
    bool reused_existing_slot{false};
    for (size_t offset = 0; offset < context.slots.size(); ++offset) {
        const size_t slot_index = (context.next_slot + offset) % context.slots.size();
        auto& slot = context.slots[slot_index];
        if (slot->in_use) {
            continue;
        }
        slot->in_use = true;
        context.next_slot = static_cast<uint32_t>((slot_index + 1) % std::max<size_t>(context.slots.size(), 1));
        slot_ptr = slot.get();
        reused_existing_slot = true;
        break;
    }

    if (slot_ptr == nullptr) {
        auto slot = std::make_unique<DeviceInputPoolSlot>();
        slot->in_use = true;
        slot_ptr = slot.get();
        context.slots.push_back(std::move(slot));
        context.next_slot = static_cast<uint32_t>(context.slots.size() % std::max<size_t>(context.slots.size(), 1));
    }

    lock.unlock();

    bool allocated_buffers{false};
    if (!EnsureGeneratedInputsDeviceBuffers(
            *slot_ptr,
            device_index,
            n,
            b,
            r,
            noise_words,
            error,
            allocated_buffers)) {
        std::lock_guard<std::mutex> relock(context.mutex);
        slot_ptr->in_use = false;
        return {};
    }

    g_profile.pool_initialized.store(true, std::memory_order_relaxed);
    if (allocated_buffers || !reused_existing_slot) {
        g_profile.allocation_events.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_profile.reuse_events.fetch_add(1, std::memory_order_relaxed);
    }

    auto holder = std::shared_ptr<DeviceInputPoolSlot>(
        slot_ptr,
        [&context](DeviceInputPoolSlot* slot) {
            std::lock_guard<std::mutex> lock(context.mutex);
            slot->in_use = false;
        });
    return std::shared_ptr<const MatMulGeneratedInputsDevice>(holder, &slot_ptr->inputs);
}

__device__ inline uint32_t RotR(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

__device__ inline uint32_t ShaCh(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ ((~x) & z);
}

__device__ inline uint32_t ShaMaj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

__device__ inline uint32_t ShaBSig0(uint32_t x)
{
    return RotR(x, 2U) ^ RotR(x, 13U) ^ RotR(x, 22U);
}

__device__ inline uint32_t ShaBSig1(uint32_t x)
{
    return RotR(x, 6U) ^ RotR(x, 11U) ^ RotR(x, 25U);
}

__device__ inline uint32_t ShaSSig0(uint32_t x)
{
    return RotR(x, 7U) ^ RotR(x, 18U) ^ (x >> 3U);
}

__device__ inline uint32_t ShaSSig1(uint32_t x)
{
    return RotR(x, 17U) ^ RotR(x, 19U) ^ (x >> 10U);
}

__device__ __constant__ uint32_t SHA256_K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

__device__ inline void Sha256Init(uint32_t state[8])
{
    state[0] = 0x6a09e667U;
    state[1] = 0xbb67ae85U;
    state[2] = 0x3c6ef372U;
    state[3] = 0xa54ff53aU;
    state[4] = 0x510e527fU;
    state[5] = 0x9b05688cU;
    state[6] = 0x1f83d9abU;
    state[7] = 0x5be0cd19U;
}

__device__ inline void SetByte(uint32_t w[64], uint32_t offset, uint32_t byte)
{
    const uint32_t word_index = offset >> 2U;
    const uint32_t shift = (3U - (offset & 3U)) * 8U;
    w[word_index] |= (byte & 0xffU) << shift;
}

__device__ inline void SetBytes(uint32_t w[16], uint32_t offset, const uint8_t* data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        SetByte(w, offset + i, data[i]);
    }
}

__device__ inline void SetLE16(uint32_t w[16], uint32_t offset, uint16_t value)
{
    SetByte(w, offset, static_cast<uint8_t>(value & 0xffU));
    SetByte(w, offset + 1U, static_cast<uint8_t>((value >> 8U) & 0xffU));
}

__device__ inline void SetLE32(uint32_t w[16], uint32_t offset, uint32_t value)
{
    SetByte(w, offset, static_cast<uint8_t>(value & 0xffU));
    SetByte(w, offset + 1U, static_cast<uint8_t>((value >> 8U) & 0xffU));
    SetByte(w, offset + 2U, static_cast<uint8_t>((value >> 16U) & 0xffU));
    SetByte(w, offset + 3U, static_cast<uint8_t>((value >> 24U) & 0xffU));
}

__device__ inline void SetLE64(uint32_t w[16], uint32_t offset, uint64_t value)
{
    for (uint32_t i = 0; i < 8U; ++i) {
        SetByte(w, offset + i, static_cast<uint8_t>((value >> (i * 8U)) & 0xffU));
    }
}

__device__ inline uint32_t Bswap32(uint32_t x)
{
    return ((x & 0x000000ffU) << 24U) |
        ((x & 0x0000ff00U) << 8U) |
        ((x & 0x00ff0000U) >> 8U) |
        ((x & 0xff000000U) >> 24U);
}

// WINDOWED SHA-256 compression: 16-word sliding schedule instead of a 64-word
// array. Byte-identical output (validated 200k nonces, 0 mismatches) but halves
// the per-thread local-memory stack frame (448->224 B), ~2x faster scanner.
__device__ inline void Sha256Compress(uint32_t state[8], uint32_t w[16])
{
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    #pragma unroll
    for (uint32_t t = 0; t < 64; ++t) {
        uint32_t wt;
        if (t < 16) {
            wt = w[t];
        } else {
            wt = ShaSSig1(w[(t - 2) & 15U]) + w[(t - 7) & 15U] + ShaSSig0(w[(t - 15) & 15U]) + w[(t - 16) & 15U];
            w[t & 15U] = wt;
        }
        const uint32_t t1 = h + ShaBSig1(e) + ShaCh(e, f, g) + SHA256_K[t] + wt;
        const uint32_t t2 = ShaBSig0(a) + ShaMaj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

__device__ inline void Sha256StateToBytes(const uint32_t state[8], uint8_t out[32])
{
    for (uint32_t i = 0; i < 8; ++i) {
        out[i * 4U] = static_cast<uint8_t>((state[i] >> 24U) & 0xffU);
        out[i * 4U + 1U] = static_cast<uint8_t>((state[i] >> 16U) & 0xffU);
        out[i * 4U + 2U] = static_cast<uint8_t>((state[i] >> 8U) & 0xffU);
        out[i * 4U + 3U] = static_cast<uint8_t>(state[i] & 0xffU);
    }
}

// ---- Oracle v2 (QTC O5), consensus reference: matmul/field.cpp ----
// preimage = seed_canonical(32) || LE32(block) [|| LE32(retry) when retry > 0]
// candidate(lane) = ReadLE32(SHA256(preimage) + 4*lane) & MODULUS; rejected when
// == MODULUS and retried with the SAME lane (retry 1, 2, ...); after 256 retries
// the deterministic fallback SHA256(seed || LE32(block) || "oracle-fallback")
// supplies ReadLE32(lane bytes) % MODULUS. Element index i lives at
// block = i >> 3, lane = i & 7, so index 0 equals the v1 oracle.
//
// The canonical seed bytes (seed.data() reversed) occupy message words w[0..7]
// as big-endian SHA words; the lane value is ReadLE32 of state[lane]'s bytes,
// i.e. Bswap32(state[lane]).

__device__ inline void PackSeedWords(const OracleSeedBytes& seed, uint32_t seed_w[8])
{
    #pragma unroll
    for (uint32_t i = 0; i < 8; ++i) {
        seed_w[i] = 0U;
    }
    for (uint32_t i = 0; i < 32; ++i) {
        SetByte(seed_w, i, seed.data[31U - i]);
    }
}

__device__ inline void OracleBlockState(const uint32_t seed_w[8],
                                        uint32_t block,
                                        uint32_t retry,
                                        uint32_t state[8])
{
    uint32_t w[16];
    #pragma unroll
    for (uint32_t i = 0; i < 8; ++i) {
        w[i] = seed_w[i];
    }
    #pragma unroll
    for (uint32_t i = 8; i < 16; ++i) {
        w[i] = 0U;
    }
    SetLE32(w, 32U, block);
    uint32_t message_len = 36U;
    if (retry > 0) {
        SetLE32(w, 36U, retry);
        message_len = 40U;
    }
    SetByte(w, message_len, 0x80U);
    w[15] = message_len * 8U;

    Sha256Init(state);
    Sha256Compress(state, w);
}

__device__ inline Element OracleFallbackLane(const uint32_t seed_w[8], uint32_t block, uint32_t lane)
{
    uint32_t w[16];
    #pragma unroll
    for (uint32_t i = 0; i < 8; ++i) {
        w[i] = seed_w[i];
    }
    #pragma unroll
    for (uint32_t i = 8; i < 16; ++i) {
        w[i] = 0U;
    }
    SetLE32(w, 32U, block);
    constexpr uint8_t fallback_tag[15] = {
        'o', 'r', 'a', 'c', 'l', 'e', '-', 'f', 'a', 'l', 'l', 'b', 'a', 'c', 'k'
    };
    for (uint32_t i = 0; i < 15; ++i) {
        SetByte(w, 36U + i, fallback_tag[i]);
    }
    SetByte(w, 51U, 0x80U);
    w[15] = 51U * 8U;

    uint32_t state[8];
    Sha256Init(state);
    Sha256Compress(state, w);
    return Bswap32(state[lane]) % MODULUS;
}

__device__ inline Element OracleLaneWithRetries(const uint32_t seed_w[8],
                                                uint32_t block,
                                                uint32_t lane,
                                                uint32_t first_retry)
{
    for (uint32_t retry = first_retry; retry < 256; ++retry) {
        uint32_t state[8];
        OracleBlockState(seed_w, block, retry, state);
        const uint32_t candidate = Bswap32(state[lane]) & MODULUS;
        if (candidate < MODULUS) {
            return candidate;
        }
    }
    return OracleFallbackLane(seed_w, block, lane);
}

// All eight lanes of one oracle block: out[k] = from_oracle(seed, 8*block + k).
__device__ inline void FromOracleBlock(const uint32_t seed_w[8], uint32_t block, Element out[ORACLE_LANES])
{
    uint32_t state[8];
    OracleBlockState(seed_w, block, /*retry=*/0U, state);
    #pragma unroll
    for (uint32_t lane = 0; lane < ORACLE_LANES; ++lane) {
        const uint32_t candidate = Bswap32(state[lane]) & MODULUS;
        out[lane] = candidate < MODULUS
            ? candidate
            : OracleLaneWithRetries(seed_w, block, lane, /*first_retry=*/1U);
    }
}

__device__ inline Element FromOracleIndex(const uint32_t seed_w[8], uint32_t index)
{
    return OracleLaneWithRetries(seed_w, index >> 3U, index & 7U, /*first_retry=*/0U);
}

// One thread per 8-lane oracle block; writes lanes [8*block, 8*block+8) ∩ [0,count).
__device__ inline void WriteOracleBlock(const uint32_t seed_w[8], uint32_t block, uint32_t count, Element* out)
{
    Element lanes[ORACLE_LANES];
    FromOracleBlock(seed_w, block, lanes);
    const uint32_t base = block * ORACLE_LANES;
    if (base + ORACLE_LANES <= count) {
        #pragma unroll
        for (uint32_t lane = 0; lane < ORACLE_LANES; ++lane) {
            out[base + lane] = lanes[lane];
        }
    } else {
        for (uint32_t lane = 0; base + lane < count; ++lane) {
            out[base + lane] = lanes[lane];
        }
    }
}

__device__ inline void CopySha256State(const uint32_t in[8], uint32_t out[8])
{
    for (uint32_t i = 0; i < 8; ++i) {
        out[i] = in[i];
    }
}

__device__ inline void Sha256MidstateFromBlockWords(uint32_t w[16], uint32_t state[8])
{
    Sha256Init(state);
    Sha256Compress(state, w);
}

__device__ inline void BuildSeedV2Block0Midstate(const OracleSeedBytes& previous_block_hash,
                                                 const OracleSeedBytes& merkle_root,
                                                 uint32_t height,
                                                 uint32_t version,
                                                 uint32_t state[8])
{
    uint32_t w[16] = {};
    constexpr char TAG[] = "QTC_MATMUL_SEED_V2";
    SetByte(w, 0U, 18U);
    for (uint32_t i = 0; i < 18U; ++i) {
        SetByte(w, 1U + i, static_cast<uint8_t>(TAG[i]));
    }
    SetBytes(w, 19U, previous_block_hash.data, 32U);
    SetLE32(w, 51U, height);
    SetLE32(w, 55U, version);
    SetBytes(w, 59U, merkle_root.data, 5U);
    Sha256MidstateFromBlockWords(w, state);
}

__device__ inline void BuildSeedV3Block0Midstate(const OracleSeedBytes& previous_block_hash,
                                                 uint64_t parent_median_time_past,
                                                 uint32_t height,
                                                 uint32_t version,
                                                 uint32_t state[8])
{
    uint32_t w[16] = {};
    constexpr char TAG[] = "QTC_MATMUL_SEED_V3";
    SetByte(w, 0U, 18U);
    for (uint32_t i = 0; i < 18U; ++i) {
        SetByte(w, 1U + i, static_cast<uint8_t>(TAG[i]));
    }
    SetBytes(w, 19U, previous_block_hash.data, 32U);
    SetLE64(w, 51U, parent_median_time_past);
    SetLE32(w, 59U, height);
    SetByte(w, 63U, static_cast<uint8_t>(version & 0xffU));
    Sha256MidstateFromBlockWords(w, state);
}

__device__ inline void BuildHeaderBlock0Midstate(uint32_t version,
                                                 const OracleSeedBytes& previous_block_hash,
                                                 const OracleSeedBytes& merkle_root,
                                                 uint32_t state[8])
{
    uint32_t w[16] = {};
    SetLE32(w, 0U, version);
    SetBytes(w, 4U, previous_block_hash.data, 32U);
    SetBytes(w, 36U, merkle_root.data, 28U);
    Sha256MidstateFromBlockWords(w, state);
}

__device__ inline void DigestSeedV2FromMidstate(const uint32_t seed_midstate[8],
                                                const OracleSeedBytes& merkle_root,
                                                uint32_t time,
                                                uint32_t bits,
                                                uint64_t nonce,
                                                uint16_t matmul_dim,
                                                uint8_t which,
                                                uint8_t out[32])
{
    uint32_t state[8];
    CopySha256State(seed_midstate, state);

    uint32_t w[16] = {};
    SetBytes(w, 0U, merkle_root.data + 5U, 27U);
    SetLE32(w, 27U, time);
    SetLE32(w, 31U, bits);
    SetLE64(w, 35U, nonce);
    SetLE16(w, 43U, matmul_dim);
    SetByte(w, 45U, which);
    SetByte(w, 46U, 0x80U);
    w[15] = 110U * 8U;
    Sha256Compress(state, w);
    Sha256StateToBytes(state, out);
}

__device__ inline void DigestSeedV3FromMidstate(const uint32_t seed_midstate[8],
                                                const OracleSeedBytes& merkle_root,
                                                uint32_t version,
                                                uint32_t time,
                                                uint32_t bits,
                                                uint64_t nonce,
                                                uint16_t matmul_dim,
                                                uint8_t which,
                                                uint8_t out[32])
{
    uint32_t state[8];
    CopySha256State(seed_midstate, state);

    uint32_t w[16] = {};
    SetByte(w, 0U, static_cast<uint8_t>((version >> 8U) & 0xffU));
    SetByte(w, 1U, static_cast<uint8_t>((version >> 16U) & 0xffU));
    SetByte(w, 2U, static_cast<uint8_t>((version >> 24U) & 0xffU));
    SetBytes(w, 3U, merkle_root.data, 32U);
    SetLE32(w, 35U, time);
    SetLE32(w, 39U, bits);
    SetLE64(w, 43U, nonce);
    SetLE16(w, 51U, matmul_dim);
    SetByte(w, 53U, which);
    SetByte(w, 54U, 0x80U);
    w[15] = 118U * 8U;
    Sha256Compress(state, w);
    Sha256StateToBytes(state, out);
}

__device__ inline void Sha256DigestWords(const uint32_t digest_state[8], uint8_t out[32])
{
    uint32_t state[8];
    Sha256Init(state);

    uint32_t w[16] = {};
    for (uint32_t i = 0; i < 8U; ++i) {
        w[i] = digest_state[i];
    }
    w[8] = 0x80000000U;
    w[15] = 32U * 8U;
    Sha256Compress(state, w);
    Sha256StateToBytes(state, out);
}

__device__ inline void DigestHeaderSigmaFromMidstate(const uint32_t header_midstate[8],
                                                     const OracleSeedBytes& merkle_root,
                                                     uint32_t time,
                                                     uint32_t bits,
                                                     uint64_t nonce,
                                                     uint16_t matmul_dim,
                                                     const uint8_t seed_a[32],
                                                     const uint8_t seed_b[32],
                                                     uint8_t out[32])
{
    uint32_t state[8];
    CopySha256State(header_midstate, state);

    uint32_t w1[16] = {};
    SetBytes(w1, 0U, merkle_root.data + 28U, 4U);
    SetLE32(w1, 4U, time);
    SetLE32(w1, 8U, bits);
    SetLE64(w1, 12U, nonce);
    SetLE16(w1, 20U, matmul_dim);
    SetBytes(w1, 22U, seed_a, 32U);
    SetBytes(w1, 54U, seed_b, 10U);
    Sha256Compress(state, w1);

    uint32_t w2[16] = {};
    SetBytes(w2, 0U, seed_b + 10U, 22U);
    SetByte(w2, 22U, 0x80U);
    w2[15] = 150U * 8U;
    Sha256Compress(state, w2);
    Sha256DigestWords(state, out);
}

__device__ inline bool Uint256InternalBytesLessOrEqual(const uint8_t lhs[32], const OracleSeedBytes& rhs)
{
    for (int i = 31; i >= 0; --i) {
        if (lhs[i] < rhs.data[i]) return true;
        if (lhs[i] > rhs.data[i]) return false;
    }
    return true;
}

__device__ inline void ComputeNonceSeedPreHashMaterials(const uint32_t seed_midstate[8],
                                                        const uint32_t header_midstate[8],
                                                        const OracleSeedBytes& merkle_root,
                                                        uint32_t version,
                                                        uint32_t time,
                                                        uint32_t bits,
                                                        uint64_t nonce,
                                                        uint16_t matmul_dim,
                                                        uint32_t seed_version,
                                                        uint8_t seed_a[32],
                                                        uint8_t seed_b[32],
                                                        uint8_t sigma[32])
{
    if (seed_version == 3U) {
        DigestSeedV3FromMidstate(
            seed_midstate,
            merkle_root,
            version,
            time,
            bits,
            nonce,
            matmul_dim,
            0U,
            seed_a);
        DigestSeedV3FromMidstate(
            seed_midstate,
            merkle_root,
            version,
            time,
            bits,
            nonce,
            matmul_dim,
            1U,
            seed_b);
    } else {
        DigestSeedV2FromMidstate(
            seed_midstate,
            merkle_root,
            time,
            bits,
            nonce,
            matmul_dim,
            0U,
            seed_a);
        DigestSeedV2FromMidstate(
            seed_midstate,
            merkle_root,
            time,
            bits,
            nonce,
            matmul_dim,
            1U,
            seed_b);
    }
    DigestHeaderSigmaFromMidstate(
        header_midstate,
        merkle_root,
        time,
        bits,
        nonce,
        matmul_dim,
        seed_a,
        seed_b,
        sigma);
}

__global__ void BuildNonceSeedPreHashMidstatesKernel(OracleSeedBytes previous_block_hash,
                                                     OracleSeedBytes merkle_root,
                                                     uint32_t version,
                                                     uint32_t height,
                                                     uint32_t seed_version,
                                                     uint64_t parent_median_time_past,
                                                     DeviceNonceSeedPreHashMidstates* out_midstates)
{
    if (seed_version == 3U) {
        BuildSeedV3Block0Midstate(
            previous_block_hash,
            parent_median_time_past,
            height,
            version,
            out_midstates->seed);
    } else {
        BuildSeedV2Block0Midstate(
            previous_block_hash,
            merkle_root,
            height,
            version,
            out_midstates->seed);
    }
    BuildHeaderBlock0Midstate(
        version,
        previous_block_hash,
        merkle_root,
        out_midstates->header);
}

__global__ void ScanNonceSeedPreHashKernel(OracleSeedBytes merkle_root,
                                           OracleSeedBytes pre_hash_target,
                                           uint32_t version,
                                           uint32_t time,
                                           uint32_t bits,
                                           uint64_t start_nonce,
                                           uint16_t matmul_dim,
                                           uint32_t seed_version,
                                           const DeviceNonceSeedPreHashMidstates* midstates,
                                           Element* out_scan_values,
                                           Element* out_pass_count,
                                           bool compact_pass_offsets,
                                           uint32_t scan_count)
{
    // Block 0 of the seed and header messages is nonce-independent. A setup
    // kernel computes those midstates once per scan window; each CUDA block
    // copies them into shared memory before the bounds guard so every thread
    // reaches the barrier.
    __shared__ uint32_t seed_midstate[8];
    __shared__ uint32_t header_midstate[8];
    if (threadIdx.x == 0) {
        for (uint32_t i = 0; i < 8U; ++i) {
            seed_midstate[i] = midstates->seed[i];
            header_midstate[i] = midstates->header[i];
        }
    }
    __syncthreads();

    const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= scan_count) {
        return;
    }

    const uint64_t nonce = start_nonce + static_cast<uint64_t>(gid);
    uint8_t seed_a[32];
    uint8_t seed_b[32];
    uint8_t sigma[32];
    ComputeNonceSeedPreHashMaterials(
        seed_midstate,
        header_midstate,
        merkle_root,
        version,
        time,
        bits,
        nonce,
        matmul_dim,
        seed_version,
        seed_a,
        seed_b,
        sigma);
    const bool passed = Uint256InternalBytesLessOrEqual(sigma, pre_hash_target);
    if (compact_pass_offsets) {
        if (passed) {
            const Element slot = atomicAdd(out_pass_count, Element{1});
            out_scan_values[slot] = gid;
        }
    } else {
        out_scan_values[gid] = passed ? 1U : 0U;
    }
}

__global__ void HydrateNonceSeedPreHashPassRecordsKernel(OracleSeedBytes merkle_root,
                                                         uint32_t version,
                                                         uint32_t time,
                                                         uint32_t bits,
                                                         uint64_t start_nonce,
                                                         uint16_t matmul_dim,
                                                         uint32_t seed_version,
                                                         const DeviceNonceSeedPreHashMidstates* midstates,
                                                         const Element* pass_offsets,
                                                         uint32_t pass_count,
                                                         DeviceNonceSeedPreHashPassRecord* out_records)
{
    __shared__ uint32_t seed_midstate[8];
    __shared__ uint32_t header_midstate[8];
    if (threadIdx.x == 0) {
        for (uint32_t i = 0; i < 8U; ++i) {
            seed_midstate[i] = midstates->seed[i];
            header_midstate[i] = midstates->header[i];
        }
    }
    __syncthreads();

    const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= pass_count) {
        return;
    }

    const Element offset = pass_offsets[gid];
    DeviceNonceSeedPreHashPassRecord record{};
    record.offset = offset;
    ComputeNonceSeedPreHashMaterials(
        seed_midstate,
        header_midstate,
        merkle_root,
        version,
        time,
        bits,
        start_nonce + static_cast<uint64_t>(offset),
        matmul_dim,
        seed_version,
        record.seed_a,
        record.seed_b,
        record.sigma);
    out_records[gid] = record;
}

// Noise generation: one thread per 8-lane oracle block for all four noise
// factors of one request. count = n*r words per factor.
__global__ void GenerateOracleNoiseKernel(OracleSeedBytes seed_el,
                                          OracleSeedBytes seed_er,
                                          OracleSeedBytes seed_fl,
                                          OracleSeedBytes seed_fr,
                                          Element* out_e_l,
                                          Element* out_e_r,
                                          Element* out_f_l,
                                          Element* out_f_r,
                                          uint32_t count,
                                          uint32_t block_count)
{
    const uint32_t block = blockIdx.x * blockDim.x + threadIdx.x;
    if (block >= block_count) {
        return;
    }

    uint32_t seed_w[8];
    PackSeedWords(seed_el, seed_w);
    WriteOracleBlock(seed_w, block, count, out_e_l);
    PackSeedWords(seed_er, seed_w);
    WriteOracleBlock(seed_w, block, count, out_e_r);
    PackSeedWords(seed_fl, seed_w);
    WriteOracleBlock(seed_w, block, count, out_f_l);
    PackSeedWords(seed_fr, seed_w);
    WriteOracleBlock(seed_w, block, count, out_f_r);
}

__global__ void GenerateOracleNoiseBatchKernel(const OracleSeedBytes* seed_el,
                                               const OracleSeedBytes* seed_er,
                                               const OracleSeedBytes* seed_fl,
                                               const OracleSeedBytes* seed_fr,
                                               Element* const* out_e_l,
                                               Element* const* out_e_r,
                                               Element* const* out_f_l,
                                               Element* const* out_f_r,
                                               uint32_t count,
                                               uint32_t blocks_per_request,
                                               size_t total_block_count)
{
    const size_t gid = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (gid >= total_block_count) {
        return;
    }

    const uint32_t batch_index = static_cast<uint32_t>(gid / blocks_per_request);
    const uint32_t block = static_cast<uint32_t>(gid % blocks_per_request);
    uint32_t seed_w[8];
    PackSeedWords(seed_el[batch_index], seed_w);
    WriteOracleBlock(seed_w, block, count, out_e_l[batch_index]);
    PackSeedWords(seed_er[batch_index], seed_w);
    WriteOracleBlock(seed_w, block, count, out_e_r[batch_index]);
    PackSeedWords(seed_fl[batch_index], seed_w);
    WriteOracleBlock(seed_w, block, count, out_f_l[batch_index]);
    PackSeedWords(seed_fr[batch_index], seed_w);
    WriteOracleBlock(seed_w, block, count, out_f_r[batch_index]);
}

// Direct oracle fill for arbitrary index ranges (parity tooling): thread k
// produces out[k] = from_oracle(seed, start_index + k).
__global__ void FillFromOracleKernel(OracleSeedBytes seed,
                                     uint32_t start_index,
                                     uint32_t count,
                                     Element* out)
{
    const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= count) {
        return;
    }
    uint32_t seed_w[8];
    PackSeedWords(seed, seed_w);
    out[gid] = FromOracleIndex(seed_w, start_index + gid);
}

uint32_t OracleBlockCount(uint32_t count)
{
    return (count + ORACLE_LANES - 1U) / ORACLE_LANES;
}

} // namespace

MatMulGeneratedInputsDevice::~MatMulGeneratedInputsDevice()
{
    if (device_index >= 0) {
        cudaSetDevice(device_index);
    }
    if (ready_event != nullptr) {
        cudaEventDestroy(reinterpret_cast<cudaEvent_t>(ready_event));
    }
    cudaFree(storage);
}

MatMulInputGenerationProfile ProbeMatMulInputGenerationProfile()
{
    MatMulInputGenerationProfile profile;

    const auto runtime = ProbeCudaRuntime();
    if (!runtime.available) {
        profile.available = false;
        profile.pool_initialized = false;
        profile.library_source = "unavailable";
        profile.reason = runtime.reason;
        return profile;
    }

    profile.available = true;
    profile.pool_initialized = g_profile.pool_initialized.load(std::memory_order_relaxed);
    profile.samples = g_profile.samples.load(std::memory_order_relaxed);
    profile.allocation_events = g_profile.allocation_events.load(std::memory_order_relaxed);
    profile.reuse_events = g_profile.reuse_events.load(std::memory_order_relaxed);
    profile.library_source = "cuda_compiled";
    {
        std::lock_guard<std::mutex> lock(g_profile.mutex);
        profile.last_encode_noise_us = g_profile.last_encode_noise_us;
        profile.last_submit_wait_us = g_profile.last_submit_wait_us;
        profile.last_gpu_generation_ms = g_profile.last_gpu_generation_ms;
        profile.reason = g_profile.reason;
    }
    if (profile.reason.empty()) {
        profile.reason = profile.pool_initialized ? "cuda_oracle_ready" : "cuda_oracle_pool_uninitialized";
    }
    return profile;
}

MatMulInputGenerationResult GenerateMatMulInputsGPU(const MatMulInputGenerationRequest& request)
{
    MatMulInputGenerationResult result;
    const auto runtime = ProbeCudaRuntime();
    result.available = runtime.available;
    if (!runtime.available) {
        result.error = runtime.reason;
        return result;
    }

    uint32_t noise_words{0};
    if (!ValidateInputGenerationRequest(request, result.error, noise_words)) {
        return result;
    }
    const auto seed_el = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_EL, request.sigma));
    const auto seed_er = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_ER, request.sigma));
    const auto seed_fl = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FL, request.sigma));
    const auto seed_fr = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FR, request.sigma));

    auto& workspace = g_workspace;
    ResetWorkspaceForDevice(workspace, runtime.device_index);

    cudaError_t error = cudaSetDevice(runtime.device_index);
    if (error != cudaSuccess) {
        result.error = "cudaSetDevice failed:" + std::string(cudaGetErrorString(error));
        return result;
    }
    if (!EnsureWorkspaceStream(workspace, result.error)) {
        return result;
    }

    if (!EnsureOutputBuffers(workspace, noise_words, result.error)) {
        return result;
    }

    const uint32_t noise_block_count = OracleBlockCount(noise_words);
    const uint32_t noise_blocks = (noise_block_count + ORACLE_THREADS - 1) / ORACLE_THREADS;
    const auto encode_noise_start = std::chrono::steady_clock::now();
    GenerateOracleNoiseKernel<<<noise_blocks, ORACLE_THREADS, 0, workspace.stream>>>(
        seed_el,
        seed_er,
        seed_fl,
        seed_fr,
        workspace.out_e_l,
        workspace.out_e_r,
        workspace.out_f_l,
        workspace.out_f_r,
        noise_words,
        noise_block_count);
    double encode_noise_us = std::chrono::duration<double, std::micro>(
                                 std::chrono::steady_clock::now() - encode_noise_start)
                                 .count();

    error = cudaGetLastError();
    if (error != cudaSuccess) {
        result.error = "CUDA oracle noise kernel failed:" + std::string(cudaGetErrorString(error));
        return result;
    }

    const auto submit_wait_start = std::chrono::steady_clock::now();
    std::string staging_warning;
    if (!workspace.host_e_l.Ensure(noise_words, staging_warning) ||
        !workspace.host_e_r.Ensure(noise_words, staging_warning) ||
        !workspace.host_f_l.Ensure(noise_words, staging_warning) ||
        !workspace.host_f_r.Ensure(noise_words, staging_warning)) {
        result.error = staging_warning;
        return result;
    }
    error = cudaMemcpyAsync(workspace.host_e_l.data(), workspace.out_e_l, noise_words * sizeof(Element), cudaMemcpyDeviceToHost, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.host_e_r.data(), workspace.out_e_r, noise_words * sizeof(Element), cudaMemcpyDeviceToHost, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.host_f_l.data(), workspace.out_f_l, noise_words * sizeof(Element), cudaMemcpyDeviceToHost, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.host_f_r.data(), workspace.out_f_r, noise_words * sizeof(Element), cudaMemcpyDeviceToHost, workspace.stream);
    if (error == cudaSuccess) error = cudaStreamSynchronize(workspace.stream);
    const double submit_wait_us = std::chrono::duration<double, std::micro>(
                                      std::chrono::steady_clock::now() - submit_wait_start)
                                      .count();
    if (error != cudaSuccess) {
        result.error = "CUDA oracle stream completion failed:" + std::string(cudaGetErrorString(error));
        return result;
    }

    result.noise_e_l.assign(workspace.host_e_l.data(), workspace.host_e_l.data() + noise_words);
    result.noise_e_r.assign(workspace.host_e_r.data(), workspace.host_e_r.data() + noise_words);
    result.noise_f_l.assign(workspace.host_f_l.data(), workspace.host_f_l.data() + noise_words);
    result.noise_f_r.assign(workspace.host_f_r.data(), workspace.host_f_r.data() + noise_words);
    result.success = true;
    UpdateProfile(encode_noise_us, submit_wait_us, "cuda_noise4_oracle_v2");
    return result;
}

MatMulInputGenerationDeviceResult GenerateMatMulInputsGPUDevice(const MatMulInputGenerationRequest& request)
{
    MatMulInputGenerationDeviceResult result;
    const auto runtime = ProbeCudaRuntime();
    result.available = runtime.available;
    if (!runtime.available) {
        result.error = runtime.reason;
        return result;
    }

    uint32_t noise_words{0};
    if (!ValidateInputGenerationRequest(request, result.error, noise_words)) {
        return result;
    }

    const auto seed_el = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_EL, request.sigma));
    const auto seed_er = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_ER, request.sigma));
    const auto seed_fl = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FL, request.sigma));
    const auto seed_fr = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FR, request.sigma));

    auto& workspace = g_workspace;
    ResetWorkspaceForDevice(workspace, runtime.device_index);

    cudaError_t error = cudaSetDevice(runtime.device_index);
    if (error != cudaSuccess) {
        result.error = "cudaSetDevice failed:" + std::string(cudaGetErrorString(error));
        return result;
    }
    if (!EnsureWorkspaceStream(workspace, result.error)) {
        return result;
    }

    auto generated = AcquireGeneratedInputsDevice(
        runtime.device_index,
        request.n,
        request.b,
        request.r,
        noise_words,
        result.error);
    if (!generated) {
        return result;
    }

    const uint32_t noise_block_count = OracleBlockCount(noise_words);
    const uint32_t noise_blocks = (noise_block_count + ORACLE_THREADS - 1) / ORACLE_THREADS;
    const auto encode_noise_start = std::chrono::steady_clock::now();
    GenerateOracleNoiseKernel<<<noise_blocks, ORACLE_THREADS, 0, workspace.stream>>>(
        seed_el,
        seed_er,
        seed_fl,
        seed_fr,
        generated->noise_e_l,
        generated->noise_e_r,
        generated->noise_f_l,
        generated->noise_f_r,
        noise_words,
        noise_block_count);
    const double encode_noise_us = std::chrono::duration<double, std::micro>(
                                       std::chrono::steady_clock::now() - encode_noise_start)
                                       .count();

    error = cudaGetLastError();
    if (error != cudaSuccess) {
        result.error = "CUDA oracle noise kernel failed:" + std::string(cudaGetErrorString(error));
        return result;
    }

    auto* generated_inputs = const_cast<MatMulGeneratedInputsDevice*>(generated.get());
    if (!EnsureGeneratedInputsReadyEvent(*generated_inputs, result.error)) {
        return result;
    }

    const auto submit_wait_start = std::chrono::steady_clock::now();
    error = cudaEventRecord(
        reinterpret_cast<cudaEvent_t>(generated_inputs->ready_event),
        workspace.stream);
    const double submit_wait_us = std::chrono::duration<double, std::micro>(
                                      std::chrono::steady_clock::now() - submit_wait_start)
                                      .count();
    if (error != cudaSuccess) {
        result.error = "CUDA oracle ready-event record failed:" + std::string(cudaGetErrorString(error));
        return result;
    }

    result.success = true;
    result.inputs = std::move(generated);
    UpdateProfile(encode_noise_us, submit_wait_us, "cuda_noise4_oracle_v2_device");
    return result;
}

MatMulInputGenerationDeviceBatchResult GenerateMatMulInputsGPUDeviceBatch(
    const MatMulInputGenerationDeviceBatchRequest& request)
{
    MatMulInputGenerationDeviceBatchResult result;
    const auto runtime = ProbeCudaRuntime();
    result.available = runtime.available;
    if (!runtime.available) {
        result.error = runtime.reason;
        return result;
    }
    if (request.batch_size == 0) {
        result.success = true;
        return result;
    }
    if (request.sigmas == nullptr) {
        result.error = "CUDA batched input generation requires sigma array";
        return result;
    }

    uint32_t noise_words{0};
    if (!ValidateInputGenerationRequest(
            {
                .n = request.n,
                .b = request.b,
                .r = request.r,
                .sigma = request.sigmas[0],
            },
            result.error,
            noise_words)) {
        return result;
    }

    std::vector<OracleSeedBytes> seed_el(request.batch_size);
    std::vector<OracleSeedBytes> seed_er(request.batch_size);
    std::vector<OracleSeedBytes> seed_fl(request.batch_size);
    std::vector<OracleSeedBytes> seed_fr(request.batch_size);
    for (uint32_t i = 0; i < request.batch_size; ++i) {
        seed_el[i] = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_EL, request.sigmas[i]));
        seed_er[i] = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_ER, request.sigmas[i]));
        seed_fl[i] = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FL, request.sigmas[i]));
        seed_fr[i] = ToInternalSeedBytes(matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FR, request.sigmas[i]));
    }

    auto& workspace = g_workspace;
    ResetWorkspaceForDevice(workspace, runtime.device_index);

    cudaError_t error = cudaSetDevice(runtime.device_index);
    if (error != cudaSuccess) {
        result.error = "cudaSetDevice failed:" + std::string(cudaGetErrorString(error));
        return result;
    }
    if (!EnsureWorkspaceStream(workspace, result.error)) {
        return result;
    }

    std::vector<std::shared_ptr<const MatMulGeneratedInputsDevice>> generated_inputs;
    generated_inputs.reserve(request.batch_size);
    std::vector<Element*> out_e_l(request.batch_size);
    std::vector<Element*> out_e_r(request.batch_size);
    std::vector<Element*> out_f_l(request.batch_size);
    std::vector<Element*> out_f_r(request.batch_size);
    for (uint32_t i = 0; i < request.batch_size; ++i) {
        auto generated = AcquireGeneratedInputsDevice(
            runtime.device_index,
            request.n,
            request.b,
            request.r,
            noise_words,
            result.error);
        if (!generated) {
            return result;
        }
        out_e_l[i] = generated->noise_e_l;
        out_e_r[i] = generated->noise_e_r;
        out_f_l[i] = generated->noise_f_l;
        out_f_r[i] = generated->noise_f_r;
        generated_inputs.push_back(std::move(generated));
    }

    if (!EnsureTypedDeviceBuffer(workspace.batch_seed_el, workspace.batch_seed_el_capacity, request.batch_size, result.error) ||
        !EnsureTypedDeviceBuffer(workspace.batch_seed_er, workspace.batch_seed_er_capacity, request.batch_size, result.error) ||
        !EnsureTypedDeviceBuffer(workspace.batch_seed_fl, workspace.batch_seed_fl_capacity, request.batch_size, result.error) ||
        !EnsureTypedDeviceBuffer(workspace.batch_seed_fr, workspace.batch_seed_fr_capacity, request.batch_size, result.error) ||
        !EnsureTypedDeviceBuffer(workspace.batch_out_e_l, workspace.batch_out_e_l_capacity, request.batch_size, result.error) ||
        !EnsureTypedDeviceBuffer(workspace.batch_out_e_r, workspace.batch_out_e_r_capacity, request.batch_size, result.error) ||
        !EnsureTypedDeviceBuffer(workspace.batch_out_f_l, workspace.batch_out_f_l_capacity, request.batch_size, result.error) ||
        !EnsureTypedDeviceBuffer(workspace.batch_out_f_r, workspace.batch_out_f_r_capacity, request.batch_size, result.error)) {
        return result;
    }

    const auto h2d_start = std::chrono::steady_clock::now();
    error = cudaMemcpyAsync(workspace.batch_seed_el, seed_el.data(), request.batch_size * sizeof(OracleSeedBytes), cudaMemcpyHostToDevice, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.batch_seed_er, seed_er.data(), request.batch_size * sizeof(OracleSeedBytes), cudaMemcpyHostToDevice, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.batch_seed_fl, seed_fl.data(), request.batch_size * sizeof(OracleSeedBytes), cudaMemcpyHostToDevice, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.batch_seed_fr, seed_fr.data(), request.batch_size * sizeof(OracleSeedBytes), cudaMemcpyHostToDevice, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.batch_out_e_l, out_e_l.data(), request.batch_size * sizeof(Element*), cudaMemcpyHostToDevice, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.batch_out_e_r, out_e_r.data(), request.batch_size * sizeof(Element*), cudaMemcpyHostToDevice, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.batch_out_f_l, out_f_l.data(), request.batch_size * sizeof(Element*), cudaMemcpyHostToDevice, workspace.stream);
    if (error == cudaSuccess) error = cudaMemcpyAsync(workspace.batch_out_f_r, out_f_r.data(), request.batch_size * sizeof(Element*), cudaMemcpyHostToDevice, workspace.stream);
    const double h2d_us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - h2d_start)
                              .count();
    if (error != cudaSuccess) {
        result.error = "CUDA batched oracle request copy failed:" + std::string(cudaGetErrorString(error));
        return result;
    }

    const uint32_t blocks_per_request = OracleBlockCount(noise_words);
    const size_t total_block_count = static_cast<size_t>(request.batch_size) * blocks_per_request;
    const uint32_t noise_blocks = static_cast<uint32_t>((total_block_count + ORACLE_THREADS - 1) / ORACLE_THREADS);
    const auto encode_noise_start = std::chrono::steady_clock::now();
    GenerateOracleNoiseBatchKernel<<<noise_blocks, ORACLE_THREADS, 0, workspace.stream>>>(
        workspace.batch_seed_el,
        workspace.batch_seed_er,
        workspace.batch_seed_fl,
        workspace.batch_seed_fr,
        workspace.batch_out_e_l,
        workspace.batch_out_e_r,
        workspace.batch_out_f_l,
        workspace.batch_out_f_r,
        noise_words,
        blocks_per_request,
        total_block_count);
    const double encode_noise_us = std::chrono::duration<double, std::micro>(
                                       std::chrono::steady_clock::now() - encode_noise_start)
                                       .count();

    error = cudaGetLastError();
    if (error != cudaSuccess) {
        result.error = "CUDA batched oracle noise kernel failed:" + std::string(cudaGetErrorString(error));
        return result;
    }

    const auto submit_wait_start = std::chrono::steady_clock::now();
    for (const auto& generated : generated_inputs) {
        auto* generated_input = const_cast<MatMulGeneratedInputsDevice*>(generated.get());
        if (!EnsureGeneratedInputsReadyEvent(*generated_input, result.error)) {
            return result;
        }
        error = cudaEventRecord(
            reinterpret_cast<cudaEvent_t>(generated_input->ready_event),
            workspace.stream);
        if (error != cudaSuccess) {
            result.error = "CUDA batched oracle ready-event record failed:" + std::string(cudaGetErrorString(error));
            return result;
        }
    }
    if (const char* debug_sync = std::getenv("QTC_MATMUL_CUDA_DEBUG_SYNC_PREPARE"); debug_sync != nullptr && debug_sync[0] == '1') {
        // Diagnostics only: make the prepare step wall time include the noise kernel.
        error = cudaStreamSynchronize(workspace.stream);
        if (error != cudaSuccess) {
            result.error = "CUDA batched oracle debug sync failed:" + std::string(cudaGetErrorString(error));
            return result;
        }
    }
    const double submit_wait_us = std::chrono::duration<double, std::micro>(
                                      std::chrono::steady_clock::now() - submit_wait_start)
                                      .count();

    result.inputs = std::move(generated_inputs);
    result.success = true;
    UpdateProfile(encode_noise_us, h2d_us + submit_wait_us, "cuda_noise4_oracle_v2_device_batch");
    return result;
}

MatMulOracleFillResult FillFromOracleGPU(const uint256& seed, uint32_t start_index, uint32_t count)
{
    MatMulOracleFillResult result;
    const auto runtime = ProbeCudaRuntime();
    result.available = runtime.available;
    if (!runtime.available) {
        result.error = runtime.reason;
        return result;
    }
    if (count == 0) {
        result.success = true;
        return result;
    }
    if (static_cast<uint64_t>(start_index) + count > (static_cast<uint64_t>(1) << 32)) {
        result.error = "oracle index range exceeds 32-bit index space";
        return result;
    }

    auto& workspace = g_workspace;
    ResetWorkspaceForDevice(workspace, runtime.device_index);

    cudaError_t error = cudaSetDevice(runtime.device_index);
    if (error != cudaSuccess) {
        result.error = "cudaSetDevice failed:" + std::string(cudaGetErrorString(error));
        return result;
    }
    if (!EnsureWorkspaceStream(workspace, result.error)) {
        return result;
    }
    if (!EnsureDeviceBuffer(workspace.out_fill, workspace.fill_capacity, count, result.error)) {
        return result;
    }
    std::string staging_warning;
    if (!workspace.host_fill.Ensure(count, staging_warning)) {
        result.error = staging_warning;
        return result;
    }

    const uint32_t blocks = (count + ORACLE_THREADS - 1) / ORACLE_THREADS;
    FillFromOracleKernel<<<blocks, ORACLE_THREADS, 0, workspace.stream>>>(
        ToInternalSeedBytes(seed),
        start_index,
        count,
        workspace.out_fill);
    error = cudaGetLastError();
    if (error != cudaSuccess) {
        result.error = "CUDA oracle fill kernel failed:" + std::string(cudaGetErrorString(error));
        return result;
    }
    error = cudaMemcpyAsync(workspace.host_fill.data(),
                            workspace.out_fill,
                            count * sizeof(Element),
                            cudaMemcpyDeviceToHost,
                            workspace.stream);
    if (error == cudaSuccess) {
        error = cudaStreamSynchronize(workspace.stream);
    }
    if (error != cudaSuccess) {
        result.error = "CUDA oracle fill completion failed:" + std::string(cudaGetErrorString(error));
        return result;
    }
    result.values.assign(workspace.host_fill.data(), workspace.host_fill.data() + count);
    result.success = true;
    return result;
}

MatMulNonceSeedPreHashScanResult ScanMatMulNonceSeedPreHashGPU(
    const MatMulNonceSeedPreHashScanRequest& request)
{
    MatMulNonceSeedPreHashScanResult result;
    const auto runtime = ProbeCudaRuntime();
    result.available = runtime.available;
    if (!runtime.available) {
        result.error = runtime.reason;
        return result;
    }
    if (request.scan_count == 0) {
        result.success = true;
        result.scanned_count = 0;
        return result;
    }
    if (request.matmul_dim == 0) {
        result.error = "CUDA nonce-seed pre-hash scan requires non-zero matmul_dim";
        return result;
    }
    if (request.seed_version != 2U && request.seed_version != 3U) {
        result.error = "CUDA nonce-seed pre-hash scan requires seed_version 2 or 3";
        return result;
    }
    if (request.seed_version == 3U && request.parent_median_time_past < 0) {
        result.error = "CUDA seed-v3 nonce-seed pre-hash scan requires non-negative parent median time past";
        return result;
    }
    if (request.compact_pass_records && !request.compact_pass_offsets) {
        result.error = "CUDA nonce-seed pre-hash pass records require compact_pass_offsets";
        return result;
    }

    auto& workspace = g_workspace;
    ResetWorkspaceForDevice(workspace, runtime.device_index);

    cudaError_t error = cudaSetDevice(runtime.device_index);
    if (error != cudaSuccess) {
        result.error = "cudaSetDevice failed:" + std::string(cudaGetErrorString(error));
        return result;
    }
    if (!EnsureWorkspaceStream(workspace, result.error)) {
        return result;
    }
    ScanScratchReleaseGuard release_scan_scratch{workspace};

    if (!EnsureDeviceBuffer(
            workspace.out_scan_flags,
            workspace.scan_flags_capacity,
            request.scan_count,
            result.error)) {
        return result;
    }
    if (request.compact_pass_offsets &&
        !EnsureDeviceBuffer(
            workspace.out_scan_pass_count,
            workspace.scan_pass_count_capacity,
            1,
            result.error)) {
        return result;
    }
    if (!EnsureTypedDeviceBuffer(
            workspace.out_scan_midstates,
            workspace.scan_midstates_capacity,
            1,
            result.error)) {
        return result;
    }

    if (request.compact_pass_offsets) {
        error = cudaMemsetAsync(
            workspace.out_scan_pass_count,
            0,
            sizeof(Element),
            workspace.stream);
        if (error != cudaSuccess) {
            result.error = "CUDA nonce-seed pre-hash scan count reset failed:" +
                std::string(cudaGetErrorString(error));
            return result;
        }
    }

    BuildNonceSeedPreHashMidstatesKernel<<<1, 1, 0, workspace.stream>>>(
        ToInternalSeedBytes(request.previous_block_hash),
        ToInternalSeedBytes(request.merkle_root),
        static_cast<uint32_t>(request.version),
        request.block_height,
        request.seed_version,
        static_cast<uint64_t>(request.parent_median_time_past),
        workspace.out_scan_midstates);
    error = cudaGetLastError();
    if (error != cudaSuccess) {
        result.error = "CUDA nonce-seed pre-hash midstate setup kernel failed:" +
            std::string(cudaGetErrorString(error));
        return result;
    }

    const uint32_t scan_blocks = (request.scan_count + ORACLE_SCAN_THREADS - 1) / ORACLE_SCAN_THREADS;
    ScanNonceSeedPreHashKernel<<<scan_blocks, ORACLE_SCAN_THREADS, 0, workspace.stream>>>(
        ToInternalSeedBytes(request.merkle_root),
        ToInternalSeedBytes(request.pre_hash_target),
        static_cast<uint32_t>(request.version),
        request.time,
        request.bits,
        request.start_nonce,
        request.matmul_dim,
        request.seed_version,
        workspace.out_scan_midstates,
        workspace.out_scan_flags,
        workspace.out_scan_pass_count,
        request.compact_pass_offsets,
        request.scan_count);
    error = cudaGetLastError();
    if (error != cudaSuccess) {
        result.error = "CUDA nonce-seed pre-hash scan kernel failed:" + std::string(cudaGetErrorString(error));
        return result;
    }

    std::string staging_warning;
    if (request.compact_pass_offsets) {
        if (!workspace.host_scan_flags.Ensure(1, staging_warning)) {
            result.error = staging_warning;
            return result;
        }
        error = cudaMemcpyAsync(
            workspace.host_scan_flags.data(),
            workspace.out_scan_pass_count,
            sizeof(Element),
            cudaMemcpyDeviceToHost,
            workspace.stream);
        if (error == cudaSuccess) {
            error = cudaStreamSynchronize(workspace.stream);
        }
        if (error != cudaSuccess) {
            result.error = "CUDA nonce-seed pre-hash compact count copy failed:" +
                std::string(cudaGetErrorString(error));
            return result;
        }

        const uint32_t pass_count = workspace.host_scan_flags.data()[0];
        if (pass_count > request.scan_count) {
            result.error = "CUDA nonce-seed pre-hash compact count exceeded scan count";
            return result;
        }
        if (!workspace.host_scan_flags.Ensure(pass_count, staging_warning)) {
            result.error = staging_warning;
            return result;
        }
        std::vector<DeviceNonceSeedPreHashPassRecord> host_pass_records;
        if (request.compact_pass_records && pass_count > 0) {
            if (!EnsureTypedDeviceBuffer(
                    workspace.out_scan_pass_records,
                    workspace.scan_pass_records_capacity,
                    pass_count,
                    result.error)) {
                return result;
            }

            const uint32_t record_blocks = (pass_count + ORACLE_SCAN_THREADS - 1) / ORACLE_SCAN_THREADS;
            HydrateNonceSeedPreHashPassRecordsKernel<<<record_blocks, ORACLE_SCAN_THREADS, 0, workspace.stream>>>(
                ToInternalSeedBytes(request.merkle_root),
                static_cast<uint32_t>(request.version),
                request.time,
                request.bits,
                request.start_nonce,
                request.matmul_dim,
                request.seed_version,
                workspace.out_scan_midstates,
                workspace.out_scan_flags,
                pass_count,
                workspace.out_scan_pass_records);
            error = cudaGetLastError();
            if (error != cudaSuccess) {
                result.error = "CUDA nonce-seed pre-hash pass record kernel failed:" +
                    std::string(cudaGetErrorString(error));
                return result;
            }
            host_pass_records.resize(pass_count);
        }

        if (pass_count > 0) {
            error = cudaMemcpyAsync(
                workspace.host_scan_flags.data(),
                workspace.out_scan_flags,
                pass_count * sizeof(Element),
                cudaMemcpyDeviceToHost,
                workspace.stream);
            if (error == cudaSuccess && request.compact_pass_records) {
                error = cudaMemcpyAsync(
                    host_pass_records.data(),
                    workspace.out_scan_pass_records,
                    pass_count * sizeof(DeviceNonceSeedPreHashPassRecord),
                    cudaMemcpyDeviceToHost,
                    workspace.stream);
            }
            if (error == cudaSuccess) {
                error = cudaStreamSynchronize(workspace.stream);
            }
            if (error != cudaSuccess) {
                result.error = "CUDA nonce-seed pre-hash compact result copy failed:" +
                    std::string(cudaGetErrorString(error));
                return result;
            }
        }

        result.pass_offsets.resize(pass_count);
        const Element* offsets = workspace.host_scan_flags.data();
        for (uint32_t i = 0; i < pass_count; ++i) {
            result.pass_offsets[i] = offsets[i];
        }
        std::sort(result.pass_offsets.begin(), result.pass_offsets.end());
        if (request.compact_pass_records) {
            result.pass_records.reserve(host_pass_records.size());
            for (const auto& record : host_pass_records) {
                result.pass_records.push_back(MatMulNonceSeedPreHashPassRecord{
                    .offset = record.offset,
                    .seed_a = FromInternalSeedBytes(record.seed_a),
                    .seed_b = FromInternalSeedBytes(record.seed_b),
                    .sigma = FromInternalSeedBytes(record.sigma),
                });
            }
            std::sort(
                result.pass_records.begin(),
                result.pass_records.end(),
                [](const MatMulNonceSeedPreHashPassRecord& lhs,
                   const MatMulNonceSeedPreHashPassRecord& rhs) {
                    return lhs.offset < rhs.offset;
                });
        }
    } else {
        if (!workspace.host_scan_flags.Ensure(request.scan_count, staging_warning)) {
            result.error = staging_warning;
            return result;
        }
        error = cudaMemcpyAsync(
            workspace.host_scan_flags.data(),
            workspace.out_scan_flags,
            request.scan_count * sizeof(Element),
            cudaMemcpyDeviceToHost,
            workspace.stream);
        if (error == cudaSuccess) {
            error = cudaStreamSynchronize(workspace.stream);
        }
        if (error != cudaSuccess) {
            result.error = "CUDA nonce-seed pre-hash scan completion failed:" + std::string(cudaGetErrorString(error));
            return result;
        }

        result.pass_flags.resize(request.scan_count);
        const Element* flags = workspace.host_scan_flags.data();
        for (uint32_t i = 0; i < request.scan_count; ++i) {
            result.pass_flags[i] = flags[i] != 0 ? 1U : 0U;
        }
    }
    result.scanned_count = request.scan_count;
    result.success = true;
    return result;
}

} // namespace qtc::cuda
