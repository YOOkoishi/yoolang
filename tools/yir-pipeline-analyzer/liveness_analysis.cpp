#include "liveness_analysis.h"

#include <algorithm>
#include <ostream>
#include <sstream>
#include <unordered_set>

#include "yir/YIR.h"

namespace yir_analysis {

namespace {

std::string value_id(const yir::Value *value) {
    if (value == nullptr) {
        return "null";
    }
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

std::string op_name_or_empty(const yir::Operation *op) {
    if (op == nullptr) {
        return "";
    }
    return op->op_name();
}

struct BasicBlock {
    std::string label;
    std::vector<const yir::Operation *> operations;
    std::vector<const yir::Value *> defs;
    std::vector<const yir::Value *> uses;
    std::vector<const yir::Value *> live_in;
    std::vector<const yir::Value *> live_out;
};

class CFGBuilder {
  public:
    explicit CFGBuilder(const yir::Function &func) : func_(func) {
    }

    std::vector<BasicBlock> build() {
        blocks_.clear();
        current_label_ = "entry";
        start_block();
        walk_region(func_.body());
        return std::move(blocks_);
    }

  private:
    void start_block() {
        BasicBlock block;
        block.label = current_label_;
        blocks_.push_back(std::move(block));
    }

    BasicBlock &current_block() {
        return blocks_.back();
    }

    void walk_region(const yir::Region &region) {
        for (const auto &op : region.operations()) {
            process_op(*op);
        }
    }

    void process_op(const yir::Operation &op) {
        const auto &op_name = op.op_name();

        if (op_name == "yir.while") {
            // While regions are treated as separate blocks
            const auto *while_op = dynamic_cast<const yir::WhileOp *>(&op);
            if (while_op == nullptr) {
                return;
            }

            std::string cond_label = current_label_ + ".while.cond";
            std::string body_label = current_label_ + ".while.body";
            std::string after_label = current_label_ + ".while.after";

            // Current block ends before while
            // Enters cond block
            current_label_ = cond_label;
            start_block();
            walk_region(while_op->cond_region());

            // Body block
            current_label_ = body_label;
            start_block();
            walk_region(while_op->body_region());

            // After block
            current_label_ = after_label;
            start_block();
        } else if (op_name == "yir.if") {
            const auto *if_op = dynamic_cast<const yir::IfOp *>(&op);
            if (if_op == nullptr) {
                return;
            }

            collect_use(if_op->condition());

            std::string then_label = current_label_ + ".if.then";
            std::string else_label = current_label_ + ".if.else";
            std::string after_label = current_label_ + ".if.after";

            auto host_label = current_label_;

            current_label_ = then_label;
            start_block();
            walk_region(if_op->then_region());

            if (if_op->has_else()) {
                current_label_ = else_label;
                start_block();
                walk_region(if_op->else_region());
            } else {
                // implicit else: jump straight to after
                current_label_ = else_label;
                start_block();
            }

            current_label_ = after_label;
            start_block();
        } else if (op_name == "yir.for") {
            const auto *for_op = dynamic_cast<const yir::ForOp *>(&op);
            if (for_op == nullptr) {
                return;
            }

            // Def: induction var, lower bound, upper bound, step
            if (for_op->induction_var() != nullptr) {
                current_block().defs.push_back(for_op->induction_var());
            }
            collect_use(for_op->lower_bound());
            collect_use(for_op->upper_bound());
            collect_use(for_op->step());

            std::string body_label = current_label_ + ".for.body";
            std::string after_label = current_label_ + ".for.after";

            current_label_ = body_label;
            start_block();
            walk_region(for_op->body_region());

            current_label_ = after_label;
            start_block();
        } else {
            // Regular operation
            current_block().operations.push_back(&op);

            // Track defs and uses
            if (op.result() != nullptr) {
                current_block().defs.push_back(op.result());
            }
            for (const auto *operand : op.operands()) {
                if (operand != nullptr) {
                    current_block().uses.push_back(operand);
                }
            }
        }
    }

    void collect_use(const yir::Value *value) {
        if (value != nullptr) {
            current_block().uses.push_back(value);
        }
    }

    const yir::Function &func_;
    std::vector<BasicBlock> blocks_;
    std::string current_label_ = "entry";
};

} // namespace

ModuleLiveness compute_yir_liveness(const yir::Module &module) {
    ModuleLiveness result;

    for (const auto &func : module.functions()) {
        CFGBuilder builder(*func);
        auto blocks = builder.build();

        if (blocks.empty()) {
            continue;
        }

        // Backward dataflow: compute live_in / live_out
        // Initialize: live_out = {}, live_in = {}
        // Iterate until fixpoint:
        //   live_out[B] = Union_{S in successors(B)} live_in[S]
        //   live_in[B] = uses[B] Union (live_out[B] - defs[B])

        std::size_t num_blocks = blocks.size();

        // Live sets: map block index to set of value pointers
        std::vector<std::unordered_set<const yir::Value *>> live_in_set(num_blocks);
        std::vector<std::unordered_set<const yir::Value *>> live_out_set(num_blocks);

        // Successor relation (simplified: linear fallthrough + loop back edges)
        // For now: linear CFG (block i -> block i+1)
        // This is a conservative approximation — we assume all blocks are
        // sequentially connected (like a basic CFG without branch analysis)

        bool changed = true;
        int max_iter = 100;
        while (changed && max_iter-- > 0) {
            changed = false;

            for (int i = static_cast<int>(num_blocks) - 1; i >= 0; --i) {
                auto &out = live_out_set[i];
                auto old_out = out;

                // live_out[B] = Union_{successors} live_in[successor]
                out.clear();
                // Simple linear CFG: successor is block i+1 if it exists
                if (i + 1 < static_cast<int>(num_blocks)) {
                    for (const auto *v : live_in_set[i + 1]) {
                        out.insert(v);
                    }
                }

                if (out != old_out) {
                    changed = true;
                }

                // live_in[B] = uses[B] Union (live_out[B] - defs[B])
                auto &in = live_in_set[i];
                auto old_in = in;
                in = out;

                // Remove defs
                for (const auto *v : blocks[i].defs) {
                    in.erase(v);
                }

                // Add uses
                for (const auto *v : blocks[i].uses) {
                    in.insert(v);
                }

                if (in != old_in) {
                    changed = true;
                }
            }
        }

        // Convert to sorted vectors for deterministic output
        FunctionLiveness func_liveness;
        func_liveness.function_name = func->name();

        for (std::size_t i = 0; i < num_blocks; ++i) {
            BlockLiveness bl;
            bl.block_label = blocks[i].label;
            bl.defs = blocks[i].defs;
            bl.uses = blocks[i].uses;

            // Sort by pointer address for determinism
            for (const auto *v : live_in_set[i]) {
                bl.live_in.push_back(v);
            }
            std::sort(bl.live_in.begin(), bl.live_in.end());
            for (const auto *v : live_out_set[i]) {
                bl.live_out.push_back(v);
            }
            std::sort(bl.live_out.begin(), bl.live_out.end());

            func_liveness.blocks.push_back(std::move(bl));
        }

        result.functions.push_back(std::move(func_liveness));
    }

    return result;
}

std::string liveness_to_json(const ModuleLiveness &liveness) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"functions\": [\n";

