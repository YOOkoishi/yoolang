#pragma once

#include "pass/CostModel.h"

#include <iosfwd>
#include <string>

namespace driver {

struct CliOptions {
    std::string input_path;
    std::string output_path;
    int opt_level = 0;
    bool emit_ast = false;
    bool emit_yir = false;
    bool emit_oir = false;
    bool emit_mir = false;
    bool emit_mir_metrics = false;
    bool emit_cost_model = false;
    bool emit_cost_model_json = false;
    bool emit_asm = false;
    bool emit_poly = false;
    bool enable_polyhedral = true;
    bool force_polyhedral = false;
    bool show_help = false;
    std::string emit_mir_stage;
    std::string cost_model_filter;
    pass::cost_model::CostModelPolicyKind cost_model_policy =
        pass::cost_model::CostModelPolicyKind::Aggressive;
};

bool parse_command_line(int argc, char **argv, CliOptions &options, std::string &error);
void print_help(const char *program, std::ostream &out);

} // namespace driver
