#pragma once
#include "llvm/IR/Module.h"

class CommandTranslator{
    public:
    [[nodiscard]] llvm::Expected<TranslationUnit> translate()
}
