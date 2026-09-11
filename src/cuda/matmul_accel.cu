// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <cuda/matmul_accel.h>

#include <cuda/cuda_context.h>
#include <cuda/cuda_scheduler.h>
#include <cuda/oracle_accel.h>

#include <cuda_runtime.h>
#include <span.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Product-committed digest v4 pipeline (QTC O5), per request in a batch:
//   1. A' = A + E_L*E_R and B' = B + F_L*F_R over GF(2^31-1). The base A/B
//      come either from the host (cached on the device by seed key) or are
//      regenerated on the device from the header seeds with oracle v2 (one
//      SHA-256 per eight consecutive elements, per-lane retry chain).
//   2. C' = A'B' in full via a shared-memory tiled GEMM (64x64x16 tiles).
//   3. Every b x b tile of C' is hashed by one thread with a single SHA-256
//      over its b*b LE32 elements in row-major order -> 32-byte tile hash.
// The (n/b)^2 tile hashes per request go back to the host, which finishes the
// digest with matmul::transcript::ComputeProductCommittedDigestFromTileHashes.
// There is no linear compression anywhere in this file (the v3 compress vector
// was the H4 shortcut); every C' element is materialised and hashed.

namespace qtc::cuda {
namespace {

using Element = matmul::field::Element;
constexpr Element MODULUS = matmul::field::MODULUS;
constexpr uint32_t REDUCE_INTERVAL{4};
constexpr uint32_t WORKSPACE_THREADS{256};
constexpr uint32_t GEMM_BM{64};
constexpr uint32_t GEMM_BN{64};
constexpr uint32_t GEMM_BK{16};
constexpr uint32_t GEMM_THREADS{256};
constexpr uint32_t TILE_HASH_THREADS{128};
constexpr uint32_t ORACLE_LANES{8};
constexpr uint32_t MAX_SUPPORTED_BLOCK_SIZE{16};

std::string CudaErrorString(cudaError_t error);

struct __align__(16) DeviceSeedBytes {
    uint8_t data[32];
};

static_assert(uint256::size() == sizeof(DeviceSeedBytes));
static_assert(sizeof(uint256) == sizeof(DeviceSeedBytes));
static_assert(std::is_trivially_copyable_v<uint256>);

struct __align__(16) DeviceDigestBytes {
    uint8_t data[32];
};

template <typename T>
struct HostStageBuffer {
    T* pinned{nullptr};
    size_t capacity{0};
    bool pinned_disabled{false};
    std::vector<T> fallback;

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

            T* candidate{nullptr};
            const cudaError_t alloc_error = cudaMallocHost(reinterpret_cast<void**>(&candidate), required * sizeof(T));
            if (alloc_error == cudaSuccess) {
                pinned = candidate;
                capacity = required;
                fallback.clear();
                return true;
            }

            pinned_disabled = true;
            error = "cudaMallocHost failed:" + CudaErrorString(alloc_error) + "; falling back to pageable host memory";
        }

        try {
            fallback.resize(required);
        } catch (const std::bad_alloc&) {
            error = "host staging allocation failed";
            return false;
        }
        return true;
    }

    T* data()
    {
        return pinned != nullptr ? pinned : fallback.data();
    }
};

struct DigestWorkspace {
    int device_index{-1};
    cudaStream_t stream{nullptr};

    uint256 cached_matrix_a_key;
    uint256 cached_matrix_b_key;
    uint32_t cached_matrix_elements{0};
    bool cached_matrix_keys_valid{false};

    // Base A/B (host supplied, cached by seed key).
    Element* device_base_a{nullptr};
    Element* device_base_b{nullptr};
    size_t base_a_capacity{0};
    size_t base_b_capacity{0};

    // Perturbed A'/B' per request.
    Element* device_matrix_a{nullptr};
    Element* device_matrix_b{nullptr};
    size_t matrix_a_capacity{0};
    size_t matrix_b_capacity{0};

    DeviceSeedBytes* device_seed_a{nullptr};
    DeviceSeedBytes* device_seed_b{nullptr};
    DeviceSeedBytes* device_sigma{nullptr};
    size_t seed_a_capacity{0};
    size_t seed_b_capacity{0};
    size_t sigma_capacity{0};

    // Host-supplied noise packed per request as [E_L | E_R | F_L | F_R].
    Element* device_packed_noise{nullptr};
    size_t packed_noise_capacity{0};
    const Element** device_prepared_input_ptrs{nullptr};
    size_t prepared_input_ptr_capacity{0};

    // Full C' per request (n^2 words) and its (n/b)^2 tile hashes.
    Element* device_output{nullptr};
    size_t output_capacity{0};
    DeviceDigestBytes* device_tile_hashes{nullptr};
    size_t tile_hash_capacity{0};
    DeviceDigestBytes* device_digests{nullptr};
    size_t digest_capacity{0};

    // Per-seed SHA midstates for matrix generation: 16 words/seed (8 packed seed
    // words + 8 post-round-7 state words). Tiny (batch_size * 64 B).
    uint32_t* device_seed_midstates{nullptr};
    size_t seed_midstates_capacity{0};

    // Stage timing events (build -> gemm -> tile hash), read after the stream sync.
    cudaEvent_t event_build_start{nullptr};
    cudaEvent_t event_gemm_start{nullptr};
    cudaEvent_t event_gemm_end{nullptr};
    cudaEvent_t event_hash_end{nullptr};
    cudaEvent_t event_copy_end{nullptr};
    bool build_event_recorded{false};

    HostStageBuffer<Element> host_matrix_a;
    HostStageBuffer<Element> host_matrix_b;
    HostStageBuffer<Element> host_packed_noise;
    HostStageBuffer<const Element*> host_pointer_table;
    HostStageBuffer<DeviceSeedBytes> host_seeds;
    HostStageBuffer<DeviceSeedBytes> host_sigma;
    HostStageBuffer<DeviceDigestBytes> host_tile_hashes;
    HostStageBuffer<DeviceDigestBytes> host_digests;

    void ReleaseDeviceBuffers()
    {
        cudaFree(device_seed_midstates);
        cudaFree(device_digests);
        cudaFree(device_tile_hashes);
        cudaFree(device_output);
        cudaFree(device_prepared_input_ptrs);
        cudaFree(device_packed_noise);
        cudaFree(device_sigma);
        cudaFree(device_seed_b);
        cudaFree(device_seed_a);
        cudaFree(device_matrix_b);
        cudaFree(device_matrix_a);
        cudaFree(device_base_b);
        cudaFree(device_base_a);

        device_seed_midstates = nullptr;
        device_digests = nullptr;
        device_tile_hashes = nullptr;
        device_output = nullptr;
        device_prepared_input_ptrs = nullptr;
        device_packed_noise = nullptr;
        device_sigma = nullptr;
        device_seed_b = nullptr;
        device_seed_a = nullptr;
        device_matrix_b = nullptr;
        device_matrix_a = nullptr;
        device_base_b = nullptr;
        device_base_a = nullptr;

        seed_midstates_capacity = 0;
        digest_capacity = 0;
        tile_hash_capacity = 0;
        output_capacity = 0;
        prepared_input_ptr_capacity = 0;
        packed_noise_capacity = 0;
        sigma_capacity = 0;
        seed_b_capacity = 0;
        seed_a_capacity = 0;
        matrix_b_capacity = 0;
        matrix_a_capacity = 0;
        base_b_capacity = 0;
        base_a_capacity = 0;

        cached_matrix_a_key.SetNull();
        cached_matrix_b_key.SetNull();
        cached_matrix_elements = 0;
        cached_matrix_keys_valid = false;
    }

    void ReleaseStream()
    {
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
            stream = nullptr;
        }
        for (cudaEvent_t* event : {&event_build_start, &event_gemm_start, &event_gemm_end, &event_hash_end, &event_copy_end}) {
            if (*event != nullptr) {
                cudaEventDestroy(*event);
                *event = nullptr;
            }
        }
    }

    bool EnsureTimingEvents()
    {
        for (cudaEvent_t* event : {&event_build_start, &event_gemm_start, &event_gemm_end, &event_hash_end, &event_copy_end}) {
            if (*event == nullptr && cudaEventCreate(event) != cudaSuccess) {
                *event = nullptr;
                return false;
            }
        }
        return true;
    }

    void MarkBuildStart()
    {
        build_event_recorded = EnsureTimingEvents() &&
            cudaEventRecord(event_build_start, stream) == cudaSuccess;
    }

    ~DigestWorkspace()
    {
        if (device_index >= 0) {
            cudaSetDevice(device_index);
        }
        ReleaseStream();
        ReleaseDeviceBuffers();
    }
};

struct DigestPoolSlot {
    DigestWorkspace workspace;
    bool in_use{false};
};

struct DigestPoolContext {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::unique_ptr<DigestPoolSlot>> slots;
    uint32_t next_slot{0};
    uint32_t active_slots{0};
    uint32_t high_water_slots{0};
    uint64_t allocation_events{0};
    uint64_t reuse_events{0};
    uint64_t wait_events{0};
    uint64_t completed_submissions{0};
    uint32_t inflight_submissions{0};
    uint32_t peak_inflight_submissions{0};
    uint32_t last_n{0};
    uint32_t last_b{0};
    uint32_t last_r{0};
    bool initialized{false};
    std::string reason{"buffer_pool_uninitialized"};
};

struct DigestProfilingSample {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    uint32_t batch_size{0};
    double host_stage_us{0.0};
    double submit_h2d_us{0.0};
    double submit_d2d_us{0.0};
    double stream_wait_event_us{0.0};
    double launch_build_perturbed_us{0.0};
    double launch_finalize_us{0.0};
    double submit_d2h_us{0.0};
    double stream_sync_us{0.0};
    double total_wall_ms{0.0};
    bool used_low_rank_path{false};
    bool used_device_prepared_inputs{false};
    bool used_pinned_host_staging{true};
    bool base_matrix_cache_hit{false};
    double gpu_build_us{0.0};
    double gpu_gemm_us{0.0};
    double gpu_tile_hash_us{0.0};
    double gpu_copy_us{0.0};
    std::string mode;
};

struct DigestProfilingContext {
    std::mutex mutex;
    uint64_t samples{0};
    uint32_t last_n{0};
    uint32_t last_b{0};
    uint32_t last_r{0};
    uint32_t last_batch_size{0};
    double last_host_stage_us{0.0};
    double last_submit_h2d_us{0.0};
    double last_submit_d2d_us{0.0};
    double last_stream_wait_event_us{0.0};
    double last_launch_build_perturbed_us{0.0};
    double last_launch_finalize_us{0.0};
    double last_submit_d2h_us{0.0};
    double last_stream_sync_us{0.0};
    double last_total_wall_ms{0.0};
    bool last_used_low_rank_path{false};
    bool last_used_device_prepared_inputs{false};
    bool last_used_pinned_host_staging{false};
    bool last_base_matrix_cache_hit{false};
    double last_gpu_build_us{0.0};
    double last_gpu_gemm_us{0.0};
    double last_gpu_tile_hash_us{0.0};
    double last_gpu_copy_us{0.0};
    std::string last_mode;
    std::string reason{"no_samples"};
};

CudaRuntimeProbe RuntimeProbeFromDeviceInfo(const CudaTopologyProbe& topology, const CudaDeviceInfo& device)
{
    CudaRuntimeProbe probe;
    probe.compiled = topology.compiled;
    probe.available = topology.available && device.supported;
    probe.reason = probe.available ? "ready" : device.reason;
    probe.device_index = device.device_index;
    probe.device_name = device.device_name;
    probe.compute_capability_major = device.compute_capability_major;
    probe.compute_capability_minor = device.compute_capability_minor;
    probe.global_memory_bytes = device.global_memory_bytes;
    probe.multiprocessor_count = device.multiprocessor_count;
    probe.driver_api_version = topology.driver_api_version;
    probe.runtime_version = topology.runtime_version;
    return probe;
}

std::optional<CudaRuntimeProbe> ResolveCudaRuntimeForSelectedDevice(int device_index, std::string& error)
{
    const auto topology = ProbeCudaTopology();
    if (!topology.available) {
        error = topology.reason;
        return std::nullopt;
    }

    for (const auto& device : topology.selected_devices) {
        if (device.device_index == device_index) {
            return RuntimeProbeFromDeviceInfo(topology, device);
        }
    }

    error = "selected_cuda_device_not_enabled:" + std::to_string(device_index);
    return std::nullopt;
}

