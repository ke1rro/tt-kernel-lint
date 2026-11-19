#pragma once

#include <utility>

#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"

namespace tt::lint {

/** A string error whose message is `format` filled in by llvm::formatv, which takes a null-terminated format. */
template <typename... Values>
[[nodiscard]] llvm::Error MakeError(const char* format, Values&&... values) {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), llvm::formatv(format, std::forward<Values>(values)...).str());
}

}  // namespace tt::lint
