#pragma once

#include "../../include/oir/oir.h"
#include "../../include/yir/YIR.h"

#include <memory>

namespace pass::yir_to_oir {

std::unique_ptr<oir::Module> lower_yir_to_oir(const yir::Module &module);

} // namespace pass::yir_to_oir
