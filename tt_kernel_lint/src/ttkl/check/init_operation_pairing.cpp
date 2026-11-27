#include "ttkl/check/init_operation_pairing.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/Expr.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

namespace tt::lint {
namespace {

using CfgBlockId = std::uint32_t;
static_assert(
    std::is_same_v<decltype(std::declval<const clang::CFGBlock&>().getBlockID()), CfgBlockId>,
    "CfgBlockId must be the type clang::CFGBlock ids have");

// The public compute API is declared under this directory, in namespace ckernel. Only its functions count as
// initializers and operations, so a kernel function that happens to share a name does not.
constexpr std::string_view kComputeApiDirectory{"tt_metal/hw/inc/api/compute/"};
constexpr std::string_view kApiNamespace{"ckernel"};

// Device code below the kernel: the compute API itself, LLK and firmware. The analysis follows calls into it, because
// API functions such as copy_tile_init and sdpa_tail_ms_reduce run initializers themselves, but reports what it finds
// there at the kernel call that reached it.
constexpr auto kImplementationDirectories = std::to_array<std::string_view>({"tt_metal/hw/", "tt_metal/tt-llk/"});

constexpr std::string_view kNoInitializerName{"none"};
constexpr std::size_t kMaxAnalysisRounds{10000};

struct PairRule {
    std::string_view operation;
    std::string_view initializer;
};

constexpr auto kPairRules = std::to_array<PairRule>({
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
});

// The set of initializers that may be active. Bit 0 means that no initializer ran since kernel entry, bit index + 1
// stands for kInitializers.names[index]. Every initializer in the table reconfigures unpack and math, so the one that
// ran last replaces whatever was active before it.
using InitSet = std::uint32_t;
constexpr InitSet kNoInitializer{1};
constexpr std::size_t kMaxInitializers{31};

struct InitializerTable {
    std::array<std::string_view, kMaxInitializers> names{};
    std::size_t size{0};
};

[[nodiscard]] constexpr InitializerTable MakeInitializerTable() {
    InitializerTable table;
    for (const PairRule& rule : kPairRules) {
        const std::span<const std::string_view> known{table.names.data(), table.size};
        if (std::find(known.begin(), known.end(), rule.initializer) == known.end()) {
            table.names.at(table.size) = rule.initializer;
            ++table.size;
        }
    }
    return table;
}

constexpr InitializerTable kInitializers{MakeInitializerTable()};

[[nodiscard]] constexpr std::optional<InitSet> InitializerBit(std::string_view name) {
    for (std::size_t index{0}; index < kInitializers.size; ++index) {
        if (kInitializers.names.at(index) == name) {
            return InitSet{2} << index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<std::string_view> RequiredInitializer(std::string_view operation) {
    for (const auto& [rule_operation, initializer] : kPairRules) {
        if (rule_operation == operation) {
            return initializer;
        }
    }
    return std::nullopt;
}

// How a stretch of code changes the active initializer: afterwards it is one of `generated`, or, when `transparent`,
// still the one that was active before.
struct InitEffect {
    InitSet generated{};
    bool transparent{};

    [[nodiscard]] bool operator==(const InitEffect&) const = default;
};

// std::nullopt when no path runs through the code.
using MaybeEffect = std::optional<InitEffect>;

constexpr InitEffect kNoEffect{0, true};

[[nodiscard]] constexpr MaybeEffect Then(const MaybeEffect& first, const MaybeEffect& second) {
    if (!first || !second) {
        return std::nullopt;
    }
    return InitEffect{
        second->generated | (second->transparent ? first->generated : InitSet{0}),
        first->transparent && second->transparent};
}

[[nodiscard]] constexpr MaybeEffect Join(const MaybeEffect& left, const MaybeEffect& right) {
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    return InitEffect{left->generated | right->generated, left->transparent || right->transparent};
}

[[nodiscard]] constexpr InitSet Apply(const InitEffect& effect, InitSet active) {
    return effect.generated | (effect.transparent ? active : InitSet{0});
}

enum class CallKind : std::uint8_t {
    kOther,
    kInitializer,
    kOperation,
};

struct CallSite {
    std::string name;
    std::string location;
    // Definition the analysis follows into; nullptr for API events and code it cannot see.
    const clang::FunctionDecl* callee{nullptr};
    std::string_view required_initializer;
    InitSet initializer{};
    CallKind kind{};
};

struct CfgBlock {
    std::vector<CallSite> calls;
    std::vector<CfgBlockId> predecessors;
};

struct OperationUse {
    std::string operation;
    std::string initializer;
    std::string call_site;
    // Effect of the code from the function entry to the call.
    InitEffect before{};

    [[nodiscard]] bool operator==(const OperationUse&) const = default;
};

struct FunctionGraph {
    // Indexed by CfgBlockId; empty when Clang could not build a CFG.
    std::vector<CfgBlock> blocks;
    std::map<std::string, OperationUse> uses;
    std::string name;
    // Effect from entry to exit; std::nullopt until a path to the exit is known.
    MaybeEffect summary;
    CfgBlockId entry{};
    CfgBlockId exit{};
    bool is_implementation{};
};

using FunctionGraphs = std::unordered_map<const clang::FunctionDecl*, FunctionGraph>;

[[nodiscard]] std::string GetLocationString(
    const clang::SourceManager& source_manager, clang::SourceLocation location) {
    location = source_manager.getExpansionLoc(location);
    const clang::PresumedLoc presumed{source_manager.getPresumedLoc(location)};
    if (presumed.isInvalid() || presumed.getFilename() == nullptr) {
        return "<invalid>";
    }
    return std::string(presumed.getFilename()) + ":" + std::to_string(presumed.getLine()) + ":" +
           std::to_string(presumed.getColumn());
}

[[nodiscard]] llvm::StringRef FileOf(const clang::SourceManager& source_manager, const clang::Decl& declaration) {
    return source_manager.getFilename(source_manager.getExpansionLoc(declaration.getLocation()));
}

[[nodiscard]] bool IsComputeApi(const clang::FunctionDecl& function, const clang::SourceManager& source_manager) {
    const auto* name_space{llvm::dyn_cast<clang::NamespaceDecl>(function.getDeclContext()->getRedeclContext())};
    if (name_space == nullptr || name_space->getName() != llvm::StringRef{kApiNamespace} ||
        !name_space->getDeclContext()->getRedeclContext()->isTranslationUnit()) {
        return false;
    }
    return FileOf(source_manager, *function.getCanonicalDecl()).contains(kComputeApiDirectory);
}

[[nodiscard]] bool IsImplementation(const clang::FunctionDecl& function, const clang::SourceManager& source_manager) {
    const llvm::StringRef file{FileOf(source_manager, function)};
    return std::any_of(
        kImplementationDirectories.begin(), kImplementationDirectories.end(), [file](std::string_view directory) {
            return file.contains(directory);
        });
}

[[nodiscard]] CallSite DescribeCall(
    const clang::CallExpr& call, const clang::FunctionDecl& callee, const clang::SourceManager& source_manager) {
    CallSite site;
    site.name = callee.getNameAsString();
    site.location = GetLocationString(source_manager, call.getBeginLoc());
    if (IsComputeApi(callee, source_manager)) {
        if (const std::optional<InitSet> bit{InitializerBit(site.name)}) {
            site.kind = CallKind::kInitializer;
            site.initializer = *bit;
            return site;
        }
        if (const std::optional<std::string_view> required{RequiredInitializer(site.name)}) {
            site.kind = CallKind::kOperation;
            site.required_initializer = *required;
            return site;
        }
    }
    const clang::FunctionDecl* definition{callee.getDefinition()};
    if (definition != nullptr && definition->hasBody() && !definition->isImplicit() &&
        !source_manager.isInSystemHeader(definition->getLocation())) {
        site.callee = definition;
    }
    return site;
}

// One CFG per function reachable from `root` through direct calls, keyed by definition so that functions never merge.
[[nodiscard]] FunctionGraphs BuildGraphs(const clang::FunctionDecl& root, clang::ASTContext& context) {
    const clang::SourceManager& source_manager{context.getSourceManager()};
    FunctionGraphs graphs;
    std::vector<const clang::FunctionDecl*> worklist{&root};

    while (!worklist.empty()) {
        const clang::FunctionDecl* function{worklist.back()};
        worklist.pop_back();
        if (function->isInvalidDecl() || graphs.contains(function)) {
            continue;
        }
        FunctionGraph graph;
        graph.name = function->getNameAsString();
        graph.is_implementation = IsImplementation(*function, source_manager);

        const std::unique_ptr<clang::CFG> cfg{
            clang::CFG::buildCFG(function, function->getBody(), &context, clang::CFG::BuildOptions())};
        if (!cfg) {
            // Nothing to follow: calls to it neither run nor reset an initializer.
            graph.summary = kNoEffect;
            graphs.emplace(function, std::move(graph));
            continue;
        }
        graph.entry = CfgBlockId{cfg->getEntry().getBlockID()};
        graph.exit = CfgBlockId{cfg->getExit().getBlockID()};
        graph.blocks.resize(cfg->getNumBlockIDs());

        for (const clang::CFGBlock* block : *cfg) {
            CfgBlock& collected{graph.blocks.at(block->getBlockID())};
            for (const clang::CFGElement& element : *block) {
                const std::optional<clang::CFGStmt> statement{element.getAs<clang::CFGStmt>()};
                if (!statement) {
                    continue;
                }
                const auto* call{llvm::dyn_cast<clang::CallExpr>(statement->getStmt())};
                const clang::FunctionDecl* callee{call == nullptr ? nullptr : call->getDirectCallee()};
                if (callee == nullptr) {
                    continue;
                }
                CallSite site{DescribeCall(*call, *callee, source_manager)};
                if (site.callee != nullptr) {
                    worklist.push_back(site.callee);
                }
                collected.calls.push_back(std::move(site));
            }
            for (const clang::CFGBlock* predecessor : block->preds()) {
                if (predecessor != nullptr) {
                    collected.predecessors.push_back(CfgBlockId{predecessor->getBlockID()});
                }
            }
        }
        graphs.emplace(function, std::move(graph));
    }
    return graphs;
}

[[nodiscard]] MaybeEffect EffectOf(const CallSite& call, const FunctionGraphs& graphs) {
    switch (call.kind) {
        case CallKind::kInitializer: return InitEffect{call.initializer, false};
        case CallKind::kOperation: return kNoEffect;
        case CallKind::kOther: break;
    }
    if (call.callee == nullptr) {
        return kNoEffect;
    }
    const auto callee{graphs.find(call.callee)};
    return callee == graphs.end() ? kNoEffect : callee->second.summary;
}

void AddUse(std::map<std::string, OperationUse>& uses, const OperationUse& use) {
    std::string key{use.call_site + '\x1f' + use.operation + '\x1f' + use.initializer};
    const auto [existing, inserted] = uses.try_emplace(std::move(key), use);
    if (!inserted) {
        existing->second.before = *Join(existing->second.before, use.before);
    }
}

// Recomputes the summary and the operation uses of `graph` from the current summaries of its callees.
// Returns whether either changed.
[[nodiscard]] bool Analyze(FunctionGraph& graph, const FunctionGraphs& graphs) {
    if (graph.blocks.empty()) {
        return false;
    }
    std::vector<MaybeEffect> in(graph.blocks.size());
    std::vector<MaybeEffect> out(graph.blocks.size());
    const auto run_block{[&graphs](MaybeEffect state, const CfgBlock& block) {
        for (const CallSite& call : block.calls) {
            state = Then(state, EffectOf(call, graphs));
        }
        return state;
    }};

    // Clang numbers the entry block last, so visiting ids in descending order follows the control flow.
    for (bool changed{true}; changed;) {
        changed = false;
        for (std::size_t index{graph.blocks.size()}; index-- > 0;) {
            const CfgBlock& block{graph.blocks[index]};
            MaybeEffect next_in{index == graph.entry ? MaybeEffect{kNoEffect} : std::nullopt};
            for (const CfgBlockId predecessor : block.predecessors) {
                next_in = Join(next_in, out.at(predecessor));
            }
            const MaybeEffect next_out{run_block(next_in, block)};
            if (next_in != in[index] || next_out != out[index]) {
                in[index] = next_in;
                out[index] = next_out;
                changed = true;
            }
        }
    }

    std::map<std::string, OperationUse> uses;
    for (std::size_t index{0}; index < graph.blocks.size(); ++index) {
        MaybeEffect state{in[index]};
        for (const CallSite& call : graph.blocks[index].calls) {
            if (!state) {
                break;
            }
            if (call.kind == CallKind::kOperation) {
                AddUse(uses, OperationUse{call.name, std::string{call.required_initializer}, call.location, *state});
            } else if (const auto callee{call.callee == nullptr ? graphs.end() : graphs.find(call.callee)};
                       callee != graphs.end()) {
                const FunctionGraph& callee_graph{callee->second};
                const bool report_here{callee_graph.is_implementation && !graph.is_implementation};
                for (const auto& entry : callee_graph.uses) {
                    OperationUse lifted{entry.second};
                    lifted.before = *Then(state, lifted.before);
                    if (report_here) {
                        lifted.operation += " via " + callee_graph.name;
                        lifted.call_site = call.location;
                    }
                    AddUse(uses, lifted);
                }
            }
            state = Then(state, EffectOf(call, graphs));
        }
    }

    const MaybeEffect summary{in.at(graph.exit)};
    const bool changed{summary != graph.summary || uses != graph.uses};
    graph.summary = summary;
    graph.uses = std::move(uses);
    return changed;
}

// Summaries and uses only grow, so the rounds end; the bound only guards against a broken invariant.
[[nodiscard]] bool AnalyzeAll(FunctionGraphs& graphs) {
    for (std::size_t round{0}; round < kMaxAnalysisRounds; ++round) {
        bool changed{false};
        for (auto& entry : graphs) {
            changed = Analyze(entry.second, graphs) || changed;
        }
        if (!changed) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::vector<std::string> InitializerNames(InitSet active) {
    std::vector<std::string> names;
    if ((active & kNoInitializer) != 0) {
        names.emplace_back(kNoInitializerName);
    }
    for (std::size_t index{0}; index < kInitializers.size; ++index) {
        if ((active & (InitSet{2} << index)) != 0) {
            names.emplace_back(kInitializers.names.at(index));
        }
    }
    return names;
}

}  // namespace

void InitOperationPairing::run(const clang::ast_matchers::MatchFinder::MatchResult& result) {
    findings_.clear();
    errors_.clear();

    const auto* function{result.Nodes.getNodeAs<clang::FunctionDecl>("function")};
    if (function == nullptr || function->isInvalidDecl() || !function->hasBody() || result.Context == nullptr) {
        return;
    }

    FunctionGraphs graphs{BuildGraphs(*function, *result.Context)};
    if (!AnalyzeAll(graphs)) {
        errors_.push_back(
            "the init/operation analysis of " + GetLocationString(*result.SourceManager, function->getLocation()) +
            " did not converge");
        return;
    }
    const auto root{graphs.find(function)};
    if (root == graphs.end()) {
        return;
    }
    for (const auto& entry : root->second.uses) {
        const OperationUse& use{entry.second};
        const InitSet active{Apply(use.before, kNoInitializer)};
        if (active == InitializerBit(use.initializer)) {
            continue;
        }
        findings_.push_back(Finding{use.operation, use.initializer, use.call_site, InitializerNames(active)});
    }
}

}  // namespace tt::lint