    for (std::size_t fi = 0; fi < liveness.functions.size(); ++fi) {
        const auto &func = liveness.functions[fi];
        oss << "    {\n";
        oss << "      \"name\": \"" << func.function_name << "\",\n";
        oss << "      \"blocks\": [\n";

        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            const auto &block = func.blocks[bi];
            oss << "        {\n";
            oss << "          \"label\": \"" << block.block_label << "\",\n";

            // live_in
            oss << "          \"live_in_count\": " << block.live_in.size() << ",\n";
            oss << "          \"live_in\": [";
            for (std::size_t i = 0; i < block.live_in.size(); ++i) {
                if (i != 0) {
                    oss << ", ";
                }
                oss << "\"" << value_id(block.live_in[i]) << "\"";
            }
            oss << "],\n";

            // live_out
            oss << "          \"live_out_count\": " << block.live_out.size() << ",\n";
            oss << "          \"live_out\": [";
            for (std::size_t i = 0; i < block.live_out.size(); ++i) {
                if (i != 0) {
                    oss << ", ";
                }
                oss << "\"" << value_id(block.live_out[i]) << "\"";
            }
            oss << "]\n";

            oss << "        }";
            if (bi + 1 < func.blocks.size()) {
                oss << ",";
            }
            oss << "\n";
        }

        oss << "      ]\n";
        oss << "    }";
        if (fi + 1 < liveness.functions.size()) {
            oss << ",";
        }
        oss << "\n";
    }

    oss << "  ]\n";
    oss << "}\n";

    return oss.str();
}

} // namespace yir_analysis
