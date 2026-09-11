// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <cuda/cuda_context.h>
#include <cuda/matmul_accel.h>

namespace qtc::cuda {

void ResolveSelectedCudaDevices(CudaTopologyProbe& topology, const std::string&)
{
    topology.available = false;
    topology.reason = "disabled_by_build";
    topology.selected_devices.clear();
}

CudaTopologyProbe ProbeCudaTopology()
{
    CudaTopologyProbe probe;
    probe.reason = "disabled_by_build";
    return probe;
}

CudaRuntimeProbe ProbeCudaRuntime()
{
    CudaRuntimeProbe probe;
    probe.reason = "disabled_by_build";
    return probe;
}

MatMulAccelerationProbe ProbeMatMulDigestAcceleration()
{
    MatMulAccelerationProbe probe;
    probe.reason = "disabled_by_build";
    return probe;
}

MatMulBufferPoolStats ProbeMatMulBufferPool()
{
    MatMulBufferPoolStats stats;
    stats.reason = "disabled_by_build";
    return stats;
}

MatMulDispatchConfig ProbeMatMulDispatchConfig()
{
    MatMulDispatchConfig config;
    config.reason = "disabled_by_build";
    return config;
}

MatMulKernelProfile ProbeMatMulKernelProfile()
{
    MatMulKernelProfile profile;
    profile.reason = "disabled_by_build";
    return profile;
}

MatMulProfilingStats ProbeMatMulProfilingStats()
{
    MatMulProfilingStats stats;
    stats.reason = "disabled_by_build";
    return stats;
}

namespace {

MatMulProductTileHashBatchResult DisabledResult()
{
    MatMulProductTileHashBatchResult result;
    result.error = "disabled_by_build";
    return result;
}

} // namespace

MatMulProductTileHashBatchResult ComputeProductTileHashesBatch(const MatMulProductTileHashBatchRequest&)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatch(const MatMulLowRankProductBatchRequest&)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatchOnDevice(const MatMulLowRankProductBatchRequest&, int)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatchMultiDevice(const MatMulLowRankProductBatchRequest&)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatch(const MatMulLowRankProductDeviceBatchRequest&)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatchOnDevice(const MatMulLowRankProductDeviceBatchRequest&, int)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatchMultiDevice(const MatMulLowRankProductDeviceBatchRequest&)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatch(const MatMulLowRankVariableBaseProductBatchRequest&)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatchOnDevice(const MatMulLowRankVariableBaseProductBatchRequest&, int)
{
    return DisabledResult();
}

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatchMultiDevice(const MatMulLowRankVariableBaseProductBatchRequest&)
{
    return DisabledResult();
}

} // namespace qtc::cuda
