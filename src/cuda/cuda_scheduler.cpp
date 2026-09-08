// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <cuda/cuda_scheduler.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace qtc::cuda {
namespace {

struct WeightedDevice {
    CudaDeviceInfo device;
    uint64_t weight{1};
};

std::string TrimAscii(const std::string& value)
{
    size_t begin{0};
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\n' || value[begin] == '\r')) {
        ++begin;
    }

    size_t end{value.size()};
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\n' || value[end - 1] == '\r')) {
        --end;
    }

    return value.substr(begin, end - begin);
}

bool ParseUInt64(const std::string& value, uint64_t& parsed, bool allow_zero)
{
    const std::string trimmed = TrimAscii(value);
    if (trimmed.empty()) {
        return false;
    }

    try {
        size_t consumed{0};
        const unsigned long long result = std::stoull(trimmed, &consumed, 10);
        if (consumed != trimmed.size() || (!allow_zero && result == 0)) {
            return false;
        }
        parsed = static_cast<uint64_t>(result);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::map<int, uint64_t> ParseDeviceWeightsOverride()
{
    const char* env = std::getenv("QTC_MATMUL_CUDA_DEVICE_WEIGHTS");
    if (env == nullptr || *env == '\0') {
        return {};
    }

    std::map<int, uint64_t> weights;
    std::string value{env};
    size_t begin{0};
    while (begin <= value.size()) {
        const size_t comma = value.find(',', begin);
        const std::string token = TrimAscii(value.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin));
        const size_t colon = token.find(':');
        if (colon == std::string::npos) {
            return {};
        }

        uint64_t device_index{0};
        uint64_t weight{0};
        if (!ParseUInt64(token.substr(0, colon), device_index, /*allow_zero=*/true) ||
            !ParseUInt64(token.substr(colon + 1), weight, /*allow_zero=*/false) ||
            device_index > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return {};
        }
        weights[static_cast<int>(device_index)] = weight;

        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }

    return weights;
}

uint64_t DefaultDeviceWeight(const CudaDeviceInfo& device)
{
    const uint64_t sm_count = std::max<uint64_t>(device.multiprocessor_count, 1);
    if (device.clock_rate_khz == 0) {
        return sm_count;
    }
    return sm_count * device.clock_rate_khz;
}

} // namespace

uint32_t ExpandCudaBatchSizeForSelectedDevices(uint32_t batch_size,
                                               size_t selected_device_count)
{
    if (selected_device_count == 0) {
        return batch_size;
    }
    const uint32_t selected_count = selected_device_count > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(selected_device_count);
    return std::max(batch_size, selected_count);
}

std::vector<CudaBatchShard> PlanCudaBatchShards(const std::vector<CudaDeviceInfo>& devices,
                                                size_t batch_size)
{
    std::vector<CudaBatchShard> shards;
    if (batch_size == 0 || devices.empty()) {
        return shards;
    }

    const auto weights_override = ParseDeviceWeightsOverride();
    std::vector<WeightedDevice> weighted_devices;
    weighted_devices.reserve(devices.size());
    for (const auto& device : devices) {
        if (!device.supported) {
            continue;
        }
        const auto weight_override = weights_override.find(device.device_index);
        weighted_devices.push_back({
            .device = device,
            .weight = weight_override != weights_override.end()
                ? weight_override->second
                : DefaultDeviceWeight(device),
        });
    }

    if (weighted_devices.empty()) {
        return shards;
    }

    std::stable_sort(weighted_devices.begin(), weighted_devices.end(), [](const WeightedDevice& a, const WeightedDevice& b) {
        return a.weight > b.weight;
    });

    const size_t active_device_count = std::min(batch_size, weighted_devices.size());
    weighted_devices.resize(active_device_count);

    std::vector<size_t> counts(active_device_count, 1);
    size_t remaining = batch_size - active_device_count;
    if (remaining > 0) {
        const uint64_t total_weight = std::accumulate(
            weighted_devices.begin(),
            weighted_devices.end(),
            uint64_t{0},
            [](uint64_t sum, const WeightedDevice& device) { return sum + device.weight; });

        struct Remainder {
            size_t index{0};
            uint64_t value{0};
        };
        std::vector<Remainder> remainders;
        remainders.reserve(active_device_count);

        size_t assigned_extra{0};
        for (size_t i = 0; i < active_device_count; ++i) {
            const uint64_t weighted_remaining = static_cast<uint64_t>(remaining) * weighted_devices[i].weight;
            const size_t extra = static_cast<size_t>(weighted_remaining / total_weight);
            counts[i] += extra;
            assigned_extra += extra;
            remainders.push_back({
                .index = i,
                .value = weighted_remaining % total_weight,
            });
        }

        std::stable_sort(remainders.begin(), remainders.end(), [](const Remainder& a, const Remainder& b) {
            return a.value > b.value;
        });

        for (size_t i = 0; assigned_extra < remaining; ++i, ++assigned_extra) {
            ++counts[remainders[i % remainders.size()].index];
        }
    }

    shards.reserve(active_device_count);
    size_t start{0};
    for (size_t i = 0; i < active_device_count; ++i) {
        if (counts[i] == 0) {
            continue;
        }
        shards.push_back({
            .device_index = weighted_devices[i].device.device_index,
            .start_index = start,
            .count = counts[i],
        });
        start += counts[i];
    }

    return shards;
}

} // namespace qtc::cuda
