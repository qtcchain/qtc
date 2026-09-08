// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_CUDA_CUDA_SCHEDULER_H
#define BITCOIN_CUDA_CUDA_SCHEDULER_H

#include <cuda/cuda_context.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qtc::cuda {

struct CudaBatchShard {
    int device_index{-1};
    size_t start_index{0};
    size_t count{0};
};

uint32_t ExpandCudaBatchSizeForSelectedDevices(uint32_t batch_size,
                                               size_t selected_device_count);
std::vector<CudaBatchShard> PlanCudaBatchShards(const std::vector<CudaDeviceInfo>& devices,
                                                size_t batch_size);

} // namespace qtc::cuda

#endif // BITCOIN_CUDA_CUDA_SCHEDULER_H
