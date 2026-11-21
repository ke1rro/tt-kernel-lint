#include "ttkl/check/init_operation_pairing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

constexpr std::size_t kMaxInitializers = 16;

struct InitializerTable {
    std::array<std::string_view, kMaxInitializers> names{};
    std::size_t size{};
};

constexpr InitializerTable MakeInitializerTable() {
    InitializerTable table;
    for (const PairRule& rule : kPairRules) {
        bool present = false;
        for (std::size_t index = 0; index < table.size; ++index) {
            if (table.names[index] == rule.initializer) {
                present = true;
                break;
            }
        }
        if (!present) {
            table.names[table.size] = rule.initializer;
            ++table.size;
        }
    }
    return table;
}

constexpr InitializerTable kInitializers = MakeInitializerTable();

using InitMask = std::uint32_t;

constexpr InitMask InitializerMask(std::string_view name) {
    for (std::size_t index = 0; index < kInitializers.size; ++index) {
        if (kInitializers.names[index] == name) {
            return InitMask{1} << index;
        }
    }
    return 0;
}

constexpr InitMask AllInitializers() {
    if (kInitializers.size == 0) {
        return 0;
    }
    return (InitMask{1} << kInitializers.size) - 1;
}

constexpr InitMask kAllInitializers = AllInitializers();

struct CallSite {
    std::string usr;
    std::string name;
    std::string call_site;
};

struct CfgBlock {
    std::vector<CallSite> calls;
    std::vector<unsigned> predecessors;
    std::vector<unsigned> successors;
};

struct OperationUse {
    std::string operation;
    std::string initializer;
    std::string call_site;
    InitMask guaranteed_before{};

    bool operator==(const OperationUse&) const = default;
};

struct FunctionGraph {
    unsigned entry{};
    unsigned exit{};
    std::unordered_map<unsigned, CfgBlock> blocks;
    InitMask guarantees{};
    std::map<std::string, OperationUse> ops;
};

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

// The parameter is moved into the map when the key is new.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
void AddOperation(std::map<std::string, OperationUse>& ops, OperationUse use) {
    const InitMask guaranteed = use.guaranteed_before;
    std::string key = use.call_site;
    key.push_back('\x1f');
    key.append(use.initializer);

    const auto [iterator, inserted] = ops.try_emplace(std::move(key), std::move(use));
    if (!inserted) {
        iterator->second.guaranteed_before &= guaranteed;
    }
}

std::unordered_map<std::string, FunctionGraph> BuildGraph(const clang::FunctionDecl& root, clang::ASTContext& context) {
    const clang::SourceManager& source_manager = context.getSourceManager();
    std::unordered_map<std::string, FunctionGraph> functions;
    std::vector<const clang::FunctionDecl*> worklist{&root};

    while (!worklist.empty()) {
        const clang::FunctionDecl* function = worklist.back();
        worklist.pop_back();

        if (function == nullptr || function->isInvalidDecl() || function->isImplicit() || !function->hasBody()) {
            continue;
        }
        if (function != &root && source_manager.isInSystemHeader(function->getBeginLoc())) {
            continue;
        }

        const std::string usr = GetUsr(function);
        if (functions.contains(usr)) {
            continue;
        }

        std::unique_ptr<clang::CFG> cfg =
            clang::CFG::buildCFG(function, function->getBody(), &context, clang::CFG::BuildOptions());
        if (!cfg) {
            continue;
        }

        FunctionGraph graph;
        graph.entry = cfg->getEntry().getBlockID();
        graph.exit = cfg->getExit().getBlockID();

        std::vector<const clang::FunctionDecl*> callees;
        for (const auto& block : *cfg) {
            CfgBlock collected;

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
                        GetLocationString(source_manager, call->getBeginLoc()),
                    });
                if (const clang::FunctionDecl* definition = callee->getDefinition(); definition != nullptr) {
                    callees.push_back(definition);
                }
            }

            for (const clang::CFGBlock* predecessor : block->preds()) {
                if (predecessor != nullptr) {
                    collected.predecessors.push_back(predecessor->getBlockID());
                }
            }
            for (const clang::CFGBlock* successor : block->succs()) {
                if (successor != nullptr) {
                    collected.successors.push_back(successor->getBlockID());
                }
            }

            graph.blocks.emplace(block->getBlockID(), std::move(collected));
        }

        functions.emplace(usr, std::move(graph));
        worklist.insert(worklist.end(), callees.begin(), callees.end());
    }

    return functions;
}

std::vector<unsigned> ReachableBlocks(const FunctionGraph& graph) {
    std::vector<unsigned> order;
    std::unordered_set<unsigned> seen;
    std::vector<unsigned> stack{graph.entry};

    while (!stack.empty()) {
        const unsigned id = stack.back();
        stack.pop_back();
        if (!seen.insert(id).second) {
            continue;
        }
        order.push_back(id);

        const auto block = graph.blocks.find(id);
        if (block == graph.blocks.end()) {
            continue;
        }
        for (const unsigned successor : block->second.successors) {
            if (graph.blocks.contains(successor)) {
                stack.push_back(successor);
            }
        }
    }
    return order;
}

