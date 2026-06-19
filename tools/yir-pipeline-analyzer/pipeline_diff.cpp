#include "pipeline_diff.h"

#include <ostream>
#include <sstream>

namespace yir_analysis {

std::vector<StageDiff> compute_pipeline_diffs(
    const std::vector<PipelineStageInfo> &stages) {
    std::vector<StageDiff> diffs;

    if (stages.size() < 2) {
        return diffs;
    }

    for (std::size_t i = 1; i < stages.size(); ++i) {
        const auto &from = stages[i - 1];
        const auto &to = stages[i];

        StageDiff diff;
        diff.stage_from = from.stage_name;
        diff.stage_to = to.stage_name;
        diff.function_name = "main";
        diff.blocks_added = 0;
        diff.blocks_removed = 0;
        diff.live_ranges_shortened = 0;
        diff.live_ranges_lengthened = 0;
        diff.live_ranges_added = 0;
        diff.live_ranges_removed = 0;

        // Track block count changes
        if (to.total_blocks > from.total_blocks) {
            diff.blocks_added = to.total_blocks - from.total_blocks;
        } else if (from.total_blocks > to.total_blocks) {
            diff.blocks_removed = from.total_blocks - to.total_blocks;
        }

        // Track live range count changes
        int range_diff = to.total_live_ranges - from.total_live_ranges;
        if (range_diff > 0) {
            diff.live_ranges_added = range_diff;
            diff.diagnostics.push_back(
                "+" + std::to_string(range_diff) + " live ranges appeared after " +
                diff.stage_to);
        } else if (range_diff < 0) {
            diff.live_ranges_removed = -range_diff;
            diff.diagnostics.push_back(
                std::to_string(-range_diff) + " live ranges eliminated by " +
                diff.stage_to);
        }

        // Track per-block live-in count changes (approximation of range lengthening)
        std::size_t max_blocks = std::min(from.live_in_counts_per_block.size(),
                                          to.live_in_counts_per_block.size());
        for (std::size_t bi = 0; bi < max_blocks; ++bi) {
            int delta = to.live_in_counts_per_block[bi] -
                       from.live_in_counts_per_block[bi];
            if (delta > 0) {
                diff.live_ranges_lengthened += delta;
            } else if (delta < 0) {
                diff.live_ranges_shortened += (-delta);
            }
        }

        // High-level diagnostics
        if (diff.live_ranges_lengthened > diff.live_ranges_shortened) {
            diff.diagnostics.push_back(
                "⚠  Live ranges are expanding across pipeline stages (net +" +
                std::to_string(diff.live_ranges_lengthened - diff.live_ranges_shortened) +
                "). Pass conflict may be present.");
        }

        if (diff.blocks_added > 0) {
            diff.diagnostics.push_back(
                std::to_string(diff.blocks_added) + " blocks added — CFG structure changed");
        }

        diffs.push_back(std::move(diff));
    }

    return diffs;
}

std::string diffs_to_report(const std::vector<StageDiff> &diffs) {
    std::ostringstream oss;

    if (diffs.empty()) {
        oss << "No pipeline stage diffs available.\n";
        return oss.str();
    }

    oss << "Pipeline Conflict Detection Report\n";
    oss << "==================================\n\n";

    for (const auto &diff : diffs) {
        oss << diff.stage_from << " → " << diff.stage_to << ":\n";
        oss << "  Live ranges: " << diff.live_ranges_added << " added, "
            << diff.live_ranges_removed << " removed, "
            << diff.live_ranges_shortened << " shortened, "
            << diff.live_ranges_lengthened << " lengthened\n";
        oss << "  CFG blocks: " << diff.blocks_added << " added, "
            << diff.blocks_removed << " removed\n";

        if (!diff.diagnostics.empty()) {
            for (const auto &diag : diff.diagnostics) {
                oss << "  " << diag << "\n";
            }
        }
        oss << "\n";
    }

    return oss.str();
}

} // namespace yir_analysis
