#include "ttkl/check/init_operation_pairing.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Casting.h"

namespace tt::lint {
namespace {

struct PairRule {
    std::string_view operation;
    std::string_view initializer;
};

constexpr std::array<PairRule, 12> kPairRules{{
    {"add_tiles", "add_init"},
    {"sub_tiles", "sub_init"},
    {"mul_tiles", "mul_init"},
    {"matmul_tiles", "matmul_init"},
    {"copy_tile", "copy_init"},
    {"add_reuse_dest_tiles", "add_reuse_dest_init"},
    {"sub_reuse_dest_tiles", "sub_reuse_dest_init"},
    {"mul_reuse_dest_tiles", "mul_reuse_dest_init"},
    {"add_block", "add_init"},
    {"sub_block", "sub_init"},
    {"mul_block", "mul_init"},
    {"matmul_block", "matmul_block_init"},
}};

std::string GetUsr(const clang::FunctionDecl* function) {
    llvm::SmallString<128> usr;
    clang::index::generateUSRForDecl(function, usr);
    return usr.str().str();
}

std::string GetLocationString(const clang::SourceManager& source_manager, clang::SourceLocation location) {
    location = source_manager.getExpansionLoc(location);
    const clang::PresumedLoc presumed = source_manager.getPresumedLoc(location);
    if (presumed.isInvalid() || presumed.getFilename() == nullptr) {
        return "<invalid>";
    }
    return std::string(presumed.getFilename()) + ":" + std::to_string(presumed.getLine()) + ":" +
           std::to_string(presumed.getColumn());
}

std::optional<std::string_view> RequiredInitializer(std::string_view operation) {
    for (const PairRule& rule : kPairRules) {
        if (rule.operation == operation) {
            return rule.initializer;
        }
    }
    return std::nullopt;
}

bool IsInitializer(std::string_view name) { return name.ends_with("_init"); }

std::set<std::string> AllKnownInitializers() {
    std::set<std::string> result;
    for (const PairRule& rule : kPairRules) {
        result.emplace(rule.initializer);
    }
    return result;
}

}  // namespace

void InitOperationPairing::run(const clang::ast_matchers::MatchFinder::MatchResult& result) {
    blocks_.clear();
    findings_.clear();

    const auto* function = result.Nodes.getNodeAs<clang::FunctionDecl>("function");
    if (function == nullptr || function->isInvalidDecl() || !function->hasBody()) {
        return;
    }

    const clang::SourceManager& source_manager = *result.SourceManager;
    std::unique_ptr<clang::CFG> cfg =
        clang::CFG::buildCFG(function, function->getBody(), result.Context, clang::CFG::BuildOptions());
    if (!cfg) {
        return;
    }

    entry_block_ = cfg->getEntry().getBlockID();

    for (const auto& block : *cfg) {
        CfgBlock collected;
        collected.id = block->getBlockID();

        for (const auto& element : *block) {
            const auto statement = element.getAs<clang::CFGStmt>();
            if (!statement.has_value()) {
                continue;
            }
            const auto* call = llvm::dyn_cast<clang::CallExpr>(statement->getStmt());
            if (call == nullptr) {
                continue;
            }
            const clang::FunctionDecl* callee = call->getDirectCallee();
            if (callee == nullptr) {
                continue;
            }

            collected.calls.push_back(
                CallSite{
                    GetUsr(callee),
                    callee->getNameInfo().getName().getAsString(),
                    GetLocationString(source_manager, callee->getBeginLoc()),
                    GetLocationString(source_manager, call->getBeginLoc()),
                });
        }

        for (const clang::CFGBlock* predecessor : block->preds()) {
            if (predecessor != nullptr) {
                collected.predecessors.push_back(predecessor->getBlockID());
            }
        }

        blocks_.emplace(collected.id, std::move(collected));
    }

    Analyze();
}

void InitOperationPairing::Analyze() {
    const std::set<std::string> all_initializers = AllKnownInitializers();

    std::unordered_map<unsigned, std::set<std::string>> in;
    std::unordered_map<unsigned, std::set<std::string>> out;
    for (const auto& [id, block] : blocks_) {
        (void)block;
        in[id] = id == entry_block_ ? std::set<std::string>{} : all_initializers;
        out[id] = all_initializers;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [id, block] : blocks_) {
            std::set<std::string> next_in;
            if (id != entry_block_ && !block.predecessors.empty()) {
                next_in = all_initializers;
                for (const unsigned predecessor : block.predecessors) {
                    const auto predecessor_out = out.find(predecessor);
                    if (predecessor_out == out.end()) {
                        next_in.clear();
                        break;
                    }
                    std::set<std::string> intersection;
                    std::set_intersection(
                        next_in.begin(),
                        next_in.end(),
                        predecessor_out->second.begin(),
                        predecessor_out->second.end(),
                        std::inserter(intersection, intersection.begin()));
                    next_in = std::move(intersection);
                }
            }

            std::set<std::string> next_out = next_in;
            for (const CallSite& call : block.calls) {
                if (IsInitializer(call.name)) {
                    next_out.insert(call.name);
                }
            }

            if (next_in != in[id] || next_out != out[id]) {
                in[id] = std::move(next_in);
                out[id] = std::move(next_out);
                changed = true;
            }
        }
    }

    for (const auto& [id, block] : blocks_) {
        std::set<std::string> state = in[id];
        for (const CallSite& call : block.calls) {
            if (IsInitializer(call.name)) {
                state.insert(call.name);
            }

            const std::optional<std::string_view> required = RequiredInitializer(call.name);
            if (required.has_value() && !state.contains(std::string(*required))) {
                findings_.push_back(Finding{call.name, std::string(*required), call.call_site});
            }
        }
    }
}

}  // namespace tt::lint
