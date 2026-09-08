// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QTC_TEST_SHIELDED_SCAN_HINT_RUNTIME_REPORT_H
#define QTC_TEST_SHIELDED_SCAN_HINT_RUNTIME_REPORT_H

#include <univalue.h>

#include <cstddef>

namespace qtc::test::shieldedv2scan {

struct RuntimeReportConfig
{
    size_t warmup_iterations{0};
    size_t measured_iterations{1};
    size_t minimum_candidate_keys{256};
};

UniValue BuildRuntimeReport(const RuntimeReportConfig& config);

} // namespace qtc::test::shieldedv2scan

#endif // QTC_TEST_SHIELDED_SCAN_HINT_RUNTIME_REPORT_H
