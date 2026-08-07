#include "pass/oir/OIRVectorCleanupPass.h"

#include "oir/OIR.h"

#include <exception>
#include <string>

namespace pass {
namespace {

bool is_vector_typed(const oir::Instruction &instruction) {
    return instruction.type() != nullptr && instruction.type()->is_vector();
}

bool is_dead_vector_computation(const oir::Instruction &instruction) {
    if (instruction.has_uses())
        return false;

    switch (instruction.op()) {
    case oir::Instruction::OpID::Splat:
    case oir::Instruction::OpID::StepVector:
    case oir::Instruction::OpID::ExtractElement:
    case oir::Instruction::OpID::InsertElement:
    case oir::Instruction::OpID::ShuffleVector:
    case oir::Instruction::OpID::VectorSelect:
    case oir::Instruction::OpID::VectorCast:
    case oir::Instruction::OpID::VPBinary:
    case oir::Instruction::OpID::VPCmp:
    case oir::Instruction::OpID::FixedABIExtractLane:
        return true;

    // SetVL has no externally observable OIR state.  A live vector operation
    // carries the actual VL as its EVL operand, so the SetVL result prevents
    // removal whenever it is semantically needed.
    case oir::Instruction::OpID::SetVL:
        return true;

    // SLP commonly leaves the scalar addresses for lanes 1..N-1 dead after
    // replacing them with one base-address VP memory operation.  GEP is a
    // non-trapping pure address computation and is safe to clean here.
    case oir::Instruction::OpID::GetElementPtr:
        return true;

    // Ordinary lane-wise source-vector arithmetic and comparisons use the
    // scalar opcode family with a vector result type.
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Or:
    case oir::Instruction::OpID::Xor:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp:
    case oir::Instruction::OpID::Phi:
        return is_vector_typed(instruction);

    // Do not erase loads, stores, gathers/scatters, reductions, calls, div/rem
    // or control-flow operations here.  Memory observability, traps, ordered
    // FP semantics and CFG changes belong to dedicated audited passes.
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::FDiv:
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::Load:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::MemZero:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
    case oir::Instruction::OpID::FixedABIPack:
    case oir::Instruction::OpID::FixedABIObjectLoadLane:
    case oir::Instruction::OpID::FixedABIObjectStoreLane:
    case oir::Instruction::OpID::VPLoad:
    case oir::Instruction::OpID::VPStore:
    case oir::Instruction::OpID::MaskedLoad:
    case oir::Instruction::OpID::MaskedStore:
    case oir::Instruction::OpID::VPGather:
    case oir::Instruction::OpID::VPScatter:
    case oir::Instruction::OpID::VPReduction:
        return false;
    }
    return false;
}

unsigned cleanup_dead_vector_computations(oir::Module &module) {
    unsigned removed = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &function : module.functions()) {
            if (function->is_external())
                continue;
            for (auto &block : function->blocks()) {
                for (auto iterator = block->instructions().begin();
                     iterator != block->instructions().end();) {
                    if (!is_dead_vector_computation(**iterator)) {
                        ++iterator;
                        continue;
                    }
                    (*iterator)->drop_all_operands();
                    iterator = block->instructions().erase(iterator);
                    ++removed;
                    changed = true;
                }
            }
        }
    }
    return removed;
}

} // namespace

std::string_view OIRVectorCleanupPass::name() const {
    return "OIRVectorCleanupPass";
}

PassKind OIRVectorCleanupPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRVectorCleanupPass::run(PassContext &context) {
    auto *module = context.ssa_module();
    if (module == nullptr) {
        return PassResult::fail("OIRVectorCleanupPass requires OIR module in pass context");
    }

    std::string error;
    if (!module->verify(&error)) {
        return PassResult::fail("OIR vector cleanup input failed verification: " + error);
    }

    try {
        const auto removed = cleanup_dead_vector_computations(*module);
        if (!module->verify(&error)) {
            return PassResult::fail("OIR vector cleanup output failed verification: " + error);
        }
        if (removed != 0)
            context.invalidate_oir_analyses();
        return PassResult::ok(removed != 0, "dead_vector_instructions=" + std::to_string(removed));
    } catch (const std::exception &exception) {
        return PassResult::fail(exception.what());
    }
}

} // namespace pass
