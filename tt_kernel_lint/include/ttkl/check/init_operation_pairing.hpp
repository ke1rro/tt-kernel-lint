#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "clang/ASTMatchers/ASTMatchFinder.h"

namespace tt::lint {

/** One direct call found in a CFG block. */
struct CallSite {
    std::string usr;
    std::string name;
    std::string definition_site;
    std::string call_site;
};

/** Calls and predecessor edges for one CFG block. */
struct CfgBlock {
    unsigned id{};
    std::vector<CallSite> calls;
    std::vector<unsigned> predecessors;
};

/**
 * Collects direct calls from kernel_main and builds the normalized call graph.
 */
class InitOperationPairing final : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
    void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override;

    [[nodiscard]] const std::unordered_map<unsigned, CfgBlock>& blocks() const noexcept { return blocks_; }

private:
    std::unordered_map<unsigned, CfgBlock> blocks_;
};

}  // namespace tt::lint
