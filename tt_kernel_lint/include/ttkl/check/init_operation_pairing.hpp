#pragma once

#include <string>
#include <vector>

#include "clang/ASTMatchers/ASTMatchFinder.h"

namespace tt::lint {

/** One operation with a missing initializer. */
struct Finding {
    std::string operation;
    std::string initializer;
    std::string call_site;
    // Initializers that may be active at the call; "none" when no initializer ran on some path.
    std::vector<std::string> active_initializers;
};

/**
 * Collects direct calls from kernel_main and from every function reachable from
 * it, then checks that each known operation has its initializer on every path.
 */
class InitOperationPairing final : public clang::ast_matchers::MatchFinder::MatchCallback {
    std::vector<Finding> findings_;
    std::vector<std::string> errors_;

public:
    void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override;

    [[nodiscard]] const std::vector<Finding>& findings() const noexcept { return findings_; }

    /** Analyses that could not complete, so their findings are not reliable. */
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }
};

}  // namespace tt::lint
