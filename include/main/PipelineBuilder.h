#pragma once

#include "main/CliOptions.h"
#include "pass/PassManager.h"

#include <iosfwd>

namespace driver {

pass::PassManager build_compilation_pipeline(const CliOptions &options, std::ostream &out);
void initialize_target_machine(pass::PassContext &context, const CliOptions &options);
void initialize_cost_model_report(pass::PassContext &context, const CliOptions &options);

} // namespace driver
