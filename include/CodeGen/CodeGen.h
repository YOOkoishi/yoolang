#pragma once

// Compatibility header. Canonical AST->IR lowering API lives in IRGen.
#include "../IRGen/IRGen.h"

namespace codegen {
using LoweringOptions = irgen::LoweringOptions;
using ASTToSSA = irgen::ASTToIRLowering;

} // namespace codegen
