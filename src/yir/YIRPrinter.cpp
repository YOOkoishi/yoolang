#include "yir/YIRPrinter.h"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace yir {

namespace {

std::string icmp_predicate(ICmpOp::Predicate pred) {
    switch (pred) {
    case ICmpOp::Predicate::Eq:
        return "eq";
    case ICmpOp::Predicate::Ne:
        return "ne";
    case ICmpOp::Predicate::Lt:
        return "lt";
    case ICmpOp::Predicate::Le:
        return "le";
    case ICmpOp::Predicate::Gt:
        return "gt";
    case ICmpOp::Predicate::Ge:
        return "ge";
    }
    return "unknown";
}

std::string fcmp_predicate(FCmpOp::Predicate pred) {
    switch (pred) {
    case FCmpOp::Predicate::Eq:
        return "eq";
    case FCmpOp::Predicate::Ne:
        return "ne";
    case FCmpOp::Predicate::Lt:
        return "lt";
    case FCmpOp::Predicate::Le:
        return "le";
    case FCmpOp::Predicate::Gt:
        return "gt";
    case FCmpOp::Predicate::Ge:
        return "ge";
    }
    return "unknown";
}

} // namespace

YIRPrinter::YIRPrinter(std::ostream &out) : out_(out) {
}

void YIRPrinter::print(const Module &module) {
    out_ << "module {\n";
    with_indent(1);
    for (const auto &global : module.globals()) {
        print_global(*global);
    }
    for (const auto &function : module.functions()) {
        print_function(*function);
    }
    with_indent(-1);
    out_ << "}\n";
}

void YIRPrinter::print_global(const Global &global) {
    write_indent();
    out_ << "yir.global " << value_name(global.address()) << " : " << global.storage_type()->str();
    if (global.is_const()) {
        out_ << " const";
    }
    if (!global.initializer().empty()) {
        out_ << " = " << global.initializer();
    }
    out_ << '\n';
}

void YIRPrinter::print_function(const Function &function) {
    write_indent();
    out_ << "yir.func @" << function.name() << '(';
    for (std::size_t i = 0; i < function.params().size(); ++i) {
        if (i != 0) {
            out_ << ", ";
        }
        out_ << value_name(function.params()[i].get()) << " : " << function.params()[i]->type()->str();
    }
    out_ << ") -> " << function.return_type()->str() << " {\n";
    with_indent(1);
    print_region(function.body());
    with_indent(-1);
    write_indent();
    out_ << "}\n";
}

void YIRPrinter::print_region(const Region &region) {
    for (const auto &op : region.operations()) {
        print_operation(*op);
    }
}

