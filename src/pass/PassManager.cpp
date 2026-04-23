#include "../../include/pass/PassManager.h"

#include <stdexcept>

namespace pass {

std::string_view to_string(PassKind kind) {
    switch (kind) {
    case PassKind::Analysis:
        return "analysis";
    case PassKind::Transform:
        return "transform";
    case PassKind::Lowering:
        return "lowering";
    case PassKind::Verification:
        return "verification";
    }
    return "unknown";
}

PassResult PassResult::ok(bool changed, std::string message) {
    PassResult result;
    result.success = true;
    result.changed = changed;
    result.message = std::move(message);
    return result;
}

PassResult PassResult::fail(std::string message) {
    PassResult result;
    result.success = false;
    result.changed = false;
    result.message = std::move(message);
    return result;
}

bool PassContext::has_ast() const {
    return ast_ != nullptr;
}

CompUnit *PassContext::ast() {
    return ast_.get();
}

const CompUnit *PassContext::ast() const {
    return ast_.get();
}

void PassContext::set_ast(std::unique_ptr<CompUnit> ast) {
    ast_ = std::move(ast);
}

std::unique_ptr<CompUnit> PassContext::take_ast() {
    return std::move(ast_);
}

bool PassContext::has_ssa_module() const {
    return ssa_module_ != nullptr;
}

ir::Module *PassContext::ssa_module() {
    return ssa_module_.get();
}

const ir::Module *PassContext::ssa_module() const {
    return ssa_module_.get();
}

void PassContext::set_ssa_module(std::unique_ptr<ir::Module> module) {
    ssa_module_ = std::move(module);
}

std::unique_ptr<ir::Module> PassContext::take_ssa_module() {
    return std::move(ssa_module_);
}

bool PassContext::has_artifact(const std::string &key) const {
    return artifacts_.find(key) != artifacts_.end();
}

void PassContext::erase_artifact(const std::string &key) {
    artifacts_.erase(key);
}

void PassManager::set_stop_on_failure(bool stop_on_failure) {
    stop_on_failure_ = stop_on_failure;
}

bool PassManager::stop_on_failure() const {
    return stop_on_failure_;
}

void PassManager::register_pass(std::unique_ptr<Pass> pass) {
    if (pass == nullptr) {
        throw std::invalid_argument("cannot register null pass");
    }
    passes_.push_back(std::move(pass));
}

void PassManager::clear() {
    passes_.clear();
}

std::size_t PassManager::size() const {
    return passes_.size();
}

bool PassManager::empty() const {
    return passes_.empty();
}

PassManagerResult PassManager::run(PassContext &context) {
    PassManagerResult summary;

    for (auto &pass : passes_) {
        PassExecution execution;
        execution.name = std::string(pass->name());
        execution.kind = pass->kind();
        execution.result = pass->run(context);

        summary.changed = summary.changed || execution.result.changed;
        summary.success = summary.success && execution.result.success;
        summary.executions.push_back(std::move(execution));

        if (!summary.success && stop_on_failure_) {
            break;
        }
    }

    return summary;
}

} // namespace pass