bool ParsePoolSlotCount(const std::string& value, uint32_t& slots)
{
    constexpr uint32_t MAX_SLOTS{32};
    try {
        size_t consumed{0};
        const unsigned long parsed = std::stoul(value, &consumed, 10);
        if (consumed != value.size() || parsed == 0) {
            return false;
        }
        slots = std::min<uint32_t>(static_cast<uint32_t>(parsed), MAX_SLOTS);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<uint32_t> ResolveCudaPoolSlotOverride(int device_index)
{
    const char* env = std::getenv("QTC_MATMUL_CUDA_POOL_SLOTS");
    if (env == nullptr || *env == '\0') {
        return std::nullopt;
    }

    const std::string value{env};
    if (value.find(':') == std::string::npos) {
        uint32_t slots{0};
        return ParsePoolSlotCount(value, slots) ? std::optional<uint32_t>{slots} : std::nullopt;
    }

    size_t begin{0};
    while (begin <= value.size()) {
        const size_t comma = value.find(',', begin);
        const std::string token = value.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        const size_t colon = token.find(':');
        if (colon == std::string::npos) {
            return std::nullopt;
        }

        int parsed_device{-1};
        uint32_t parsed_slots{0};
        try {
            size_t consumed{0};
            parsed_device = std::stoi(token.substr(0, colon), &consumed, 10);
            if (consumed != token.substr(0, colon).size() || parsed_device < 0) {
                return std::nullopt;
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (!ParsePoolSlotCount(token.substr(colon + 1), parsed_slots)) {
            return std::nullopt;
        }
        if (parsed_device == device_index) {
            return parsed_slots;
        }

        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }

    return std::nullopt;
}

uint32_t ResolveCudaPoolSlotCount(int device_index)
{
    constexpr uint32_t DEFAULT_SLOTS{1};
    constexpr uint32_t MAX_SLOTS{32};

    if (const auto override_slots = ResolveCudaPoolSlotOverride(device_index)) {
        return *override_slots;
    }

    const auto topology = ProbeCudaTopology();
    uint32_t multiprocessor_count{0};
    if (topology.available) {
        for (const auto& device : topology.selected_devices) {
            if (device.device_index == device_index) {
                multiprocessor_count = device.multiprocessor_count;
                break;
            }
        }
    }
    if (multiprocessor_count == 0) {
        return DEFAULT_SLOTS;
    }

    const uint32_t hw = std::thread::hardware_concurrency();
    const uint32_t cpu_limit =
        hw >= 24 ? 8U :
        hw >= 16 ? 6U :
        hw >= 12 ? 4U :
        hw >= 8 ? 3U :
        hw >= 4 ? 2U : 1U;
    const uint32_t gpu_limit =
        multiprocessor_count >= 128 ? 8U :
        multiprocessor_count >= 96 ? 7U :
        multiprocessor_count >= 64 ? 6U :
        multiprocessor_count >= 48 ? 5U :
        multiprocessor_count >= 24 ? 4U :
        multiprocessor_count >= 12 ? 2U : 1U;
    return std::clamp<uint32_t>(std::min(cpu_limit, gpu_limit), 1U, MAX_SLOTS);
}

DigestPoolContext& GetPoolContext(int device_index)
{
    static std::mutex contexts_mutex;
    static std::map<int, std::unique_ptr<DigestPoolContext>> contexts;

    std::lock_guard<std::mutex> lock(contexts_mutex);
    auto& context = contexts[device_index];
    if (context == nullptr) {
        context = std::make_unique<DigestPoolContext>();
        context->slots.resize(ResolveCudaPoolSlotCount(device_index));
        for (auto& slot : context->slots) {
            slot = std::make_unique<DigestPoolSlot>();
        }
    }
    return *context;
}

DigestProfilingContext& GetProfilingContext()
{
    static DigestProfilingContext context;
    return context;
}

using SteadyClock = std::chrono::steady_clock;

double DurationMicros(const SteadyClock::time_point start, const SteadyClock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - start).count();
}

double DurationMillis(const SteadyClock::time_point start, const SteadyClock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

bool IsBaseMatrixCacheHit(const DigestWorkspace& workspace,
                          const uint256* matrix_a_cache_key,
                          const uint256* matrix_b_cache_key,
                          uint32_t matrix_elements)
{
    return matrix_a_cache_key != nullptr &&
        matrix_b_cache_key != nullptr &&
        workspace.cached_matrix_keys_valid &&
        workspace.cached_matrix_a_key == *matrix_a_cache_key &&
        workspace.cached_matrix_b_key == *matrix_b_cache_key &&
        workspace.cached_matrix_elements == matrix_elements &&
        workspace.device_base_a != nullptr &&
        workspace.device_base_b != nullptr;
}

void RecordProfilingSample(const DigestProfilingSample& sample)
{
    auto& context = GetProfilingContext();
    std::lock_guard<std::mutex> lock(context.mutex);
    ++context.samples;
    context.last_n = sample.n;
    context.last_b = sample.b;
    context.last_r = sample.r;
    context.last_batch_size = sample.batch_size;
    context.last_host_stage_us = sample.host_stage_us;
    context.last_submit_h2d_us = sample.submit_h2d_us;
    context.last_submit_d2d_us = sample.submit_d2d_us;
    context.last_stream_wait_event_us = sample.stream_wait_event_us;
    context.last_launch_build_perturbed_us = sample.launch_build_perturbed_us;
    context.last_launch_finalize_us = sample.launch_finalize_us;
    context.last_submit_d2h_us = sample.submit_d2h_us;
    context.last_stream_sync_us = sample.stream_sync_us;
    context.last_total_wall_ms = sample.total_wall_ms;
    context.last_used_low_rank_path = sample.used_low_rank_path;
    context.last_used_device_prepared_inputs = sample.used_device_prepared_inputs;
    context.last_used_pinned_host_staging = sample.used_pinned_host_staging;
    context.last_base_matrix_cache_hit = sample.base_matrix_cache_hit;
    context.last_gpu_build_us = sample.gpu_build_us;
    context.last_gpu_gemm_us = sample.gpu_gemm_us;
    context.last_gpu_tile_hash_us = sample.gpu_tile_hash_us;
    context.last_gpu_copy_us = sample.gpu_copy_us;
    context.last_mode = sample.mode;
    context.reason = "samples_recorded";
}

uint64_t DeviceCapacityBytes(const DigestWorkspace& workspace)
{
    uint64_t bytes{0};
    auto add = [&bytes](size_t capacity, size_t element_size) {
        bytes += static_cast<uint64_t>(capacity) * static_cast<uint64_t>(element_size);
    };

    add(workspace.base_a_capacity, sizeof(Element));
    add(workspace.base_b_capacity, sizeof(Element));
    add(workspace.matrix_a_capacity, sizeof(Element));
    add(workspace.matrix_b_capacity, sizeof(Element));
    add(workspace.seed_a_capacity, sizeof(DeviceSeedBytes));
    add(workspace.seed_b_capacity, sizeof(DeviceSeedBytes));
    add(workspace.packed_noise_capacity, sizeof(Element));
    add(workspace.prepared_input_ptr_capacity, sizeof(Element*));
    add(workspace.output_capacity, sizeof(Element));
    add(workspace.tile_hash_capacity, sizeof(DeviceDigestBytes));
    add(workspace.digest_capacity, sizeof(DeviceDigestBytes));
    add(workspace.sigma_capacity, sizeof(DeviceSeedBytes));
    add(workspace.seed_midstates_capacity, sizeof(uint32_t));
    return bytes;
}

class BufferPoolLease
{
public:
    BufferPoolLease() = default;

    BufferPoolLease(DigestPoolContext* context, DigestPoolSlot* slot)
        : m_context(context), m_slot(slot)
    {
    }

    BufferPoolLease(BufferPoolLease&& other) noexcept
        : m_context(other.m_context), m_slot(other.m_slot)
    {
        other.m_context = nullptr;
        other.m_slot = nullptr;
    }

    BufferPoolLease& operator=(BufferPoolLease&& other) noexcept
    {
        if (this != &other) {
            Release();
            m_context = other.m_context;
            m_slot = other.m_slot;
            other.m_context = nullptr;
            other.m_slot = nullptr;
        }
        return *this;
    }

    BufferPoolLease(const BufferPoolLease&) = delete;
    BufferPoolLease& operator=(const BufferPoolLease&) = delete;

    ~BufferPoolLease() { Release(); }

    DigestWorkspace& workspace() const { return m_slot->workspace; }

    void RecordRequest(uint32_t n, uint32_t b, uint32_t r, bool allocated_buffers) const
    {
        if (m_context == nullptr) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_context->mutex);
        m_context->initialized = true;
        m_context->last_n = n;
        m_context->last_b = b;
        m_context->last_r = r;
        if (allocated_buffers) {
            ++m_context->allocation_events;
        } else {
            ++m_context->reuse_events;
        }
        m_context->reason = "buffer_pool_slots_ready";
    }

private:
    void Release()
    {
        if (m_context == nullptr || m_slot == nullptr) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_context->mutex);
            if (m_slot->in_use) {
                m_slot->in_use = false;
                if (m_context->active_slots > 0) {
                    --m_context->active_slots;
                }
                if (m_context->inflight_submissions > 0) {
                    --m_context->inflight_submissions;
                }
                ++m_context->completed_submissions;
            }
        }

        m_context->cv.notify_one();
        m_context = nullptr;
        m_slot = nullptr;
    }

    DigestPoolContext* m_context{nullptr};
    DigestPoolSlot* m_slot{nullptr};
};

std::optional<BufferPoolLease> AcquireBufferPoolSlot(int device_index, std::string& error)
{
    auto& context = GetPoolContext(device_index);
    std::unique_lock<std::mutex> lock(context.mutex);
    if (context.slots.empty()) {
        error = "No CUDA buffer pool slots are configured";
        return std::nullopt;
    }

    bool waited{false};
    constexpr uint32_t kMaxWaitRounds = 40; // 40 * 50ms = 2 seconds
    uint32_t wait_rounds{0};
    while (true) {
        auto acquire_slot = [&](bool prefer_reused_buffers) -> std::optional<BufferPoolLease> {
            for (size_t offset = 0; offset < context.slots.size(); ++offset) {
                const size_t slot_index = (context.next_slot + offset) % context.slots.size();
                auto& slot = context.slots[slot_index];
                if (slot->in_use) {
                    continue;
                }

                const bool has_reusable_buffers =
                    slot->workspace.stream != nullptr ||
                    slot->workspace.base_a_capacity > 0 ||
                    slot->workspace.base_b_capacity > 0 ||
                    slot->workspace.matrix_a_capacity > 0 ||
                    slot->workspace.matrix_b_capacity > 0 ||
                    slot->workspace.output_capacity > 0 ||
                    slot->workspace.prepared_input_ptr_capacity > 0;
                if (prefer_reused_buffers && !has_reusable_buffers) {
                    continue;
                }

                slot->in_use = true;
                ++context.active_slots;
                context.high_water_slots = std::max(context.high_water_slots, context.active_slots);
                ++context.inflight_submissions;
                context.peak_inflight_submissions = std::max(context.peak_inflight_submissions, context.inflight_submissions);
                context.next_slot = static_cast<uint32_t>((slot_index + 1) % context.slots.size());
                if (waited) {
                    ++context.wait_events;
                }
                return BufferPoolLease{&context, slot.get()};
            }
            return std::nullopt;
        };

        if (auto lease = acquire_slot(/*prefer_reused_buffers=*/true)) {
            return lease;
        }
        if (auto lease = acquire_slot(/*prefer_reused_buffers=*/false)) {
            return lease;
        }

        waited = true;
        if (++wait_rounds > kMaxWaitRounds) {
            error = "CUDA buffer pool exhausted after timeout";
            return std::nullopt;
        }
        context.cv.wait_for(lock, std::chrono::milliseconds{50});
    }
}

// ---------------------------------------------------------------------------
// Device arithmetic over GF(2^31 - 1)
// ---------------------------------------------------------------------------

__host__ __device__ __forceinline__ Element Reduce64(uint64_t value)
{
    const uint64_t fold1 = (value & static_cast<uint64_t>(MODULUS)) + (value >> 31);
    const uint32_t lo = static_cast<uint32_t>(fold1 & MODULUS);
    const uint32_t hi = static_cast<uint32_t>(fold1 >> 31);
    uint32_t result = lo + hi;
    const uint32_t ge_mask = static_cast<uint32_t>(-static_cast<int32_t>(result >= MODULUS));
    result -= (MODULUS & ge_mask);
    return result;
}

__host__ __device__ __forceinline__ Element FieldAdd(Element a, Element b)
{
    uint32_t sum = a + b;
    if (sum >= MODULUS) {
        sum -= MODULUS;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Device SHA-256
// ---------------------------------------------------------------------------

__device__ __forceinline__ uint32_t RotR(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

__device__ __forceinline__ uint32_t ShaCh(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ ((~x) & z);
}

__device__ __forceinline__ uint32_t ShaMaj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

__device__ __forceinline__ uint32_t ShaBSig0(uint32_t x)
{
    return RotR(x, 2U) ^ RotR(x, 13U) ^ RotR(x, 22U);
}

__device__ __forceinline__ uint32_t ShaBSig1(uint32_t x)
{
    return RotR(x, 6U) ^ RotR(x, 11U) ^ RotR(x, 25U);
}

__device__ __forceinline__ uint32_t ShaSSig0(uint32_t x)
{
    return RotR(x, 7U) ^ RotR(x, 18U) ^ (x >> 3U);
}

__device__ __forceinline__ uint32_t ShaSSig1(uint32_t x)
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

__device__ __forceinline__ void SetShaByte(uint32_t w[16], uint32_t offset, uint32_t byte)
{
    const uint32_t word_index = offset >> 2U;
    const uint32_t shift = (3U - (offset & 3U)) * 8U;
    w[word_index] |= (byte & 0xffU) << shift;
}

__device__ __forceinline__ void SetShaLE32(uint32_t w[16], uint32_t offset, uint32_t value)
{
    SetShaByte(w, offset, value & 0xffU);
    SetShaByte(w, offset + 1U, (value >> 8U) & 0xffU);
    SetShaByte(w, offset + 2U, (value >> 16U) & 0xffU);
    SetShaByte(w, offset + 3U, (value >> 24U) & 0xffU);
}

__device__ __forceinline__ uint32_t Bswap32(uint32_t x)
{
    return ((x & 0x000000ffU) << 24U) |
        ((x & 0x0000ff00U) << 8U) |
        ((x & 0x00ff0000U) >> 8U) |
        ((x & 0xff000000U) >> 24U);
}

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

// WINDOWED SHA-256 compression: 16-word sliding schedule instead of a 64-word
// array (byte-identical output, halves the per-thread local-memory frame).
// NOTE: w[] is consumed/overwritten by the schedule; re-fill it fully before
// the next call.
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
    for (uint32_t i = 0; i < 8U; ++i) {
        out[i * 4U] = static_cast<uint8_t>((state[i] >> 24U) & 0xffU);
        out[i * 4U + 1U] = static_cast<uint8_t>((state[i] >> 16U) & 0xffU);
        out[i * 4U + 2U] = static_cast<uint8_t>((state[i] >> 8U) & 0xffU);
        out[i * 4U + 3U] = static_cast<uint8_t>(state[i] & 0xffU);
    }
}

// ---------------------------------------------------------------------------
// Oracle v2 (QTC O5), consensus reference matmul/field.cpp:
//   preimage  = seed_canonical(32) || LE32(block) [|| LE32(retry) when retry > 0]
//   lane k    = ReadLE32(SHA256(preimage) + 4k) & MODULUS, rejected (retry++,
//               SAME lane) when == MODULUS; after 256 retries the fallback
//               SHA256(seed || LE32(block) || "oracle-fallback") lane % MODULUS.
//   element i = block i >> 3, lane i & 7.
// The canonical seed bytes (seed.data() reversed) occupy message words
// w[0..7]; ReadLE32 of a state word's bytes is Bswap32(state[k]).
// ---------------------------------------------------------------------------

__device__ inline void PackSeedWords(const DeviceSeedBytes& seed, uint32_t seed_w[8])
{
    #pragma unroll
    for (uint32_t i = 0; i < 8; ++i) {
        seed_w[i] = 0U;
    }
    for (uint32_t i = 0; i < 32; ++i) {
        SetShaByte(seed_w, i, seed.data[31U - i]);
    }
}

__device__ inline void OracleBlockState(const uint32_t seed_w[8], uint32_t block, uint32_t retry, uint32_t state[8])
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
    SetShaLE32(w, 32U, block);
    uint32_t message_len = 36U;
    if (retry > 0) {
        SetShaLE32(w, 36U, retry);
        message_len = 40U;
    }
    SetShaByte(w, message_len, 0x80U);
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
    SetShaLE32(w, 32U, block);
    constexpr uint8_t fallback_tag[15] = {
        'o', 'r', 'a', 'c', 'l', 'e', '-', 'f', 'a', 'l', 'l', 'b', 'a', 'c', 'k'
    };
    for (uint32_t i = 0; i < 15; ++i) {
        SetShaByte(w, 36U + i, fallback_tag[i]);
    }
    SetShaByte(w, 51U, 0x80U);
    w[15] = 51U * 8U;

    uint32_t state[8];
    Sha256Init(state);
    Sha256Compress(state, w);
    return Bswap32(state[lane]) % MODULUS;
}

__device__ inline Element OracleLaneWithRetries(const uint32_t seed_w[8], uint32_t block, uint32_t lane, uint32_t first_retry)
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

// --- Seed-midstate matrix generation ---
// Every oracle block of a base matrix hashes seed||LE32(block) with the SAME
// 32-byte seed in w[0..7]; SHA-256 rounds 0-7 consume only those words, so the
// post-round-7 state is identical across all blocks. PrecomputeSeedPairMidstates
// computes it once per A/B seed (one thread each) into a 16-word record (8
// packed seed words + 8 state words); the generation kernel resumes each
// block's retry-0 hash at round 8. Byte-identical to OracleBlockState(retry=0);
// the ~2^-31-per-lane rejection defers to the full retry chain, which reads the
// seed words from the same record.

__device__ inline void WriteSeedMidstate(const DeviceSeedBytes& seed, uint32_t* o)
{
    uint32_t w[8];
    PackSeedWords(seed, w);
    uint32_t a = 0x6a09e667U, b = 0xbb67ae85U, c = 0x3c6ef372U, d = 0xa54ff53aU;
    uint32_t e = 0x510e527fU, f = 0x9b05688cU, g = 0x1f83d9abU, h = 0x5be0cd19U;
    #pragma unroll
    for (uint32_t t = 0; t < 8; ++t) {
        const uint32_t t1 = h + ShaBSig1(e) + ShaCh(e, f, g) + SHA256_K[t] + w[t];
        const uint32_t t2 = ShaBSig0(a) + ShaMaj(a, b, c);
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    o[0] = w[0]; o[1] = w[1]; o[2] = w[2]; o[3] = w[3];
    o[4] = w[4]; o[5] = w[5]; o[6] = w[6]; o[7] = w[7];
    o[8] = a; o[9] = b; o[10] = c; o[11] = d;
    o[12] = e; o[13] = f; o[14] = g; o[15] = h;
}

__global__ void PrecomputeSeedPairMidstatesKernel(const DeviceSeedBytes* seeds_a,
                                                  const DeviceSeedBytes* seeds_b,
                                                  uint32_t seed_count,
                                                  uint32_t* midstates)
{
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= seed_count) {
        return;
    }
    WriteSeedMidstate(seeds_a[i], midstates + static_cast<size_t>(i) * 16U);
    WriteSeedMidstate(seeds_b[i], midstates + static_cast<size_t>(seed_count + i) * 16U);
}

// Retry-0 oracle block state resuming from a precomputed midstate record.
__device__ inline void OracleBlockStateFromMidstate(const uint32_t* mb, uint32_t block, uint32_t state[8])
{
    uint32_t w[16];
    w[0] = mb[0]; w[1] = mb[1]; w[2] = mb[2]; w[3] = mb[3];
    w[4] = mb[4]; w[5] = mb[5]; w[6] = mb[6]; w[7] = mb[7];
    #pragma unroll
    for (uint32_t i = 8; i < 16; ++i) {
        w[i] = 0U;
    }
    SetShaLE32(w, 32U, block);
    SetShaByte(w, 36U, 0x80U);
    w[15] = 36U * 8U;

    uint32_t a = mb[8], b = mb[9], c = mb[10], d = mb[11];
    uint32_t e = mb[12], f = mb[13], g = mb[14], h = mb[15];
    #pragma unroll
    for (uint32_t t = 8; t < 64; ++t) {
        uint32_t wt;
        if (t < 16) {
            wt = w[t];
        } else {
            wt = ShaSSig1(w[(t - 2) & 15U]) + w[(t - 7) & 15U] + ShaSSig0(w[(t - 15) & 15U]) + w[(t - 16) & 15U];
            w[t & 15U] = wt;
        }
        const uint32_t t1 = h + ShaBSig1(e) + ShaCh(e, f, g) + SHA256_K[t] + wt;
        const uint32_t t2 = ShaBSig0(a) + ShaMaj(a, b, c);
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] = 0x6a09e667U + a;
    state[1] = 0xbb67ae85U + b;
    state[2] = 0x3c6ef372U + c;
    state[3] = 0xa54ff53aU + d;
    state[4] = 0x510e527fU + e;
    state[5] = 0x9b05688cU + f;
    state[6] = 0x1f83d9abU + g;
    state[7] = 0x5be0cd19U + h;
}

// Eight consecutive base-matrix elements [8*block, 8*block+8) from a midstate.
__device__ inline void BaseBlockFromMidstate(const uint32_t* mb, uint32_t block, Element out[ORACLE_LANES])
{
    uint32_t state[8];
    OracleBlockStateFromMidstate(mb, block, state);
    #pragma unroll
    for (uint32_t lane = 0; lane < ORACLE_LANES; ++lane) {
        const uint32_t candidate = Bswap32(state[lane]) & MODULUS;
        out[lane] = candidate < MODULUS
            ? candidate
            : OracleLaneWithRetries(mb, block, lane, /*first_retry=*/1U);
    }
}

// ---------------------------------------------------------------------------
// Perturbed matrix generation: A' = A + E_L*E_R, B' = B + F_L*F_R.
// ---------------------------------------------------------------------------

// Variable-base path: one thread per 8-element oracle block of BOTH matrices.
// Requires n % 8 == 0 so the eight lanes share one row.
__global__ void GeneratePerturbedMatrixPairFromSeedMidstateKernel(
    const uint32_t* __restrict__ midstates_a,
    const uint32_t* __restrict__ midstates_b,
    const Element* const* __restrict__ packed_input_ptrs,
    uint32_t noise_e_left_offset_words,
    uint32_t noise_e_right_offset_words,
    uint32_t noise_f_left_offset_words,
    uint32_t noise_f_right_offset_words,
    uint32_t n,
    uint32_t r,
    uint32_t blocks_per_matrix,
    size_t total_blocks,
    Element* __restrict__ output_a_batch,
    Element* __restrict__ output_b_batch)
{
    const size_t gid = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (gid >= total_blocks) {
        return;
    }
    const uint32_t batch_index = static_cast<uint32_t>(gid / blocks_per_matrix);
    const uint32_t block = static_cast<uint32_t>(gid % blocks_per_matrix);
    const uint32_t index0 = block * ORACLE_LANES;
    const uint32_t row = index0 / n;
    const uint32_t col0 = index0 % n;

    Element base_a[ORACLE_LANES];
    Element base_b[ORACLE_LANES];
    BaseBlockFromMidstate(midstates_a + static_cast<size_t>(batch_index) * 16U, block, base_a);
    BaseBlockFromMidstate(midstates_b + static_cast<size_t>(batch_index) * 16U, block, base_b);

    const Element* packed_inputs = packed_input_ptrs[batch_index];
    const Element* noise_e_left = packed_inputs + noise_e_left_offset_words;
    const Element* noise_e_right = packed_inputs + noise_e_right_offset_words;
    const Element* noise_f_left = packed_inputs + noise_f_left_offset_words;
    const Element* noise_f_right = packed_inputs + noise_f_right_offset_words;

    uint64_t acc_a[ORACLE_LANES] = {};
    uint64_t acc_b[ORACLE_LANES] = {};
    uint32_t pending{0};
    for (uint32_t k = 0; k < r; ++k) {
        const uint64_t el = noise_e_left[row * r + k];
        const uint64_t fl = noise_f_left[row * r + k];
        const Element* er = noise_e_right + static_cast<size_t>(k) * n + col0;
        const Element* fr = noise_f_right + static_cast<size_t>(k) * n + col0;
        #pragma unroll
        for (uint32_t lane = 0; lane < ORACLE_LANES; ++lane) {
            acc_a[lane] += el * er[lane];
            acc_b[lane] += fl * fr[lane];
        }
        if (++pending == REDUCE_INTERVAL) {
            #pragma unroll
            for (uint32_t lane = 0; lane < ORACLE_LANES; ++lane) {
                acc_a[lane] = Reduce64(acc_a[lane]);
                acc_b[lane] = Reduce64(acc_b[lane]);
            }
            pending = 0;
        }
    }

    const size_t out_offset = static_cast<size_t>(batch_index) * blocks_per_matrix * ORACLE_LANES + index0;
    #pragma unroll
    for (uint32_t lane = 0; lane < ORACLE_LANES; ++lane) {
        output_a_batch[out_offset + lane] = FieldAdd(base_a[lane], Reduce64(acc_a[lane]));
        output_b_batch[out_offset + lane] = FieldAdd(base_b[lane], Reduce64(acc_b[lane]));
    }
}

// Cached-base path: one thread per element, base matrix shared by the batch.
__global__ void BuildPerturbedMatrixPackedPointersKernel(const Element* __restrict__ base_matrix,
                                                         const Element* const* __restrict__ packed_input_ptrs,
                                                         uint32_t noise_left_offset_words,
                                                         uint32_t noise_right_offset_words,
                                                         uint32_t n,
                                                         uint32_t r,
                                                         size_t total_matrix_elements,
                                                         uint32_t matrix_elements,
                                                         Element* __restrict__ output_batch)
{
    const size_t gid = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (gid >= total_matrix_elements) {
        return;
    }

    const uint32_t batch_index = static_cast<uint32_t>(gid / matrix_elements);
    const uint32_t local_index = static_cast<uint32_t>(gid % matrix_elements);
    const uint32_t row = local_index / n;
    const uint32_t col = local_index % n;
    const Element* packed_inputs = packed_input_ptrs[batch_index];
    const Element* noise_left = packed_inputs + noise_left_offset_words;
    const Element* noise_right = packed_inputs + noise_right_offset_words;

    uint64_t acc{0};
    uint32_t pending{0};
    for (uint32_t k = 0; k < r; ++k) {
        acc += static_cast<uint64_t>(noise_left[row * r + k]) * noise_right[k * n + col];
        if (++pending == REDUCE_INTERVAL) {
            acc = Reduce64(acc);
            pending = 0;
        }
    }

    output_batch[gid] = FieldAdd(base_matrix[local_index], Reduce64(acc));
}

// ---------------------------------------------------------------------------
// C' = A'B' over GF(2^31-1). Products are < 2^62, so up to four accumulate in a
// uint64 before Reduce64 (exact modular reduction: the summation order does not
// affect the field element).
// ---------------------------------------------------------------------------

__global__ __launch_bounds__(GEMM_THREADS)
void FieldGemmTiledKernel(const Element* __restrict__ a_batch,
                          const Element* __restrict__ b_batch,
                          uint32_t n,
                          uint32_t matrix_elements,
                          Element* __restrict__ c_batch)
{
    __shared__ Element as[GEMM_BM][GEMM_BK + 1U];
    __shared__ __align__(16) Element bs[GEMM_BK][GEMM_BN];

    const uint32_t batch_index = blockIdx.z;
    const uint32_t row0 = blockIdx.y * GEMM_BM;
    const uint32_t col0 = blockIdx.x * GEMM_BN;
    const Element* A = a_batch + static_cast<size_t>(batch_index) * matrix_elements;
    const Element* B = b_batch + static_cast<size_t>(batch_index) * matrix_elements;
    Element* C = c_batch + static_cast<size_t>(batch_index) * matrix_elements;

    const uint32_t tid = threadIdx.x;
    const uint32_t tx = tid & 15U;
    const uint32_t ty = tid >> 4U;

    uint64_t acc[4][4] = {};
    uint32_t pending{0};

    for (uint32_t k0 = 0; k0 < n; k0 += GEMM_BK) {
        #pragma unroll
        for (uint32_t i = 0; i < 4U; ++i) {
            const uint32_t idx = tid + i * GEMM_THREADS;
            const uint32_t r = idx >> 4U;
            const uint32_t kk = idx & 15U;
            as[r][kk] = A[static_cast<size_t>(row0 + r) * n + k0 + kk];
        }
        #pragma unroll
        for (uint32_t i = 0; i < 4U; ++i) {
            const uint32_t idx = tid + i * GEMM_THREADS;
            const uint32_t kk = idx >> 6U;
            const uint32_t c = idx & 63U;
            bs[kk][c] = B[static_cast<size_t>(k0 + kk) * n + col0 + c];
        }
        __syncthreads();

        #pragma unroll
        for (uint32_t kk = 0; kk < GEMM_BK; ++kk) {
            const uint64_t a0 = as[ty * 4U][kk];
            const uint64_t a1 = as[ty * 4U + 1U][kk];
            const uint64_t a2 = as[ty * 4U + 2U][kk];
            const uint64_t a3 = as[ty * 4U + 3U][kk];
            const uint4 bv = *reinterpret_cast<const uint4*>(&bs[kk][tx * 4U]);
            const uint64_t b0 = bv.x;
            const uint64_t b1 = bv.y;
            const uint64_t b2 = bv.z;
            const uint64_t b3 = bv.w;
            acc[0][0] += a0 * b0; acc[0][1] += a0 * b1; acc[0][2] += a0 * b2; acc[0][3] += a0 * b3;
            acc[1][0] += a1 * b0; acc[1][1] += a1 * b1; acc[1][2] += a1 * b2; acc[1][3] += a1 * b3;
            acc[2][0] += a2 * b0; acc[2][1] += a2 * b1; acc[2][2] += a2 * b2; acc[2][3] += a2 * b3;
            acc[3][0] += a3 * b0; acc[3][1] += a3 * b1; acc[3][2] += a3 * b2; acc[3][3] += a3 * b3;
            if (++pending == REDUCE_INTERVAL) {
                #pragma unroll
                for (uint32_t i = 0; i < 4U; ++i) {
                    #pragma unroll
                    for (uint32_t j = 0; j < 4U; ++j) {
                        acc[i][j] = Reduce64(acc[i][j]);
                    }
                }
                pending = 0;
            }
        }
        __syncthreads();
    }

    #pragma unroll
    for (uint32_t i = 0; i < 4U; ++i) {
        Element* c_row = C + static_cast<size_t>(row0 + ty * 4U + i) * n + col0 + tx * 4U;
        #pragma unroll
        for (uint32_t j = 0; j < 4U; ++j) {
            c_row[j] = Reduce64(acc[i][j]);
        }
    }
}

// Generic fallback for shapes not divisible by the 64-wide tile (small n).
__global__ void FieldGemmSimpleKernel(const Element* __restrict__ a_batch,
                                      const Element* __restrict__ b_batch,
                                      uint32_t n,
                                      uint32_t matrix_elements,
                                      size_t total_elements,
                                      Element* __restrict__ c_batch)
{
    const size_t gid = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (gid >= total_elements) {
        return;
    }
    const uint32_t batch_index = static_cast<uint32_t>(gid / matrix_elements);
    const uint32_t local_index = static_cast<uint32_t>(gid % matrix_elements);
    const uint32_t row = local_index / n;
    const uint32_t col = local_index % n;
    const Element* A = a_batch + static_cast<size_t>(batch_index) * matrix_elements + static_cast<size_t>(row) * n;
    const Element* B = b_batch + static_cast<size_t>(batch_index) * matrix_elements + col;

    uint64_t acc{0};
    uint32_t pending{0};
    for (uint32_t k = 0; k < n; ++k) {
        acc += static_cast<uint64_t>(A[k]) * B[static_cast<size_t>(k) * n];
        if (++pending == REDUCE_INTERVAL) {
            acc = Reduce64(acc);
            pending = 0;
        }
    }
    c_batch[gid] = Reduce64(acc);
}

// ---------------------------------------------------------------------------
// Tile hashing (transcript::HashProductTile): one thread per b x b tile,
// single SHA-256 over the tile's b*b elements as LE32 bytes in row-major order.
// ---------------------------------------------------------------------------

__global__ void HashProductTilesKernel(const Element* __restrict__ c_batch,
                                       uint32_t n,
                                       uint32_t b,
                                       uint32_t blocks_per_axis,
                                       uint32_t tiles_per_request,
                                       uint32_t matrix_elements,
                                       uint32_t total_tiles,
                                       DeviceDigestBytes* __restrict__ out)
{
    const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
    if (gid >= total_tiles) {
        return;
    }
    const uint32_t batch_index = gid / tiles_per_request;
    const uint32_t local_tile = gid % tiles_per_request;
    const uint32_t ti = local_tile / blocks_per_axis;
    const uint32_t tj = local_tile % blocks_per_axis;
    const Element* tile = c_batch + static_cast<size_t>(batch_index) * matrix_elements +
        static_cast<size_t>(ti) * b * n + static_cast<size_t>(tj) * b;

    uint32_t state[8];
    Sha256Init(state);
    uint32_t w[16];
    uint32_t wi = 0;
    for (uint32_t x = 0; x < b; ++x) {
        const Element* row = tile + static_cast<size_t>(x) * n;
        for (uint32_t y = 0; y < b; ++y) {
            // LE32 element bytes read as a big-endian SHA word.
            w[wi++] = Bswap32(row[y]);
            if (wi == 16U) {
                Sha256Compress(state, w);
                wi = 0;
            }
        }
    }

    const uint64_t bit_length = static_cast<uint64_t>(b) * b * 32U;
    w[wi] = 0x80000000U;
    for (uint32_t i = wi + 1U; i < 16U; ++i) {
        w[i] = 0U;
    }
    if (wi <= 13U) {
        w[14] = static_cast<uint32_t>(bit_length >> 32U);
        w[15] = static_cast<uint32_t>(bit_length);
        Sha256Compress(state, w);
    } else {
        Sha256Compress(state, w);
        #pragma unroll
        for (uint32_t i = 0; i < 16U; ++i) {
            w[i] = 0U;
        }
        w[14] = static_cast<uint32_t>(bit_length >> 32U);
        w[15] = static_cast<uint32_t>(bit_length);
        Sha256Compress(state, w);
    }
    Sha256StateToBytes(state, out[gid].data);
}

__device__ inline void SetShaBytes(uint32_t w[16], uint32_t offset, const uint8_t* data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        SetShaByte(w, offset + i, data[i]);
    }
}

__device__ inline void Sha256SingleBlock32(const uint8_t in[32], uint8_t out[32])
{
    uint32_t w[16] = {};
    SetShaBytes(w, 0U, in, 32U);
    SetShaByte(w, 32U, 0x80U);
    w[15] = 32U * 8U;

    uint32_t state[8];
    Sha256Init(state);
    Sha256Compress(state, w);
    Sha256StateToBytes(state, out);
}

// ---------------------------------------------------------------------------
// Digest finish on the device (transcript::HashProductTileHashes +
// FinalizeProductCommittedDigestFromHash): one thread per request.
//   root   = SHA256(tile_hash[0] || ... || tile_hash[N^2-1])   (row-major)
//   digest = SHA256d("matmul-product-digest-v4" || sigma || root || LE32(n) || LE32(b))
// Only the 32-byte digest travels back to the host.
// ---------------------------------------------------------------------------

__global__ void FinalizeProductDigestsKernel(const DeviceDigestBytes* __restrict__ tile_hashes,
                                             const DeviceSeedBytes* __restrict__ sigmas,
                                             uint32_t tiles_per_request,
                                             uint32_t batch_size,
                                             uint32_t n,
                                             uint32_t b,
                                             DeviceDigestBytes* __restrict__ digests)
{
    const uint32_t batch_index = blockIdx.x * blockDim.x + threadIdx.x;
    if (batch_index >= batch_size) {
        return;
    }

    uint32_t state[8];
    Sha256Init(state);
    uint32_t w[16];
    uint32_t wi = 0;
    const DeviceDigestBytes* tiles = tile_hashes + static_cast<size_t>(batch_index) * tiles_per_request;
    for (uint32_t t = 0; t < tiles_per_request; ++t) {
        const uint4* words = reinterpret_cast<const uint4*>(tiles[t].data);
        const uint4 lo = words[0];
        const uint4 hi = words[1];
        // Raw digest bytes read little-endian; SHA words are big-endian.
        w[wi++] = Bswap32(lo.x);
        w[wi++] = Bswap32(lo.y);
        w[wi++] = Bswap32(lo.z);
        w[wi++] = Bswap32(lo.w);
        w[wi++] = Bswap32(hi.x);
        w[wi++] = Bswap32(hi.y);
        w[wi++] = Bswap32(hi.z);
        w[wi++] = Bswap32(hi.w);
        if (wi == 16U) {
            Sha256Compress(state, w);
            wi = 0;
        }
    }
    const uint64_t bit_length = static_cast<uint64_t>(tiles_per_request) * 32U * 8U;
    w[wi] = 0x80000000U;
    for (uint32_t i = wi + 1U; i < 16U; ++i) {
        w[i] = 0U;
    }
    if (wi <= 13U) {
        w[14] = static_cast<uint32_t>(bit_length >> 32U);
        w[15] = static_cast<uint32_t>(bit_length);
        Sha256Compress(state, w);
    } else {
        Sha256Compress(state, w);
        #pragma unroll
        for (uint32_t i = 0; i < 16U; ++i) {
            w[i] = 0U;
        }
        w[14] = static_cast<uint32_t>(bit_length >> 32U);
        w[15] = static_cast<uint32_t>(bit_length);
        Sha256Compress(state, w);
    }
    uint8_t root[32];
    Sha256StateToBytes(state, root);

    constexpr uint8_t PRODUCT_DIGEST_TAG_V4[24] = {
        'm', 'a', 't', 'm', 'u', 'l', '-', 'p',
        'r', 'o', 'd', 'u', 'c', 't', '-', 'd',
        'i', 'g', 'e', 's', 't', '-', 'v', '4'
    };
    Sha256Init(state);
    uint32_t block0[16] = {};
    SetShaBytes(block0, 0U, PRODUCT_DIGEST_TAG_V4, 24U);
    SetShaBytes(block0, 24U, sigmas[batch_index].data, 32U);
    SetShaBytes(block0, 56U, root, 8U);
    Sha256Compress(state, block0);

    uint32_t block1[16] = {};
    SetShaBytes(block1, 0U, root + 8U, 24U);
    SetShaLE32(block1, 24U, n);
    SetShaLE32(block1, 28U, b);
    SetShaByte(block1, 32U, 0x80U);
    block1[15] = 96U * 8U;
    Sha256Compress(state, block1);

    uint8_t inner[32];
    Sha256StateToBytes(state, inner);
    Sha256SingleBlock32(inner, digests[batch_index].data);
}

// ---------------------------------------------------------------------------
// Host plumbing
// ---------------------------------------------------------------------------

std::string CudaErrorString(cudaError_t error)
{
    return cudaGetErrorString(error);
}

void ResetWorkspaceForDevice(DigestWorkspace& workspace, int device_index)
{
    if (workspace.device_index == device_index) {
        return;
    }

    if (workspace.device_index >= 0) {
        cudaSetDevice(workspace.device_index);
    }
    workspace.ReleaseStream();
    workspace.ReleaseDeviceBuffers();
    workspace.device_index = device_index;
}

bool EnsureWorkspaceStream(DigestWorkspace& workspace, std::string& error)
{
    if (workspace.stream != nullptr) {
        return true;
    }

    const cudaError_t stream_error = cudaStreamCreateWithFlags(&workspace.stream, cudaStreamNonBlocking);
    if (stream_error != cudaSuccess) {
        error = "cudaStreamCreateWithFlags failed:" + CudaErrorString(stream_error);
        workspace.stream = nullptr;
        return false;
    }
    return true;
}

template <typename T>
bool EnsureDeviceBuffer(T*& buffer, size_t& capacity, size_t required, std::string& error, bool& allocated)
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
        error = "cudaMalloc failed:" + CudaErrorString(alloc_error);
        return false;
    }

    allocated = true;
    capacity = required;
    return true;
}

