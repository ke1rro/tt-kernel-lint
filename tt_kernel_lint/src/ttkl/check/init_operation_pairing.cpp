#include "ttkl/check/init_operation_pairing.hpp"

#include <memory>
#include <type_traits>
#include <utility>

#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Casting.h"

namespace tt::lint {
namespace {

using ClangBlockId = decltype(std::declval<const clang::CFGBlock&>().getBlockID());
static_assert(std::is_same_v<ClangBlockId, CfgBlockId>, "CfgBlockId must be the type clang::CFGBlock ids have");

std::string GetUsr(const clang::FunctionDecl* function) {
    llvm::SmallString<128> usr;
    clang::index::generateUSRForDecl(function, usr);
    return usr.str().str();
}

std::string GetLocationString(const clang::SourceManager& source_manager, clang::SourceLocation location) {
    location = source_manager.getExpansionLoc(location);
    const clang::PresumedLoc presumed{source_manager.getPresumedLoc(location)};
    if (presumed.isInvalid() || presumed.getFilename() == nullptr) {
        return "<invalid>";
    }
    return std::string(presumed.getFilename()) + ":" + std::to_string(presumed.getLine()) + ":" +
           std::to_string(presumed.getColumn());
}

}  // namespace

void InitOperationPairing::run(const clang::ast_matchers::MatchFinder::MatchResult& result) {
    blocks_.clear();

    const auto* function = result.Nodes.getNodeAs<clang::FunctionDecl>("function");
    if (function == nullptr || function->isInvalidDecl() || !function->hasBody()) {
        return;
    }

    const clang::SourceManager& source_manager{*result.SourceManager};
    const std::unique_ptr<clang::CFG> cfg{
        clang::CFG::buildCFG(function, function->getBody(), result.Context, clang::CFG::BuildOptions())};
    if (!cfg) {
        return;
    }

    for (const auto& block : *cfg) {
        CfgBlock collected;
        collected.id = CfgBlockId{block->getBlockID()};

        for (const auto& element : *block) {
            const auto statement = element.getAs<clang::CFGStmt>();
            if (!statement.has_value()) {
                continue;
            }
            const auto* call = llvm::dyn_cast<clang::CallExpr>(statement->getStmt());
            if (call == nullptr) {
                continue;
            }
            const clang::FunctionDecl* callee{call->getDirectCallee()};
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
                collected.predecessors.push_back(CfgBlockId{predecessor->getBlockID()});
            }
        }

        blocks_.emplace(collected.id, std::move(collected));
    }
}

}  // namespace tt::lint
