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
};

/**
 * Collects direct calls from kernel_main and from every function reachable from
 * it, then checks that each known operation has its initializer on every path.
 */
class InitOperationPairing final : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
    void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override;

    [[nodiscard]] const std::vector<Finding>& findings() const noexcept { return findings_; }

private:
    std::vector<Finding> findings_;
};

}  // namespace tt::lint