bool EnsureCachedBaseMatrices(DigestWorkspace& workspace,
                              const CudaRuntimeProbe& runtime,
                              const Element* matrix_a,
                              const Element* matrix_b,
                              const uint256* matrix_a_cache_key,
                              const uint256* matrix_b_cache_key,
                              uint32_t matrix_elements,
                              std::string& error,
                              bool& allocated)
{
    if (IsBaseMatrixCacheHit(workspace, matrix_a_cache_key, matrix_b_cache_key, matrix_elements)) {
        return true;
    }

    if (!EnsureDeviceBuffer(workspace.device_base_a, workspace.base_a_capacity, matrix_elements, error, allocated) ||
        !EnsureDeviceBuffer(workspace.device_base_b, workspace.base_b_capacity, matrix_elements, error, allocated)) {
        return false;
    }

    cudaError_t copy_error = cudaSetDevice(runtime.device_index);
    if (copy_error == cudaSuccess) {
        copy_error = cudaMemcpyAsync(workspace.device_base_a,
                                     matrix_a,
                                     matrix_elements * sizeof(Element),
                                     cudaMemcpyHostToDevice,
                                     workspace.stream);
    }
    if (copy_error == cudaSuccess) {
        copy_error = cudaMemcpyAsync(workspace.device_base_b,
                                     matrix_b,
                                     matrix_elements * sizeof(Element),
                                     cudaMemcpyHostToDevice,
                                     workspace.stream);
    }
    if (copy_error != cudaSuccess) {
        error = "cudaMemcpy base_matrices failed:" + CudaErrorString(copy_error);
        return false;
    }

    if (matrix_a_cache_key != nullptr && matrix_b_cache_key != nullptr) {
        workspace.cached_matrix_a_key = *matrix_a_cache_key;
        workspace.cached_matrix_b_key = *matrix_b_cache_key;
        workspace.cached_matrix_keys_valid = true;
    } else {
        workspace.cached_matrix_a_key.SetNull();
        workspace.cached_matrix_b_key.SetNull();
        workspace.cached_matrix_keys_valid = false;
    }
    workspace.cached_matrix_elements = matrix_elements;
    return true;
}

