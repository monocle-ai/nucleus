/**
 * (c) 2015 Nucleus project. All rights reserved.
 * Released under GPL v2 license. Read LICENSE for more details.
 */

#pragma once

#include "nucleus/common.h"

#include "llvm/IR/Function.h"

namespace cpu {
namespace hir {

class Function
{
public:
    llvm::Function* value = nullptr;
};

}  // namespace hir
}  // namespace cpu
