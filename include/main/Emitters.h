#pragma once

#include "ast/ast.h"
#include "main/CliOptions.h"
#include "pass/PassManager.h"

#include <iosfwd>
#include <memory>
#include <string>

namespace driver {

std::unique_ptr<CompUnit> parse_ast_from_file(const std::string &input_path, std::ostream &err);
int run_pipeline(pass::PassManager &pm, pass::PassContext &context, std::ostream &err);
bool emit_requested_outputs(const CliOptions &options, pass::PassContext &context,
                            std::ostream &out, std::ostream &err);

} // namespace driver