bool ValidateShape(uint32_t n, uint32_t b, uint32_t batch_size, std::string& error)
{
    if (batch_size == 0) {
        error = "CUDA digest batch request requires at least one entry";
        return false;
    }
    if (n == 0 || b == 0) {
        error = "matrix dimension and transcript block size must be non-zero";
        return false;
    }
    if ((n % b) != 0) {
        error = "matrix dimension must be divisible by transcript block size";
        return false;
    }
    if ((n % ORACLE_LANES) != 0) {
        error = "matrix dimension must be a multiple of the 8-lane oracle block";
        return false;
    }
    if (b > MAX_SUPPORTED_BLOCK_SIZE) {
        error = "CUDA digest request block size exceeds supported tile size";
        return false;
    }
    if (n > 4096U) {
        error = "CUDA digest request dimension exceeds supported bounds";
        return false;
    }
    if (batch_size > 65535U) {
        error = "CUDA digest batch size exceeds supported bounds";
        return false;
    }
    return true;
}

bool ValidateRank(uint32_t n, uint32_t r, std::string& error)
{
    if (r == 0) {
        error = "noise rank must be non-zero";
        return false;
    }
    if (r > n) {
        error = "noise rank exceeds matrix dimension";
        return false;
    }
    return true;
}

bool ValidateHostNoisePointers(const Element* const* e_l,
                               const Element* const* e_r,
                               const Element* const* f_l,
                               const Element* const* f_r,
                               uint32_t batch_size,
                               std::string& error)
{
    if (e_l == nullptr || e_r == nullptr || f_l == nullptr || f_r == nullptr) {
        error = "CUDA digest batch request requires all four noise factor arrays";
        return false;
    }
    for (uint32_t i = 0; i < batch_size; ++i) {
        if (e_l[i] == nullptr || e_r[i] == nullptr || f_l[i] == nullptr || f_r[i] == nullptr) {
            error = "CUDA digest batch request contains null noise pointers";
            return false;
        }
    }
    return true;
}

