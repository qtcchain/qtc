// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QTC_TEST_SHIELDED_MATRICT_RUNTIME_REPORT_H
#define QTC_TEST_SHIELDED_MATRICT_RUNTIME_REPORT_H

#include <univalue.h>

#include <cstddef>

namespace qtc::test::matrictplus {

struct RuntimeReportConfig
{
    size_t warmup_iterations{0};
    size_t measured_iterations{1};
};

UniValue BuildRuntimeReport(const RuntimeReportConfig& config);

} // namespace qtc::test::matrictplus

#endif // QTC_TEST_SHIELDED_MATRICT_RUNTIME_REPORT_H