void YIRPrinter::print_operation(const Operation &op) {
    if (const auto *const_i32 = dynamic_cast<const ConstI32Op *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.const.i32 " << const_i32->value() << " : i32\n";
        return;
    }
    if (const auto *const_f32 = dynamic_cast<const ConstF32Op *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.const.f32 " << std::setprecision(9) << const_f32->value() << " : f32\n";
        return;
    }
    if (const auto *const_bool = dynamic_cast<const ConstBoolOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.const.bool " << (const_bool->value() ? "true" : "false") << " : i1\n";
        return;
    }
    if (dynamic_cast<const ZeroOp *>(&op) != nullptr) {
        write_indent();
        write_result(op);
        out_ << "yir.zero : " << op.result()->type()->str() << '\n';
        return;
    }
    if (const auto *var_op = dynamic_cast<const VarOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.var : " << op.result()->type()->str();
        if (var_op->has_initializer()) {
            out_ << " = " << value_name(var_op->initializer());
        }
        out_ << '\n';
        return;
    }
    if (const auto *assign = dynamic_cast<const AssignOp *>(&op)) {
        write_indent();
        out_ << "yir.assign " << value_name(assign->target()) << ", "
             << value_name(assign->value()) << '\n';
        return;
    }
    if (dynamic_cast<const ArrayVarOp *>(&op) != nullptr) {
        write_indent();
        write_result(op);
        out_ << "yir.array_var : " << op.result()->type()->str() << '\n';
        return;
    }
    if (const auto *array_init = dynamic_cast<const ArrayInitOp *>(&op)) {
        write_indent();
        out_ << "yir.array_init " << value_name(array_init->array()) << " : "
             << array_init->array_type()->str() << " {\n";
        with_indent(1);
        for (const auto &entry : array_init->entries()) {
            write_indent();
            out_ << '[';
            for (std::size_t i = 0; i < entry.indices.size(); ++i) {
                if (i != 0) {
                    out_ << ", ";
                }
                out_ << entry.indices[i];
            }
            out_ << "] = " << (entry.literal.empty() ? init_value(entry.value) : entry.literal) << '\n';
        }
        if (array_init->default_zero()) {
            write_indent();
            out_ << "default = zero\n";
        }
        with_indent(-1);
        write_indent();
        out_ << "}\n";
        return;
    }
    if (const auto *array_load = dynamic_cast<const ArrayLoadOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.array_load " << value_name(array_load->array()) << ", [";
        auto indices = array_load->indices();
        for (std::size_t i = 0; i < indices.size(); ++i) {
            if (i != 0) {
                out_ << ", ";
            }
            out_ << value_name(indices[i]);
        }
        out_ << "] : " << op.result()->type()->str() << '\n';
        return;
    }
    if (const auto *array_store = dynamic_cast<const ArrayStoreOp *>(&op)) {
        write_indent();
        out_ << "yir.array_store " << value_name(array_store->value()) << ", "
             << value_name(array_store->array()) << ", [";
        auto indices = array_store->indices();
        for (std::size_t i = 0; i < indices.size(); ++i) {
            if (i != 0) {
                out_ << ", ";
            }
            out_ << value_name(indices[i]);
        }
        out_ << "]\n";
        return;
    }
    if (const auto *alloca_op = dynamic_cast<const AllocaOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.alloca : " << alloca_op->storage_type()->str() << '\n';
        return;
    }
    if (const auto *load_op = dynamic_cast<const LoadOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.load " << value_name(load_op->address()) << " : " << op.result()->type()->str()
             << '\n';
        return;
    }
    if (const auto *store_op = dynamic_cast<const StoreOp *>(&op)) {
        write_indent();
        out_ << "yir.store " << value_name(store_op->value()) << ", "
             << value_name(store_op->address()) << '\n';
        return;
    }
    if (const auto *elem_addr = dynamic_cast<const ElemAddrOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.elem_addr " << value_name(elem_addr->base()) << ", [";
        auto indices = elem_addr->indices();
        for (std::size_t i = 0; i < indices.size(); ++i) {
            if (i != 0) {
                out_ << ", ";
            }
            out_ << value_name(indices[i]);
        }
        out_ << "] : " << op.result()->type()->str() << '\n';
        return;
    }
    if (const auto *decay = dynamic_cast<const DecayOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.decay " << value_name(decay->array_address()) << " : "
             << op.result()->type()->str() << '\n';
        return;
    }
    if (const auto *icmp = dynamic_cast<const ICmpOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.icmp " << icmp_predicate(icmp->predicate()) << ' ' << value_name(icmp->lhs())
             << ", " << value_name(icmp->rhs()) << " : i1\n";
        return;
    }
    if (const auto *fcmp = dynamic_cast<const FCmpOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.fcmp " << fcmp_predicate(fcmp->predicate()) << ' ' << value_name(fcmp->lhs())
             << ", " << value_name(fcmp->rhs()) << " : i1\n";
        return;
    }
    if (const auto *binary = dynamic_cast<const BinaryOpBase *>(&op)) {
        write_indent();
        write_result(op);
        out_ << op.op_name() << ' ' << value_name(binary->lhs()) << ", " << value_name(binary->rhs())
             << " : " << op.result()->type()->str() << '\n';
        return;
    }
    if (dynamic_cast<const ZExtI1ToI32Op *>(&op) != nullptr ||
        dynamic_cast<const TruncI32ToI1Op *>(&op) != nullptr ||
        dynamic_cast<const SIToFPOp *>(&op) != nullptr ||
        dynamic_cast<const FPToSIOp *>(&op) != nullptr ||
        dynamic_cast<const ToBoolOp *>(&op) != nullptr || dynamic_cast<const NotOp *>(&op) != nullptr) {
        write_indent();
        write_result(op);
        out_ << op.op_name() << ' ' << value_name(op.operands()[0]) << " : "
             << op.result()->type()->str() << '\n';
        return;
    }
    if (const auto *call = dynamic_cast<const CallOp *>(&op)) {
        write_indent();
        write_result(op);
        out_ << "yir.call @" << call->callee() << '(';
        for (std::size_t i = 0; i < call->args().size(); ++i) {
            if (i != 0) {
                out_ << ", ";
            }
            out_ << value_name(call->args()[i]);
        }
        out_ << ')';
        if (op.result() != nullptr) {
            out_ << " : " << op.result()->type()->str();
        }
        out_ << '\n';
        return;
    }
    if (const auto *if_op = dynamic_cast<const IfOp *>(&op)) {
        write_indent();
        out_ << "yir.if " << value_name(if_op->condition()) << " {\n";
        with_indent(1);
        print_region(if_op->then_region());
        with_indent(-1);
        write_indent();
        out_ << '}';
        if (if_op->has_else()) {
            out_ << " else {\n";
            with_indent(1);
            print_region(if_op->else_region());
            with_indent(-1);
            write_indent();
            out_ << '}';
        }
        out_ << '\n';
        return;
    }
    if (const auto *while_op = dynamic_cast<const WhileOp *>(&op)) {
        write_indent();
        out_ << "yir.while {\n";
        with_indent(1);
        write_indent();
        out_ << "^cond:\n";
        with_indent(1);
        print_region(while_op->cond_region());
        with_indent(-1);
        write_indent();
        out_ << "^body:\n";
        with_indent(1);
        print_region(while_op->body_region());
        with_indent(-1);
        with_indent(-1);
        write_indent();
        out_ << "}\n";
        return;
    }
    if (const auto *for_op = dynamic_cast<const ForOp *>(&op)) {
        write_indent();
        out_ << "yir.for " << value_name(for_op->induction_var()) << " = "
             << value_name(for_op->lower_bound()) << " to " << value_name(for_op->upper_bound())
             << " step " << value_name(for_op->step());
        if (for_op->is_parallel()) {
            out_ << " parallel";
        }
        out_ << " {\n";
        with_indent(1);
        print_region(for_op->body_region());
        with_indent(-1);
        write_indent();
        out_ << "}\n";
        return;
    }
    if (const auto *cond = dynamic_cast<const CondOp *>(&op)) {
        write_indent();
        out_ << "yir.cond " << value_name(cond->condition()) << '\n';
        return;
    }
    if (dynamic_cast<const BreakOp *>(&op) != nullptr) {
        write_indent();
        out_ << "yir.break\n";
        return;
    }
    if (dynamic_cast<const ContinueOp *>(&op) != nullptr) {
        write_indent();
        out_ << "yir.continue\n";
        return;
    }
    if (const auto *ret = dynamic_cast<const ReturnOp *>(&op)) {
        write_indent();
        out_ << "yir.return";
        if (ret->has_value()) {
            out_ << ' ' << value_name(ret->value());
        }
        out_ << '\n';
        return;
    }
}

std::string YIRPrinter::value_name(const Value *value) {
    if (value == nullptr) {
        return "<null>";
    }
    auto it = names_.find(value);
    if (it != names_.end()) {
        return it->second;
    }

    std::string name = value->name();
    if (name.empty()) {
        name = "v" + std::to_string(next_value_id_++);
    }
    if (!name.empty() && name[0] != '%' && name[0] != '@') {
        name = "%" + name;
    }

    names_.emplace(value, name);
    return name;
}

std::string YIRPrinter::init_value(const Value *value) {
    if (value == nullptr) {
        return "zero";
    }
    if (const auto *op = value->defining_op()) {
        if (const auto *i32 = dynamic_cast<const ConstI32Op *>(op)) {
            return std::to_string(i32->value());
        }
        if (const auto *f32 = dynamic_cast<const ConstF32Op *>(op)) {
            std::ostringstream oss;
            oss << std::setprecision(9) << f32->value();
            return oss.str();
        }
        if (const auto *boolean = dynamic_cast<const ConstBoolOp *>(op)) {
            return boolean->value() ? "true" : "false";
        }
        if (dynamic_cast<const ZeroOp *>(op) != nullptr) {
            return "zero";
        }
    }
    return value_name(value);
}

void YIRPrinter::write_indent() {
    for (int i = 0; i < indent_; ++i) {
        out_ << "  ";
    }
}

void YIRPrinter::write_result(const Operation &op) {
    if (op.result() != nullptr) {
        out_ << value_name(op.result()) << " = ";
    }
}

void YIRPrinter::with_indent(int delta) {
    indent_ += delta;
}

std::string print_yir_to_string(const Module &module) {
    std::ostringstream oss;
    YIRPrinter printer(oss);
    printer.print(module);
    return oss.str();
}

} // namespace yir