bool ValidateGeneratedInputs(const MatMulGeneratedInputsDevice* const* generated_inputs,
                             uint32_t batch_size,
                             std::string& error)
{
    if (generated_inputs == nullptr) {
        error = "CUDA digest batch request requires device-generated inputs";
        return false;
    }
    for (uint32_t i = 0; i < batch_size; ++i) {
        if (generated_inputs[i] == nullptr) {
            error = "CUDA digest batch request contains null device-generated input handles";
            return false;
        }
    }
    return true;
}

bool ValidateProductTileHashBatchRequest(const MatMulProductTileHashBatchRequest& request, std::string& error)
{
    if (!ValidateShape(request.n, request.b, request.batch_size, error)) {
        return false;
    }
    if (request.matrix_a_perturbed == nullptr || request.matrix_b_perturbed == nullptr) {
        error = "CUDA digest batch request requires perturbed matrices";
        return false;
    }
    for (uint32_t i = 0; i < request.batch_size; ++i) {
        if (request.matrix_a_perturbed[i] == nullptr || request.matrix_b_perturbed[i] == nullptr) {
            error = "CUDA digest batch request contains null matrix pointers";
            return false;
        }
    }
    return true;
}

bool ValidateLowRankBatchRequest(const MatMulLowRankProductBatchRequest& request, std::string& error)
{
    if (!ValidateShape(request.n, request.b, request.batch_size, error) ||
        !ValidateRank(request.n, request.r, error)) {
        return false;
    }
    if (request.matrix_a == nullptr || request.matrix_b == nullptr) {
        error = "CUDA digest batch request requires base matrices";
        return false;
    }
    return ValidateHostNoisePointers(
        request.noise_e_l, request.noise_e_r, request.noise_f_l, request.noise_f_r, request.batch_size, error);
}

bool ValidateLowRankDeviceBatchRequest(const MatMulLowRankProductDeviceBatchRequest& request, std::string& error)
{
    if (!ValidateShape(request.n, request.b, request.batch_size, error) ||
        !ValidateRank(request.n, request.r, error)) {
        return false;
    }
    if (request.matrix_a == nullptr || request.matrix_b == nullptr) {
        error = "CUDA digest batch request requires base matrices";
        return false;
    }
    return ValidateGeneratedInputs(request.generated_inputs, request.batch_size, error);
}

bool VariableBaseRequestUsesDeviceInputs(const MatMulLowRankVariableBaseProductBatchRequest& request)
{
    return request.generated_inputs != nullptr;
}

bool ValidateLowRankVariableBaseBatchRequest(const MatMulLowRankVariableBaseProductBatchRequest& request,
                                             std::string& error)
{
    if (!ValidateShape(request.n, request.b, request.batch_size, error) ||
        !ValidateRank(request.n, request.r, error)) {
        return false;
    }
    if (request.matrix_a_seeds == nullptr || request.matrix_b_seeds == nullptr) {
        error = "CUDA variable-base digest batch request requires matrix seeds";
        return false;
    }
    if (VariableBaseRequestUsesDeviceInputs(request)) {
        return ValidateGeneratedInputs(request.generated_inputs, request.batch_size, error);
    }
    return ValidateHostNoisePointers(
        request.noise_e_l, request.noise_e_r, request.noise_f_l, request.noise_f_r, request.batch_size, error);
}

struct NoiseLayout {
    uint32_t noise_left_elements{0};
    uint32_t noise_right_elements{0};
    uint32_t e_l_offset{0};
    uint32_t e_r_offset{0};
    uint32_t f_l_offset{0};
    uint32_t f_r_offset{0};
    uint32_t packed_words{0};
};

NoiseLayout MakeNoiseLayout(uint32_t n, uint32_t r)
{
    NoiseLayout layout;
    layout.noise_left_elements = n * r;
    layout.noise_right_elements = r * n;
    layout.e_l_offset = 0;
    layout.e_r_offset = layout.e_l_offset + layout.noise_left_elements;
    layout.f_l_offset = layout.e_r_offset + layout.noise_right_elements;
    layout.f_r_offset = layout.f_l_offset + layout.noise_left_elements;
    layout.packed_words = layout.f_r_offset + layout.noise_right_elements;
    return layout;
}

// Stage host noise pointers into a per-request packed device buffer and build
// the device pointer table so every downstream kernel sees the same
// [E_L | E_R | F_L | F_R] layout as device-generated inputs.
bool StageHostNoise(DigestWorkspace& workspace,
                    const NoiseLayout& layout,
                    uint32_t batch_size,
                    const Element* const* noise_e_l,
                    const Element* const* noise_e_r,
                    const Element* const* noise_f_l,
                    const Element* const* noise_f_r,
                    DigestProfilingSample& sample,
                    std::string& error,
                    bool& allocated_buffers)
{
    const size_t total_packed = static_cast<size_t>(batch_size) * layout.packed_words;
    if (!EnsureDeviceBuffer(workspace.device_packed_noise, workspace.packed_noise_capacity, total_packed, error, allocated_buffers) ||
        !EnsureDeviceBuffer(workspace.device_prepared_input_ptrs, workspace.prepared_input_ptr_capacity, batch_size, error, allocated_buffers)) {
        return false;
    }
    std::string staging_warning;
    if (!workspace.host_packed_noise.Ensure(total_packed, staging_warning)) {
        error = staging_warning;
        return false;
    }
    sample.used_pinned_host_staging = sample.used_pinned_host_staging && workspace.host_packed_noise.pinned != nullptr;

    if (!workspace.host_pointer_table.Ensure(batch_size, staging_warning)) {
        error = staging_warning;
        return false;
    }
    const auto host_stage_start = SteadyClock::now();
    const Element** pointer_table = workspace.host_pointer_table.data();
    Element* host = workspace.host_packed_noise.data();
    for (uint32_t i = 0; i < batch_size; ++i) {
        Element* dst = host + static_cast<size_t>(i) * layout.packed_words;
        std::memcpy(dst + layout.e_l_offset, noise_e_l[i], layout.noise_left_elements * sizeof(Element));
        std::memcpy(dst + layout.e_r_offset, noise_e_r[i], layout.noise_right_elements * sizeof(Element));
        std::memcpy(dst + layout.f_l_offset, noise_f_l[i], layout.noise_left_elements * sizeof(Element));
        std::memcpy(dst + layout.f_r_offset, noise_f_r[i], layout.noise_right_elements * sizeof(Element));
        pointer_table[i] = workspace.device_packed_noise + static_cast<size_t>(i) * layout.packed_words;
    }
    sample.host_stage_us += DurationMicros(host_stage_start, SteadyClock::now());

    const auto h2d_start = SteadyClock::now();
    cudaError_t copy_error = cudaMemcpyAsync(workspace.device_packed_noise,
                                             host,
                                             total_packed * sizeof(Element),
                                             cudaMemcpyHostToDevice,
                                             workspace.stream);
    if (copy_error == cudaSuccess) {
        copy_error = cudaMemcpyAsync(workspace.device_prepared_input_ptrs,
                                     pointer_table,
                                     batch_size * sizeof(const Element*),
                                     cudaMemcpyHostToDevice,
                                     workspace.stream);
    }
    sample.submit_h2d_us += DurationMicros(h2d_start, SteadyClock::now());
    if (copy_error != cudaSuccess) {
        error = "cudaMemcpy host noise failed:" + CudaErrorString(copy_error);
        return false;
    }
    return true;
}

// Wait on device-generated inputs and build the device pointer table.
bool StageDeviceGeneratedInputs(DigestWorkspace& workspace,
                                const CudaRuntimeProbe& runtime,
                                const NoiseLayout& layout,
                                uint32_t n,
                                uint32_t b,
                                uint32_t r,
                                uint32_t batch_size,
                                const MatMulGeneratedInputsDevice* const* generated_inputs,
                                DigestProfilingSample& sample,
                                std::string& error,
                                bool& allocated_buffers)
{
    if (!EnsureDeviceBuffer(workspace.device_prepared_input_ptrs, workspace.prepared_input_ptr_capacity, batch_size, error, allocated_buffers)) {
        return false;
    }

    std::string staging_warning;
    if (!workspace.host_pointer_table.Ensure(batch_size, staging_warning)) {
        error = staging_warning;
        return false;
    }
    const Element** pointer_table = workspace.host_pointer_table.data();
    const auto wait_start = SteadyClock::now();
    for (uint32_t i = 0; i < batch_size; ++i) {
        const auto* generated = generated_inputs[i];
        if (generated->device_index != runtime.device_index ||
            generated->n != n ||
            generated->b != b ||
            generated->r != r ||
            generated->noise_words != layout.noise_left_elements ||
            generated->storage == nullptr ||
            generated->noise_e_l != generated->storage + layout.e_l_offset ||
            generated->noise_e_r != generated->storage + layout.e_r_offset ||
            generated->noise_f_l != generated->storage + layout.f_l_offset ||
            generated->noise_f_r != generated->storage + layout.f_r_offset) {
            error = "CUDA digest batch request contains incompatible device-generated inputs";
            return false;
        }

        if (generated->ready_event != nullptr) {
            const cudaError_t wait_error = cudaStreamWaitEvent(
                workspace.stream,
                reinterpret_cast<cudaEvent_t>(generated->ready_event),
                0);
            if (wait_error != cudaSuccess) {
                error = "cudaStreamWaitEvent failed:" + CudaErrorString(wait_error);
                return false;
            }
        }

        pointer_table[i] = generated->storage;
    }
    sample.stream_wait_event_us = DurationMicros(wait_start, SteadyClock::now());

    const auto pointer_copy_start = SteadyClock::now();
    const cudaError_t copy_error = cudaMemcpyAsync(workspace.device_prepared_input_ptrs,
                                                   pointer_table,
                                                   batch_size * sizeof(const Element*),
                                                   cudaMemcpyHostToDevice,
                                                   workspace.stream);
    sample.submit_h2d_us += DurationMicros(pointer_copy_start, SteadyClock::now());
    if (copy_error != cudaSuccess) {
        error = "cudaMemcpy prepared_input_ptrs failed:" + CudaErrorString(copy_error);
        return false;
    }
    return true;
}

// Build A'/B' from the cached base matrices plus the staged packed noise.
bool LaunchPerturbFromCachedBase(DigestWorkspace& workspace,
                                 const NoiseLayout& layout,
                                 uint32_t n,
                                 uint32_t r,
                                 uint32_t batch_size,
                                 DigestProfilingSample& sample,
                                 std::string& error,
                                 bool& allocated_buffers)
{
    const uint32_t matrix_elements = n * n;
    const size_t total_matrix_elements = static_cast<size_t>(batch_size) * matrix_elements;
    if (!EnsureDeviceBuffer(workspace.device_matrix_a, workspace.matrix_a_capacity, total_matrix_elements, error, allocated_buffers) ||
        !EnsureDeviceBuffer(workspace.device_matrix_b, workspace.matrix_b_capacity, total_matrix_elements, error, allocated_buffers)) {
        return false;
    }

    const uint32_t build_blocks = static_cast<uint32_t>((total_matrix_elements + WORKSPACE_THREADS - 1) / WORKSPACE_THREADS);
    const auto build_start = SteadyClock::now();
    workspace.MarkBuildStart();
    BuildPerturbedMatrixPackedPointersKernel<<<build_blocks, WORKSPACE_THREADS, 0, workspace.stream>>>(
        workspace.device_base_a,
        workspace.device_prepared_input_ptrs,
        layout.e_l_offset,
        layout.e_r_offset,
        n,
        r,
        total_matrix_elements,
        matrix_elements,
        workspace.device_matrix_a);
    cudaError_t launch_error = cudaGetLastError();
    if (launch_error == cudaSuccess) {
        BuildPerturbedMatrixPackedPointersKernel<<<build_blocks, WORKSPACE_THREADS, 0, workspace.stream>>>(
            workspace.device_base_b,
            workspace.device_prepared_input_ptrs,
            layout.f_l_offset,
            layout.f_r_offset,
            n,
            r,
            total_matrix_elements,
            matrix_elements,
            workspace.device_matrix_b);
        launch_error = cudaGetLastError();
    }
    sample.launch_build_perturbed_us = DurationMicros(build_start, SteadyClock::now());
    if (launch_error != cudaSuccess) {
        error = "CUDA perturbed-matrix kernel failed:" + CudaErrorString(launch_error);
        return false;
    }
    return true;
}

