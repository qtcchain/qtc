// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_CUDA_CUDA_CONTEXT_H
#define BITCOIN_CUDA_CUDA_CONTEXT_H

#include <cstdint>
#include <string>
#include <vector>

namespace qtc::cuda {

struct CudaDeviceInfo {
    int device_index{-1};
    bool supported{false};
    std::string reason;
    std::string device_name;
    uint32_t compute_capability_major{0};
    uint32_t compute_capability_minor{0};
    uint64_t global_memory_bytes{0};
    uint32_t multiprocessor_count{0};
    uint32_t clock_rate_khz{0};
    uint32_t memory_clock_rate_khz{0};
    uint32_t memory_bus_width_bits{0};
    uint32_t pci_domain_id{0};
    uint32_t pci_bus_id{0};
    uint32_t pci_device_id{0};
};

struct CudaTopologyProbe {
    bool compiled{false};
    bool available{false};
    std::string reason;
    uint32_t driver_api_version{0};
    uint32_t runtime_version{0};
    std::vector<CudaDeviceInfo> visible_devices;
    std::vector<CudaDeviceInfo> selected_devices;
};

struct CudaRuntimeProbe {
    bool compiled{false};
    bool available{false};
    std::string reason;
    int device_index{-1};
    std::string device_name;
    uint32_t compute_capability_major{0};
    uint32_t compute_capability_minor{0};
    uint64_t global_memory_bytes{0};
    uint32_t multiprocessor_count{0};
    uint32_t driver_api_version{0};
    uint32_t runtime_version{0};
};

void ResolveSelectedCudaDevices(CudaTopologyProbe& topology, const std::string& device_selection);
CudaTopologyProbe ProbeCudaTopology();
CudaRuntimeProbe ProbeCudaRuntime();

} // namespace qtc::cuda

#endif // BITCOIN_CUDA_CUDA_CONTEXT_H
