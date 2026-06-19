#pragma once

#include <string>
#include <vector>

namespace yir_analysis {

struct LiveRange {
    int start_block;
    int end_block;
};

struct PipelineStageInfo {
    std::string stage_name;
    int total_live_ranges;
    int total_blocks;
    std::vector<int> live_in_counts_per_block;
};

struct StageDiff {
    std::string stage_from;
    std::string stage_to;
    std::string function_name;
    int blocks_added;
    int blocks_removed;
    int live_ranges_shortened;
    int live_ranges_lengthened;
    int live_ranges_added;
    int live_ranges_removed;
    std::vector<std::string> diagnostics;
};

std::vector<StageDiff> compute_pipeline_diffs(
    const std::vector<PipelineStageInfo> &stages);

std::string diffs_to_report(const std::vector<StageDiff> &diffs);

} // namespace yir_analysis