// GEMM + tile hashing over device_matrix_a/b, then copy the tile hashes back.
bool RunProductTileHashPipeline(const BufferPoolLease& lease,
                                DigestWorkspace& workspace,
                                uint32_t n,
                                uint32_t b,
                                uint32_t r,
                                uint32_t batch_size,
                                const uint256* sigmas,
                                bool return_tile_hashes,
                                MatMulProductTileHashBatchResult& result,
                                DigestProfilingSample& sample,
                                bool& allocated_buffers)
{
    const uint32_t blocks_per_axis = n / b;
    const uint32_t tiles_per_request = blocks_per_axis * blocks_per_axis;
    const uint32_t matrix_elements = n * n;
    const size_t total_output_count = static_cast<size_t>(batch_size) * matrix_elements;
    const uint32_t total_tiles = batch_size * tiles_per_request;
    const bool finish_on_device = sigmas != nullptr;
    const bool fetch_tile_hashes = !finish_on_device || return_tile_hashes;

    if (!EnsureDeviceBuffer(workspace.device_output,
                            workspace.output_capacity,
                            total_output_count,
                            result.error,
                            allocated_buffers) ||
        !EnsureDeviceBuffer(workspace.device_tile_hashes,
                            workspace.tile_hash_capacity,
                            total_tiles,
                            result.error,
                            allocated_buffers)) {
        return false;
    }
    if (finish_on_device) {
        if (!EnsureDeviceBuffer(workspace.device_sigma,
                                workspace.sigma_capacity,
                                batch_size,
                                result.error,
                                allocated_buffers) ||
            !EnsureDeviceBuffer(workspace.device_digests,
                                workspace.digest_capacity,
                                batch_size,
                                result.error,
                                allocated_buffers)) {
            return false;
        }
        std::string staging_warning;
        if (!workspace.host_sigma.Ensure(batch_size, staging_warning) ||
            !workspace.host_digests.Ensure(batch_size, staging_warning)) {
            result.error = staging_warning;
            return false;
        }
        std::memcpy(workspace.host_sigma.data(), sigmas, static_cast<size_t>(batch_size) * sizeof(DeviceSeedBytes));
        const auto sigma_h2d_start = SteadyClock::now();
        const cudaError_t sigma_error = cudaMemcpyAsync(workspace.device_sigma,
                                                        workspace.host_sigma.data(),
                                                        static_cast<size_t>(batch_size) * sizeof(DeviceSeedBytes),
                                                        cudaMemcpyHostToDevice,
                                                        workspace.stream);
        sample.submit_h2d_us += DurationMicros(sigma_h2d_start, SteadyClock::now());
        if (sigma_error != cudaSuccess) {
            result.error = "cudaMemcpy sigmas failed:" + CudaErrorString(sigma_error);
            return false;
        }
    }

    lease.RecordRequest(n, b, r, allocated_buffers);

    const bool timing_events = workspace.EnsureTimingEvents();
    if (timing_events) {
        cudaEventRecord(workspace.event_gemm_start, workspace.stream);
    }
    const auto finalize_start = SteadyClock::now();
    if (n % GEMM_BM == 0U) {
        const dim3 grid(n / GEMM_BN, n / GEMM_BM, batch_size);
        FieldGemmTiledKernel<<<grid, GEMM_THREADS, 0, workspace.stream>>>(
            workspace.device_matrix_a,
            workspace.device_matrix_b,
            n,
            matrix_elements,
            workspace.device_output);
    } else {
        const uint32_t gemm_blocks = static_cast<uint32_t>((total_output_count + WORKSPACE_THREADS - 1) / WORKSPACE_THREADS);
        FieldGemmSimpleKernel<<<gemm_blocks, WORKSPACE_THREADS, 0, workspace.stream>>>(
            workspace.device_matrix_a,
            workspace.device_matrix_b,
            n,
            matrix_elements,
            total_output_count,
            workspace.device_output);
    }
    cudaError_t error = cudaGetLastError();
    if (error != cudaSuccess) {
        result.error = "CUDA product GEMM kernel failed:" + CudaErrorString(error);
        return false;
    }

    if (timing_events) {
        cudaEventRecord(workspace.event_gemm_end, workspace.stream);
    }
    const uint32_t hash_blocks = (total_tiles + TILE_HASH_THREADS - 1U) / TILE_HASH_THREADS;
    HashProductTilesKernel<<<hash_blocks, TILE_HASH_THREADS, 0, workspace.stream>>>(
        workspace.device_output,
        n,
        b,
        blocks_per_axis,
        tiles_per_request,
        matrix_elements,
        total_tiles,
        workspace.device_tile_hashes);
    error = cudaGetLastError();
    if (error != cudaSuccess) {
        result.error = "CUDA product tile hash kernel failed:" + CudaErrorString(error);
        return false;
    }
    if (finish_on_device) {
        const uint32_t finalize_blocks = (batch_size + 63U) / 64U;
        FinalizeProductDigestsKernel<<<finalize_blocks, 64U, 0, workspace.stream>>>(
            workspace.device_tile_hashes,
            workspace.device_sigma,
            tiles_per_request,
            batch_size,
            n,
            b,
            workspace.device_digests);
        error = cudaGetLastError();
        if (error != cudaSuccess) {
            result.error = "CUDA product digest finalize kernel failed:" + CudaErrorString(error);
            return false;
        }
    }
    if (timing_events) {
        cudaEventRecord(workspace.event_hash_end, workspace.stream);
    }
    sample.launch_finalize_us = DurationMicros(finalize_start, SteadyClock::now());

    std::string staging_warning;
    if (fetch_tile_hashes && !workspace.host_tile_hashes.Ensure(total_tiles, staging_warning)) {
        result.error = staging_warning;
        return false;
    }

    if (const char* debug_sync = std::getenv("QTC_MATMUL_CUDA_DEBUG_SYNC_BEFORE_D2H"); debug_sync != nullptr && debug_sync[0] == '1') {
        // Diagnostics only: isolate the D2H call time from the pipeline completion.
        const auto pre_sync_start = SteadyClock::now();
        error = cudaStreamSynchronize(workspace.stream);
        sample.submit_d2d_us = DurationMicros(pre_sync_start, SteadyClock::now());
        if (error != cudaSuccess) {
            result.error = "CUDA debug pre-D2H sync failed:" + CudaErrorString(error);
            return false;
        }
    }
    const auto d2h_start = SteadyClock::now();
    error = cudaSuccess;
    if (finish_on_device) {
        error = cudaMemcpyAsync(workspace.host_digests.data(),
                                workspace.device_digests,
                                static_cast<size_t>(batch_size) * sizeof(DeviceDigestBytes),
                                cudaMemcpyDeviceToHost,
                                workspace.stream);
    }
    if (error == cudaSuccess && fetch_tile_hashes) {
        error = cudaMemcpyAsync(workspace.host_tile_hashes.data(),
                                workspace.device_tile_hashes,
                                static_cast<size_t>(total_tiles) * sizeof(DeviceDigestBytes),
                                cudaMemcpyDeviceToHost,
                                workspace.stream);
    }
    sample.submit_d2h_us = DurationMicros(d2h_start, SteadyClock::now());
    if (timing_events) {
        cudaEventRecord(workspace.event_copy_end, workspace.stream);
    }
    {
        // Report the real staging type: a pageable destination turns the D2H
        // copy into a synchronous, slow transfer.
        cudaPointerAttributes attributes{};
        void* staging = finish_on_device
            ? static_cast<void*>(workspace.host_digests.data())
            : static_cast<void*>(workspace.host_tile_hashes.data());
        const cudaError_t attr_error = cudaPointerGetAttributes(&attributes, staging);
        sample.used_pinned_host_staging = attr_error == cudaSuccess && attributes.type == cudaMemoryTypeHost;
        if (attr_error != cudaSuccess) {
            (void)cudaGetLastError();
        }
    }
    const auto sync_start = SteadyClock::now();
    if (error == cudaSuccess) {
        error = cudaStreamSynchronize(workspace.stream);
    }
    sample.stream_sync_us = DurationMicros(sync_start, SteadyClock::now());
    if (error != cudaSuccess) {
        result.error = "CUDA stream completion failed:" + CudaErrorString(error);
        return false;
    }

    if (timing_events) {
        float copy_ms{0.0f};
        if (cudaEventElapsedTime(&copy_ms, workspace.event_hash_end, workspace.event_copy_end) == cudaSuccess) {
            sample.gpu_copy_us = static_cast<double>(copy_ms) * 1000.0;
        }
        float ms{0.0f};
        if (workspace.build_event_recorded &&
            cudaEventElapsedTime(&ms, workspace.event_build_start, workspace.event_gemm_start) == cudaSuccess) {
            sample.gpu_build_us = static_cast<double>(ms) * 1000.0;
        }
        if (cudaEventElapsedTime(&ms, workspace.event_gemm_start, workspace.event_gemm_end) == cudaSuccess) {
            sample.gpu_gemm_us = static_cast<double>(ms) * 1000.0;
        }
        if (cudaEventElapsedTime(&ms, workspace.event_gemm_end, workspace.event_hash_end) == cudaSuccess) {
            sample.gpu_tile_hash_us = static_cast<double>(ms) * 1000.0;
        }
        workspace.build_event_recorded = false;
    }

    result.tiles_per_request = tiles_per_request;
    result.tile_hashes.clear();
    result.digests.clear();
    if (fetch_tile_hashes) {
        result.tile_hashes.reserve(total_tiles);
        const DeviceDigestBytes* host_tiles = workspace.host_tile_hashes.data();
        for (uint32_t i = 0; i < total_tiles; ++i) {
            result.tile_hashes.emplace_back(Span<const unsigned char>{host_tiles[i].data, sizeof(host_tiles[i].data)});
        }
    }
    if (finish_on_device) {
        result.digests.reserve(batch_size);
        const DeviceDigestBytes* host_digests = workspace.host_digests.data();
        for (uint32_t i = 0; i < batch_size; ++i) {
            result.digests.emplace_back(Span<const unsigned char>{host_digests[i].data, sizeof(host_digests[i].data)});
        }
    }
    result.success = true;
    return true;
}

struct DeviceSession {
    CudaRuntimeProbe runtime;
    std::optional<BufferPoolLease> lease;
};

bool OpenDeviceSession(int device_index,
                       DeviceSession& session,
                       MatMulProductTileHashBatchResult& result)
{
    const auto runtime_probe = ResolveCudaRuntimeForSelectedDevice(device_index, result.error);
    result.available = runtime_probe.has_value();
    if (!runtime_probe.has_value()) {
        return false;
    }
    session.runtime = *runtime_probe;

    session.lease = AcquireBufferPoolSlot(session.runtime.device_index, result.error);
    if (!session.lease.has_value()) {
        return false;
    }

    auto& workspace = session.lease->workspace();
    ResetWorkspaceForDevice(workspace, session.runtime.device_index);

    const cudaError_t error = cudaSetDevice(session.runtime.device_index);
    if (error != cudaSuccess) {
        result.error = "cudaSetDevice failed:" + CudaErrorString(error);
        return false;
    }
    return EnsureWorkspaceStream(workspace, result.error);
}

// Merge per-device shard results back into batch order.
template <typename Shard>
bool MergeShardResults(const std::vector<std::pair<Shard, MatMulProductTileHashBatchResult>>& shard_results,
                       uint32_t batch_size,
                       const char* failure_prefix,
                       MatMulProductTileHashBatchResult& result)
{
    for (const auto& [shard, shard_result] : shard_results) {
        if (!shard_result.success) {
            result.available = shard_result.available;
            result.error = "cuda_device_" + std::to_string(shard.device_index) + failure_prefix +
                (shard_result.error.empty() ? "unknown_error" : shard_result.error);
            return false;
        }
        if (result.tiles_per_request == 0) {
            result.tiles_per_request = shard_result.tiles_per_request;
        } else if (result.tiles_per_request != shard_result.tiles_per_request) {
            result.error = "cuda_multi_device_batch_tiles_per_request_mismatch";
            return false;
        }
    }
    if (result.tiles_per_request == 0) {
        result.error = "cuda_multi_device_batch_empty_result";
        return false;
    }

    const bool have_digests = !shard_results.front().second.digests.empty();
    const bool have_tiles = !shard_results.front().second.tile_hashes.empty();
    if (have_digests) {
        result.digests.assign(batch_size, uint256{});
    }
    if (have_tiles) {
        result.tile_hashes.assign(static_cast<size_t>(batch_size) * result.tiles_per_request, uint256{});
    }
    for (const auto& [shard, shard_result] : shard_results) {
        const bool digests_ok = !have_digests || shard_result.digests.size() == shard.indices.size();
        const bool tiles_ok = !have_tiles ||
            shard_result.tile_hashes.size() == shard.indices.size() * result.tiles_per_request;
        if (!digests_ok || !tiles_ok) {
            result.success = false;
            result.error = "cuda_multi_device_batch_result_size_mismatch";
            result.tile_hashes.clear();
            result.digests.clear();
            result.tiles_per_request = 0;
            return false;
        }
        for (size_t i = 0; i < shard.indices.size(); ++i) {
            if (have_digests) {
                result.digests[shard.indices[i]] = shard_result.digests[i];
            }
            if (have_tiles) {
                std::copy(
                    shard_result.tile_hashes.begin() + i * result.tiles_per_request,
                    shard_result.tile_hashes.begin() + (i + 1) * result.tiles_per_request,
                    result.tile_hashes.begin() + shard.indices[i] * result.tiles_per_request);
            }
        }
    }
    result.success = true;
    return true;
}

struct IndexShard {
    int device_index{-1};
    std::vector<size_t> indices;
};

std::vector<IndexShard> PlanContiguousShards(const CudaTopologyProbe& topology, uint32_t batch_size)
{
    std::vector<IndexShard> shards;
    for (const auto& shard : PlanCudaBatchShards(topology.selected_devices, batch_size)) {
        IndexShard entry;
        entry.device_index = shard.device_index;
        entry.indices.reserve(shard.count);
        for (size_t i = 0; i < shard.count; ++i) {
            entry.indices.push_back(shard.start_index + i);
        }
        shards.push_back(std::move(entry));
    }
    return shards;
}

std::vector<IndexShard> PlanShardsByDeviceInputs(const MatMulGeneratedInputsDevice* const* generated_inputs,
                                                 uint32_t batch_size)
{
    std::map<int, IndexShard> shard_map;
    for (uint32_t i = 0; i < batch_size; ++i) {
        auto& shard = shard_map[generated_inputs[i]->device_index];
        shard.device_index = generated_inputs[i]->device_index;
        shard.indices.push_back(i);
    }
    std::vector<IndexShard> shards;
    shards.reserve(shard_map.size());
    for (auto& entry : shard_map) {
        shards.push_back(std::move(entry.second));
    }
    return shards;
}

} // namespace

MatMulAccelerationProbe ProbeMatMulDigestAcceleration()
{
    const auto runtime = ProbeCudaRuntime();
    return MatMulAccelerationProbe{
        .available = runtime.available,
        .reason = runtime.reason,
        .device_name = runtime.device_name,
        .compute_capability_major = runtime.compute_capability_major,
        .compute_capability_minor = runtime.compute_capability_minor,
        .global_memory_bytes = runtime.global_memory_bytes,
        .multiprocessor_count = runtime.multiprocessor_count,
        .driver_api_version = runtime.driver_api_version,
        .runtime_version = runtime.runtime_version,
    };
}

