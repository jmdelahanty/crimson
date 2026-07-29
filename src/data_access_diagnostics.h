#pragma once

#include "data_access_scheduler.h"

#include <iosfwd>
#include <string_view>

namespace crimson::data {

void writeDataAccessSchedulerDiagnostics(
    std::ostream &output, std::string_view backend,
    const DataAccessSchedulerMetrics &metrics);

} // namespace crimson::data
