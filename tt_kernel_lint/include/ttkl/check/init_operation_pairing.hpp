#pragma once

#include <cstdint>
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

/** Identifier of a block within one clang::CFG, as numbered by CFGBlock::getBlockID(). */
using CfgBlockId = std::uint32_t;

/** Calls and predecessor edges for one CFG block. */
struct CfgBlock {
    std::vector<CallSite> calls;
    std::vector<CfgBlockId> predecessors;
    CfgBlockId id{};
};

/**
 * Collects direct calls from kernel_main and builds the normalized call graph.
 */
class InitOperationPairing final : public clang::ast_matchers::MatchFinder::MatchCallback {
    std::unordered_map<CfgBlockId, CfgBlock> blocks_;

public:
    void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override;

    [[nodiscard]] const std::unordered_map<CfgBlockId, CfgBlock>& blocks() const noexcept { return blocks_; }
};

}  // namespace tt::lint