MatMulBufferPoolStats ProbeMatMulBufferPool()
{
    MatMulBufferPoolStats stats;

    const auto topology = ProbeCudaTopology();
    if (!topology.available) {
        stats.available = false;
        stats.initialized = false;
        stats.reason = topology.reason;
        return stats;
    }

    stats.available = true;
    for (const auto& device : topology.selected_devices) {
        auto& context = GetPoolContext(device.device_index);
        std::lock_guard<std::mutex> lock(context.mutex);
        stats.initialized = stats.initialized || context.initialized;
        stats.allocation_events += context.allocation_events;
        stats.reuse_events += context.reuse_events;
        stats.wait_events += context.wait_events;
        stats.completed_submissions += context.completed_submissions;
        stats.slot_count += static_cast<uint32_t>(context.slots.size());
        stats.active_slots += context.active_slots;
        stats.high_water_slots += context.high_water_slots;
        stats.inflight_submissions += context.inflight_submissions;
        stats.peak_inflight_submissions += context.peak_inflight_submissions;
        for (const auto& slot : context.slots) {
            if (slot == nullptr) {
                continue;
            }
            const uint64_t slot_bytes = DeviceCapacityBytes(slot->workspace);
            stats.device_capacity_bytes += slot_bytes;
            stats.max_slot_device_capacity_bytes = std::max(stats.max_slot_device_capacity_bytes, slot_bytes);
            if (slot_bytes > 0) {
                ++stats.slots_with_device_buffers;
            }
            if (slot->in_use) {
                stats.active_device_capacity_bytes += slot_bytes;
            }
        }
        if (context.initialized) {
            stats.n = context.last_n;
            stats.b = context.last_b;
            stats.r = context.last_r;
        }
    }
    stats.reason = stats.initialized ? "buffer_pool_slots_ready" : "buffer_pool_uninitialized";
    return stats;
}

MatMulDispatchConfig ProbeMatMulDispatchConfig()
{
    MatMulDispatchConfig config;

    const auto runtime = ProbeCudaRuntime();
    if (!runtime.available) {
        config.reason = runtime.reason;
        return config;
    }

    config.available = true;
    config.build_perturbed_threads = WORKSPACE_THREADS;
    config.gemm_tile_dim = GEMM_BM;
    config.gemm_threads = GEMM_THREADS;
    config.tile_hash_threads = TILE_HASH_THREADS;
    config.max_supported_block_size = MAX_SUPPORTED_BLOCK_SIZE;
    config.nonblocking_streams = true;
    config.reason = "ready";
    return config;
}

MatMulKernelProfile ProbeMatMulKernelProfile()
{
    MatMulKernelProfile profile;

    const auto runtime = ProbeCudaRuntime();
    if (!runtime.available) {
        profile.reason = runtime.reason;
        return profile;
    }

    const char* device_prepared_env = std::getenv("QTC_MATMUL_CUDA_DEVICE_PREPARED_INPUTS");
    profile.available = true;
    profile.low_rank_perturbation_kernel = true;
    profile.tiled_product_digest_v4 = true;
    profile.pinned_host_staging = true;
    profile.base_matrix_cache = true;
    profile.shared_buffer_pool = true;
    profile.nonblocking_streams = true;
    profile.device_prepared_inputs_supported = true;
    profile.device_prepared_inputs_default = false;
    profile.device_prepared_inputs_enabled = device_prepared_env != nullptr &&
        device_prepared_env[0] != '\0' &&
        device_prepared_env[0] != '0';
    profile.execution_model = "nonblocking_stream_per_device_pool_slot";
    profile.staging_strategy = "pinned_host_with_pageable_fallback";
    profile.device_prepared_inputs_policy = "auto_product_digest_shape_plus_env";
    profile.reason = "ready";
    return profile;
}

MatMulProfilingStats ProbeMatMulProfilingStats()
{
    MatMulProfilingStats stats;

    const auto runtime = ProbeCudaRuntime();
    if (!runtime.available) {
        stats.reason = runtime.reason;
        return stats;
    }

    auto& context = GetProfilingContext();
    std::lock_guard<std::mutex> lock(context.mutex);
    stats.available = true;
    stats.samples = context.samples;
    stats.last_n = context.last_n;
    stats.last_b = context.last_b;
    stats.last_r = context.last_r;
    stats.last_batch_size = context.last_batch_size;
    stats.last_host_stage_us = context.last_host_stage_us;
    stats.last_submit_h2d_us = context.last_submit_h2d_us;
    stats.last_submit_d2d_us = context.last_submit_d2d_us;
    stats.last_stream_wait_event_us = context.last_stream_wait_event_us;
    stats.last_launch_build_perturbed_us = context.last_launch_build_perturbed_us;
    stats.last_launch_finalize_us = context.last_launch_finalize_us;
    stats.last_submit_d2h_us = context.last_submit_d2h_us;
    stats.last_stream_sync_us = context.last_stream_sync_us;
    stats.last_total_wall_ms = context.last_total_wall_ms;
    stats.last_used_low_rank_path = context.last_used_low_rank_path;
    stats.last_used_device_prepared_inputs = context.last_used_device_prepared_inputs;
    stats.last_used_pinned_host_staging = context.last_used_pinned_host_staging;
    stats.last_base_matrix_cache_hit = context.last_base_matrix_cache_hit;
    stats.last_gpu_build_us = context.last_gpu_build_us;
    stats.last_gpu_gemm_us = context.last_gpu_gemm_us;
    stats.last_gpu_tile_hash_us = context.last_gpu_tile_hash_us;
    stats.last_gpu_copy_us = context.last_gpu_copy_us;
    stats.last_mode = context.last_mode;
    stats.reason = context.reason;
    return stats;
}

