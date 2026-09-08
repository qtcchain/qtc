// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QTC_TEST_SHIELDED_V2_NETTING_CAPACITY_REPORT_H
#define QTC_TEST_SHIELDED_V2_NETTING_CAPACITY_REPORT_H

#include <consensus/amount.h>
#include <univalue.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qtc::test::shieldedv2netting {

struct RuntimeScenarioConfig
{
    size_t domain_count{2};
    uint64_t pairwise_cancellation_bps{5000};
};

struct RuntimeReportConfig
{
    size_t warmup_iterations{0};
    size_t measured_iterations{1};
    CAmount pair_gross_flow_sat{COIN};
    uint32_t settlement_window{144};
    std::vector<RuntimeScenarioConfig> scenarios;
};

UniValue BuildRuntimeReport(const RuntimeReportConfig& config);

} // namespace qtc::test::shieldedv2netting

#endif // QTC_TEST_SHIELDED_V2_NETTING_CAPACITY_REPORT_H