void ComputeSummary(FunctionGraph& graph, const std::unordered_map<std::string, FunctionGraph>& functions) {
    const std::vector<unsigned> reachable = ReachableBlocks(graph);
    const std::unordered_set<unsigned> reachable_set(reachable.begin(), reachable.end());

    std::unordered_map<unsigned, InitMask> in;
    std::unordered_map<unsigned, InitMask> out;
    in.reserve(reachable.size());
    out.reserve(reachable.size());
    for (const unsigned id : reachable) {
        in.emplace(id, id == graph.entry ? InitMask{0} : kAllInitializers);
        out.emplace(id, kAllInitializers);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const unsigned id : reachable) {
            const auto block_it = graph.blocks.find(id);
            if (block_it == graph.blocks.end()) {
                continue;
            }
            const CfgBlock& block = block_it->second;

            InitMask next_in = 0;
            if (id != graph.entry) {
                bool first = true;
                for (const unsigned predecessor : block.predecessors) {
                    if (!reachable_set.contains(predecessor)) {
                        continue;
                    }
                    const auto predecessor_out = out.find(predecessor);
                    if (predecessor_out == out.end()) {
                        continue;
                    }
                    next_in = first ? predecessor_out->second : (next_in & predecessor_out->second);
                    first = false;
                }
            }

            InitMask next_out = next_in;
            for (const CallSite& call : block.calls) {
                next_out |= InitializerMask(call.name);
                const auto callee = functions.find(call.usr);
                if (callee != functions.end()) {
                    next_out |= callee->second.guarantees;
                }
            }

            if (next_in != in.at(id) || next_out != out.at(id)) {
                in[id] = next_in;
                out[id] = next_out;
                changed = true;
            }
        }
    }

    const auto exit_in = in.find(graph.exit);
    graph.guarantees = exit_in == in.end() ? 0 : exit_in->second;

    std::map<std::string, OperationUse> ops;
    for (const unsigned id : reachable) {
        const auto block_it = graph.blocks.find(id);
        if (block_it == graph.blocks.end()) {
            continue;
        }

        InitMask state = in.at(id);
        for (const CallSite& call : block_it->second.calls) {
            const InitMask initializer = InitializerMask(call.name);
            if (initializer != 0) {
                state |= initializer;
                continue;
            }

            if (const std::optional<std::string_view> required = RequiredInitializer(call.name); required.has_value()) {
                AddOperation(ops, OperationUse{call.name, std::string(*required), call.call_site, state});
            }

            const auto callee = functions.find(call.usr);
            if (callee == functions.end()) {
                continue;
            }
            for (const auto& entry : callee->second.ops) {
                AddOperation(
                    ops,
                    OperationUse{
                        entry.second.operation,
                        entry.second.initializer,
                        entry.second.call_site,
                        state | entry.second.guaranteed_before,
                    });
            }
            state |= callee->second.guarantees;
        }
    }
    graph.ops = std::move(ops);
}

std::vector<Finding> AnalyzeGraph(std::unordered_map<std::string, FunctionGraph>& functions, const std::string& root) {
    constexpr unsigned kMaxPasses = 10000;
    for (unsigned pass = 0; pass < kMaxPasses; ++pass) {
        bool changed = false;
        for (auto& entry : functions) {
            FunctionGraph& graph = entry.second;
            const InitMask previous_guarantees = graph.guarantees;
            const std::map<std::string, OperationUse> previous_ops = graph.ops;
            ComputeSummary(graph, functions);
            if (graph.guarantees != previous_guarantees || graph.ops != previous_ops) {
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }

    std::vector<Finding> findings;
    const auto root_graph = functions.find(root);
    if (root_graph == functions.end()) {
        return findings;
    }

    for (const auto& entry : root_graph->second.ops) {
        const OperationUse& use = entry.second;
        if ((use.guaranteed_before & InitializerMask(use.initializer)) == 0) {
            findings.push_back(Finding{use.operation, use.initializer, use.call_site});
        }
    }
    return findings;
}

}  // namespace

void InitOperationPairing::run(const clang::ast_matchers::MatchFinder::MatchResult& result) {
    findings_.clear();

    const auto* function = result.Nodes.getNodeAs<clang::FunctionDecl>("function");
    if (function == nullptr || function->isInvalidDecl() || !function->hasBody() || result.Context == nullptr) {
        return;
    }

    std::unordered_map<std::string, FunctionGraph> functions = BuildGraph(*function, *result.Context);
    findings_ = AnalyzeGraph(functions, GetUsr(function));
}

}  // namespace tt::lint