MatMulProductTileHashBatchResult ComputeProductTileHashesBatch(const MatMulProductTileHashBatchRequest& request)
{
    MatMulProductTileHashBatchResult result;
    DigestProfilingSample sample;
    const auto runtime = ProbeCudaRuntime();
    result.available = runtime.available;
    if (!runtime.available) {
        result.error = runtime.reason;
        return result;
    }
    if (!ValidateProductTileHashBatchRequest(request, result.error)) {
        return result;
    }

    DeviceSession session;
    if (!OpenDeviceSession(runtime.device_index, session, result)) {
        return result;
    }
    auto& workspace = session.lease->workspace();

    sample.n = request.n;
    sample.b = request.b;
    sample.batch_size = request.batch_size;
    sample.mode = "product_tile_hashes_perturbed";
    const uint32_t matrix_elements = request.n * request.n;
    const size_t total_matrix_elements = static_cast<size_t>(request.batch_size) * matrix_elements;
    bool allocated_buffers{false};

    if (!EnsureDeviceBuffer(workspace.device_matrix_a, workspace.matrix_a_capacity, total_matrix_elements, result.error, allocated_buffers) ||
        !EnsureDeviceBuffer(workspace.device_matrix_b, workspace.matrix_b_capacity, total_matrix_elements, result.error, allocated_buffers)) {
        return result;
    }

    std::string staging_warning;
    if (!workspace.host_matrix_a.Ensure(total_matrix_elements, staging_warning) ||
        !workspace.host_matrix_b.Ensure(total_matrix_elements, staging_warning)) {
        result.error = staging_warning;
        return result;
    }
    sample.used_pinned_host_staging = workspace.host_matrix_a.pinned != nullptr &&
        workspace.host_matrix_b.pinned != nullptr;
    const auto total_start = SteadyClock::now();
    const auto host_stage_start = SteadyClock::now();
    for (uint32_t i = 0; i < request.batch_size; ++i) {
        std::memcpy(workspace.host_matrix_a.data() + static_cast<size_t>(i) * matrix_elements,
                    request.matrix_a_perturbed[i],
                    matrix_elements * sizeof(Element));
        std::memcpy(workspace.host_matrix_b.data() + static_cast<size_t>(i) * matrix_elements,
                    request.matrix_b_perturbed[i],
                    matrix_elements * sizeof(Element));
    }
    sample.host_stage_us = DurationMicros(host_stage_start, SteadyClock::now());

    const auto h2d_start = SteadyClock::now();
    cudaError_t error = cudaMemcpyAsync(workspace.device_matrix_a,
                                        workspace.host_matrix_a.data(),
                                        total_matrix_elements * sizeof(Element),
                                        cudaMemcpyHostToDevice,
                                        workspace.stream);
    if (error == cudaSuccess) {
        error = cudaMemcpyAsync(workspace.device_matrix_b,
                                workspace.host_matrix_b.data(),
                                total_matrix_elements * sizeof(Element),
                                cudaMemcpyHostToDevice,
                                workspace.stream);
    }
    sample.submit_h2d_us = DurationMicros(h2d_start, SteadyClock::now());
    if (error != cudaSuccess) {
        result.error = "cudaMemcpy host_to_device failed:" + CudaErrorString(error);
        return result;
    }

    if (!RunProductTileHashPipeline(
            *session.lease, workspace, request.n, request.b, /*r=*/0, request.batch_size,
            request.sigmas, request.return_tile_hashes, result, sample, allocated_buffers)) {
        return result;
    }

    sample.total_wall_ms = DurationMillis(total_start, SteadyClock::now());
    RecordProfilingSample(sample);
    return result;
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatchOnDevice(
    const MatMulLowRankProductBatchRequest& request,
    int device_index)
{
    MatMulProductTileHashBatchResult result;
    DigestProfilingSample sample;
    if (!ValidateLowRankBatchRequest(request, result.error)) {
        result.available = true;
        return result;
    }

    DeviceSession session;
    if (!OpenDeviceSession(device_index, session, result)) {
        return result;
    }
    auto& workspace = session.lease->workspace();
    const auto& runtime = session.runtime;

    sample.n = request.n;
    sample.b = request.b;
    sample.r = request.r;
    sample.batch_size = request.batch_size;
    sample.mode = "product_tile_hashes_low_rank_host_noise";
    sample.used_low_rank_path = true;
    const uint32_t matrix_elements = request.n * request.n;
    const NoiseLayout layout = MakeNoiseLayout(request.n, request.r);
    bool allocated_buffers{false};
    sample.base_matrix_cache_hit = IsBaseMatrixCacheHit(
        workspace, request.matrix_a_cache_key, request.matrix_b_cache_key, matrix_elements);

    const auto total_start = SteadyClock::now();
    if (!EnsureCachedBaseMatrices(
            workspace, runtime, request.matrix_a, request.matrix_b,
            request.matrix_a_cache_key, request.matrix_b_cache_key,
            matrix_elements, result.error, allocated_buffers)) {
        return result;
    }
    if (!StageHostNoise(
            workspace, layout, request.batch_size,
            request.noise_e_l, request.noise_e_r, request.noise_f_l, request.noise_f_r,
            sample, result.error, allocated_buffers)) {
        return result;
    }
    if (!LaunchPerturbFromCachedBase(
            workspace, layout, request.n, request.r, request.batch_size,
            sample, result.error, allocated_buffers)) {
        return result;
    }
    if (!RunProductTileHashPipeline(
            *session.lease, workspace, request.n, request.b, request.r, request.batch_size,
            request.sigmas, request.return_tile_hashes, result, sample, allocated_buffers)) {
        return result;
    }

    sample.total_wall_ms = DurationMillis(total_start, SteadyClock::now());
    RecordProfilingSample(sample);
    return result;
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatch(
    const MatMulLowRankProductBatchRequest& request)
{
    return ComputeProductTileHashesLowRankBatchMultiDevice(request);
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatchMultiDevice(
    const MatMulLowRankProductBatchRequest& request)
{
    MatMulProductTileHashBatchResult result;

    const auto topology = ProbeCudaTopology();
    result.available = topology.available;
    if (!topology.available) {
        result.error = topology.reason;
        return result;
    }
    if (!ValidateLowRankBatchRequest(request, result.error)) {
        return result;
    }

    const auto shards = PlanContiguousShards(topology, request.batch_size);
    if (shards.empty()) {
        result.error = "no_cuda_batch_shards_available";
        return result;
    }
    if (shards.size() == 1) {
        return ComputeProductTileHashesLowRankBatchOnDevice(request, shards.front().device_index);
    }

    std::vector<std::future<std::pair<IndexShard, MatMulProductTileHashBatchResult>>> futures;
    futures.reserve(shards.size());
    for (const auto& shard : shards) {
        futures.push_back(std::async(std::launch::async, [request, shard]() {
            const size_t start = shard.indices.front();
            const MatMulLowRankProductBatchRequest shard_request{
                .n = request.n,
                .b = request.b,
                .r = request.r,
                .batch_size = static_cast<uint32_t>(shard.indices.size()),
                .matrix_a = request.matrix_a,
                .matrix_b = request.matrix_b,
                .matrix_a_cache_key = request.matrix_a_cache_key,
                .matrix_b_cache_key = request.matrix_b_cache_key,
                .noise_e_l = request.noise_e_l + start,
                .noise_e_r = request.noise_e_r + start,
                .noise_f_l = request.noise_f_l + start,
                .noise_f_r = request.noise_f_r + start,
                .sigmas = request.sigmas != nullptr ? request.sigmas + start : nullptr,
                .return_tile_hashes = request.return_tile_hashes,
            };
            return std::make_pair(shard, ComputeProductTileHashesLowRankBatchOnDevice(shard_request, shard.device_index));
        }));
    }

    std::vector<std::pair<IndexShard, MatMulProductTileHashBatchResult>> shard_results;
    shard_results.reserve(futures.size());
    try {
        for (auto& future : futures) {
            shard_results.push_back(future.get());
        }
    } catch (const std::exception& e) {
        result.error = std::string{"cuda_multi_device_batch_exception:"} + e.what();
        return result;
    }
    MergeShardResults(shard_results, request.batch_size, "_batch_failed:", result);
    return result;
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatchOnDevice(
    const MatMulLowRankProductDeviceBatchRequest& request,
    int device_index)
{
    MatMulProductTileHashBatchResult result;
    DigestProfilingSample sample;
    if (!ValidateLowRankDeviceBatchRequest(request, result.error)) {
        result.available = true;
        return result;
    }

    DeviceSession session;
    if (!OpenDeviceSession(device_index, session, result)) {
        return result;
    }
    auto& workspace = session.lease->workspace();
    const auto& runtime = session.runtime;

    sample.n = request.n;
    sample.b = request.b;
    sample.r = request.r;
    sample.batch_size = request.batch_size;
    sample.mode = "product_tile_hashes_low_rank_device_noise";
    sample.used_low_rank_path = true;
    sample.used_device_prepared_inputs = true;
    const uint32_t matrix_elements = request.n * request.n;
    const NoiseLayout layout = MakeNoiseLayout(request.n, request.r);
    bool allocated_buffers{false};
    sample.base_matrix_cache_hit = IsBaseMatrixCacheHit(
        workspace, request.matrix_a_cache_key, request.matrix_b_cache_key, matrix_elements);

    const auto total_start = SteadyClock::now();
    if (!EnsureCachedBaseMatrices(
            workspace, runtime, request.matrix_a, request.matrix_b,
            request.matrix_a_cache_key, request.matrix_b_cache_key,
            matrix_elements, result.error, allocated_buffers)) {
        return result;
    }
    if (!StageDeviceGeneratedInputs(
            workspace, runtime, layout, request.n, request.b, request.r, request.batch_size,
            request.generated_inputs, sample, result.error, allocated_buffers)) {
        return result;
    }
    if (!LaunchPerturbFromCachedBase(
            workspace, layout, request.n, request.r, request.batch_size,
            sample, result.error, allocated_buffers)) {
        return result;
    }
    if (!RunProductTileHashPipeline(
            *session.lease, workspace, request.n, request.b, request.r, request.batch_size,
            request.sigmas, request.return_tile_hashes, result, sample, allocated_buffers)) {
        return result;
    }

    sample.total_wall_ms = DurationMillis(total_start, SteadyClock::now());
    RecordProfilingSample(sample);
    return result;
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatch(
    const MatMulLowRankProductDeviceBatchRequest& request)
{
    return ComputeProductTileHashesLowRankDeviceBatchMultiDevice(request);
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatchMultiDevice(
    const MatMulLowRankProductDeviceBatchRequest& request)
{
    MatMulProductTileHashBatchResult result;

    const auto topology = ProbeCudaTopology();
    result.available = topology.available;
    if (!topology.available) {
        result.error = topology.reason;
        return result;
    }
    if (!ValidateLowRankDeviceBatchRequest(request, result.error)) {
        return result;
    }

    const auto shards = PlanShardsByDeviceInputs(request.generated_inputs, request.batch_size);
    if (shards.size() == 1) {
        return ComputeProductTileHashesLowRankDeviceBatchOnDevice(request, shards.front().device_index);
    }

    std::vector<std::future<std::pair<IndexShard, MatMulProductTileHashBatchResult>>> futures;
    futures.reserve(shards.size());
    for (const auto& shard : shards) {
        futures.push_back(std::async(std::launch::async, [request, shard]() {
            std::vector<const MatMulGeneratedInputsDevice*> inputs;
            std::vector<uint256> sigmas;
            inputs.reserve(shard.indices.size());
            sigmas.reserve(shard.indices.size());
            for (const size_t index : shard.indices) {
                inputs.push_back(request.generated_inputs[index]);
                if (request.sigmas != nullptr) {
                    sigmas.push_back(request.sigmas[index]);
                }
            }
            const MatMulLowRankProductDeviceBatchRequest shard_request{
                .n = request.n,
                .b = request.b,
                .r = request.r,
                .batch_size = static_cast<uint32_t>(inputs.size()),
                .matrix_a = request.matrix_a,
                .matrix_b = request.matrix_b,
                .matrix_a_cache_key = request.matrix_a_cache_key,
                .matrix_b_cache_key = request.matrix_b_cache_key,
                .generated_inputs = inputs.data(),
                .sigmas = request.sigmas != nullptr ? sigmas.data() : nullptr,
                .return_tile_hashes = request.return_tile_hashes,
            };
            return std::make_pair(shard, ComputeProductTileHashesLowRankDeviceBatchOnDevice(shard_request, shard.device_index));
        }));
    }

    std::vector<std::pair<IndexShard, MatMulProductTileHashBatchResult>> shard_results;
    shard_results.reserve(futures.size());
    try {
        for (auto& future : futures) {
            shard_results.push_back(future.get());
        }
    } catch (const std::exception& e) {
        result.error = std::string{"cuda_multi_device_prepared_batch_exception:"} + e.what();
        return result;
    }
    MergeShardResults(shard_results, request.batch_size, "_prepared_batch_failed:", result);
    return result;
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatchOnDevice(
    const MatMulLowRankVariableBaseProductBatchRequest& request,
    int device_index)
{
    MatMulProductTileHashBatchResult result;
    DigestProfilingSample sample;
    if (!ValidateLowRankVariableBaseBatchRequest(request, result.error)) {
        result.available = true;
        return result;
    }

    DeviceSession session;
    if (!OpenDeviceSession(device_index, session, result)) {
        return result;
    }
    auto& workspace = session.lease->workspace();
    const auto& runtime = session.runtime;
    const bool device_inputs = VariableBaseRequestUsesDeviceInputs(request);

    sample.n = request.n;
    sample.b = request.b;
    sample.r = request.r;
    sample.batch_size = request.batch_size;
    sample.mode = device_inputs
        ? "product_tile_hashes_variable_base_device_noise"
        : "product_tile_hashes_variable_base_host_noise";
    sample.used_low_rank_path = true;
    sample.used_device_prepared_inputs = device_inputs;
    const uint32_t matrix_elements = request.n * request.n;
    const size_t total_matrix_elements = static_cast<size_t>(request.batch_size) * matrix_elements;
    const NoiseLayout layout = MakeNoiseLayout(request.n, request.r);
    bool allocated_buffers{false};

    if (!EnsureDeviceBuffer(workspace.device_seed_a, workspace.seed_a_capacity, request.batch_size, result.error, allocated_buffers) ||
        !EnsureDeviceBuffer(workspace.device_seed_b, workspace.seed_b_capacity, request.batch_size, result.error, allocated_buffers) ||
        !EnsureDeviceBuffer(workspace.device_matrix_a, workspace.matrix_a_capacity, total_matrix_elements, result.error, allocated_buffers) ||
        !EnsureDeviceBuffer(workspace.device_matrix_b, workspace.matrix_b_capacity, total_matrix_elements, result.error, allocated_buffers) ||
        !EnsureDeviceBuffer(workspace.device_seed_midstates, workspace.seed_midstates_capacity, static_cast<size_t>(request.batch_size) * 32U, result.error, allocated_buffers)) {
        return result;
    }

    const auto total_start = SteadyClock::now();
    if (device_inputs) {
        if (!StageDeviceGeneratedInputs(
                workspace, runtime, layout, request.n, request.b, request.r, request.batch_size,
                request.generated_inputs, sample, result.error, allocated_buffers)) {
            return result;
        }
    } else if (!StageHostNoise(
                   workspace, layout, request.batch_size,
                   request.noise_e_l, request.noise_e_r, request.noise_f_l, request.noise_f_r,
                   sample, result.error, allocated_buffers)) {
        return result;
    }

    std::string staging_warning;
    if (!workspace.host_seeds.Ensure(static_cast<size_t>(request.batch_size) * 2U, staging_warning)) {
        result.error = staging_warning;
        return result;
    }
    DeviceSeedBytes* host_seeds = workspace.host_seeds.data();
    std::memcpy(host_seeds, request.matrix_a_seeds, static_cast<size_t>(request.batch_size) * sizeof(DeviceSeedBytes));
    std::memcpy(host_seeds + request.batch_size, request.matrix_b_seeds, static_cast<size_t>(request.batch_size) * sizeof(DeviceSeedBytes));
    const auto h2d_start = SteadyClock::now();
    cudaError_t error = cudaMemcpyAsync(workspace.device_seed_a,
                                        host_seeds,
                                        request.batch_size * sizeof(DeviceSeedBytes),
                                        cudaMemcpyHostToDevice,
                                        workspace.stream);
    if (error == cudaSuccess) {
        error = cudaMemcpyAsync(workspace.device_seed_b,
                                host_seeds + request.batch_size,
                                request.batch_size * sizeof(DeviceSeedBytes),
                                cudaMemcpyHostToDevice,
                                workspace.stream);
    }
    sample.submit_h2d_us += DurationMicros(h2d_start, SteadyClock::now());
    if (error != cudaSuccess) {
        result.error = "cudaMemcpy variable-base seeds failed:" + CudaErrorString(error);
        return result;
    }

    const uint32_t blocks_per_matrix = matrix_elements / ORACLE_LANES;
    const size_t total_blocks = static_cast<size_t>(request.batch_size) * blocks_per_matrix;
    const uint32_t build_blocks = static_cast<uint32_t>((total_blocks + WORKSPACE_THREADS - 1) / WORKSPACE_THREADS);
    const uint32_t midstate_blocks = (request.batch_size + WORKSPACE_THREADS - 1U) / WORKSPACE_THREADS;
    const auto build_start = SteadyClock::now();
    workspace.MarkBuildStart();
    PrecomputeSeedPairMidstatesKernel<<<midstate_blocks, WORKSPACE_THREADS, 0, workspace.stream>>>(
        workspace.device_seed_a,
        workspace.device_seed_b,
        request.batch_size,
        workspace.device_seed_midstates);
    error = cudaGetLastError();
    if (error == cudaSuccess) {
        GeneratePerturbedMatrixPairFromSeedMidstateKernel<<<build_blocks, WORKSPACE_THREADS, 0, workspace.stream>>>(
            workspace.device_seed_midstates,
            workspace.device_seed_midstates + static_cast<size_t>(request.batch_size) * 16U,
            workspace.device_prepared_input_ptrs,
            layout.e_l_offset,
            layout.e_r_offset,
            layout.f_l_offset,
            layout.f_r_offset,
            request.n,
            request.r,
            blocks_per_matrix,
            total_blocks,
            workspace.device_matrix_a,
            workspace.device_matrix_b);
        error = cudaGetLastError();
    }
    sample.launch_build_perturbed_us = DurationMicros(build_start, SteadyClock::now());
    if (error != cudaSuccess) {
        result.error = "CUDA variable-base matrix kernel failed:" + CudaErrorString(error);
        return result;
    }

    if (!RunProductTileHashPipeline(
            *session.lease, workspace, request.n, request.b, request.r, request.batch_size,
            request.sigmas, request.return_tile_hashes, result, sample, allocated_buffers)) {
        return result;
    }

    sample.total_wall_ms = DurationMillis(total_start, SteadyClock::now());
    RecordProfilingSample(sample);
    return result;
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatch(
    const MatMulLowRankVariableBaseProductBatchRequest& request)
{
    return ComputeProductTileHashesLowRankVariableBaseBatchMultiDevice(request);
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatchMultiDevice(
    const MatMulLowRankVariableBaseProductBatchRequest& request)
{
    MatMulProductTileHashBatchResult result;

    const auto topology = ProbeCudaTopology();
    result.available = topology.available;
    if (!topology.available) {
        result.error = topology.reason;
        return result;
    }
    if (!ValidateLowRankVariableBaseBatchRequest(request, result.error)) {
        return result;
    }

    const bool device_inputs = VariableBaseRequestUsesDeviceInputs(request);
    const auto shards = device_inputs
        ? PlanShardsByDeviceInputs(request.generated_inputs, request.batch_size)
        : PlanContiguousShards(topology, request.batch_size);
    if (shards.empty()) {
        result.error = "no_cuda_batch_shards_available";
        return result;
    }
    if (shards.size() == 1) {
        return ComputeProductTileHashesLowRankVariableBaseBatchOnDevice(request, shards.front().device_index);
    }

    std::vector<std::future<std::pair<IndexShard, MatMulProductTileHashBatchResult>>> futures;
    futures.reserve(shards.size());
    for (const auto& shard : shards) {
        futures.push_back(std::async(std::launch::async, [request, shard, device_inputs]() {
            std::vector<uint256> seed_a;
            std::vector<uint256> seed_b;
            std::vector<uint256> sigmas;
            std::vector<const MatMulGeneratedInputsDevice*> inputs;
            std::vector<const Element*> e_l;
            std::vector<const Element*> e_r;
            std::vector<const Element*> f_l;
            std::vector<const Element*> f_r;
            seed_a.reserve(shard.indices.size());
            seed_b.reserve(shard.indices.size());
            sigmas.reserve(shard.indices.size());
            for (const size_t index : shard.indices) {
                seed_a.push_back(request.matrix_a_seeds[index]);
                seed_b.push_back(request.matrix_b_seeds[index]);
                if (request.sigmas != nullptr) {
                    sigmas.push_back(request.sigmas[index]);
                }
                if (device_inputs) {
                    inputs.push_back(request.generated_inputs[index]);
                } else {
                    e_l.push_back(request.noise_e_l[index]);
                    e_r.push_back(request.noise_e_r[index]);
                    f_l.push_back(request.noise_f_l[index]);
                    f_r.push_back(request.noise_f_r[index]);
                }
            }
            const MatMulLowRankVariableBaseProductBatchRequest shard_request{
                .n = request.n,
                .b = request.b,
                .r = request.r,
                .batch_size = static_cast<uint32_t>(shard.indices.size()),
                .matrix_a_seeds = seed_a.data(),
                .matrix_b_seeds = seed_b.data(),
                .generated_inputs = device_inputs ? inputs.data() : nullptr,
                .noise_e_l = device_inputs ? nullptr : e_l.data(),
                .noise_e_r = device_inputs ? nullptr : e_r.data(),
                .noise_f_l = device_inputs ? nullptr : f_l.data(),
                .noise_f_r = device_inputs ? nullptr : f_r.data(),
                .sigmas = request.sigmas != nullptr ? sigmas.data() : nullptr,
                .return_tile_hashes = request.return_tile_hashes,
            };
            return std::make_pair(
                shard,
                ComputeProductTileHashesLowRankVariableBaseBatchOnDevice(shard_request, shard.device_index));
        }));
    }

    std::vector<std::pair<IndexShard, MatMulProductTileHashBatchResult>> shard_results;
    shard_results.reserve(futures.size());
    try {
        for (auto& future : futures) {
            shard_results.push_back(future.get());
        }
    } catch (const std::exception& e) {
        result.error = std::string{"cuda_multi_device_variable_base_batch_exception:"} + e.what();
        return result;
    }
    MergeShardResults(shard_results, request.batch_size, "_variable_base_batch_failed:", result);
    return result;
}

} // namespace qtc::cuda
