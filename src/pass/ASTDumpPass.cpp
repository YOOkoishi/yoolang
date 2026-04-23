#include "../../include/pass/ASTDumpPass.h"

#include "../../include/ast/ast_printer.h"

#include <ostream>

namespace pass {

ASTDumpPass::ASTDumpPass(std::ostream &out) : out_(out) {
}

std::string_view ASTDumpPass::name() const {
    return "ast-dump";
}

PassKind ASTDumpPass::kind() const {
    return PassKind::Lowering;
}

PassResult ASTDumpPass::run(PassContext &context) {
    if (!context.has_ast()) {
        return PassResult::fail("ASTDumpPass requires AST in pass context");
    }

    out_ << print_ast_to_string(*context.ast());
    return PassResult::ok(false);
}

} // namespace pass
