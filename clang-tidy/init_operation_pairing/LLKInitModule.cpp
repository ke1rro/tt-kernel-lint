#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/AST/Decl.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Analysis/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Casting.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include <vector>
#include <unordered_map>
#include <string>

using namespace clang::tooling;
using namespace llvm;
using namespace clang::ast_matchers;

static cl::OptionCategory tt_kernel_init_static_check("tt-kernel-init-static-check options");

auto function_declaration_matcher = functionDecl(isDefinition(), hasName("kernel_main")).bind("function_declaration");

SmallString<128> get_usr(const clang::FunctionDecl* func_declaration) {
    llvm::SmallString<128> usr;
    clang::index::generateUSRForDecl(func_declaration, usr);
    return usr;
}

std::string get_location_string(const clang::SourceManager& SM, clang::SourceLocation loc) {
    loc = SM.getExpansionLoc(loc);
    clang::PresumedLoc p = SM.getPresumedLoc(loc);
    if (p.isInvalid()) {
        return "<invalid>";
    }
    return std::string(p.getFilename()) + ":" + std::to_string(p.getLine()) + ":" + std::to_string(p.getColumn());
}

struct CallInfo {
    SmallString<128> usr;
    std::string name;
    std::string definition_site;
    std::string call_site;

    CallInfo(SmallString<128> usr_, std::string name_, std::string definition_site_, std::string call_site_) :
        usr{usr_}, name{name_}, definition_site{definition_site_}, call_site{call_site_} {};
};

struct BlockInfo {
    std::vector<CallInfo> calls;
    std::vector<unsigned> preds;
};

using InfoMapping = std::unordered_map<unsigned, BlockInfo>;
using FunctionToInfo = std::unordered_map<std::string, unsigned>;

FunctionToInfo func_info_map;

class OpInitCallPrinter : public MatchFinder::MatchCallback {
public:
    virtual void run(const MatchFinder::MatchResult& Result) {
        if (const auto FD = Result.Nodes.getNodeAs<clang::FunctionDecl>("function_declaration")) {
            if (!FD || !FD->hasBody()) {
                return;
            }
            auto decl_id = get_usr(FD).str().str();
            if (func_info_map.count(decl_id)) {
                return;
            }
            if (FD->isInvalidDecl()) {
                return;
            }
            const clang::SourceManager& SM = *Result.SourceManager;
            std::unique_ptr<clang::CFG> cfg =
                clang::CFG::buildCFG(FD, FD->getBody(), Result.Context, clang::CFG::BuildOptions());
            if (!cfg) {
                return;
            }

            InfoMapping info_map;
            for (const auto cfg_block : *cfg) {
                BlockInfo block_info;
                for (const auto cfg_block_element : *cfg_block) {
                    if (auto cfg_statement = cfg_block_element.getAs<clang::CFGStmt>()) {
                        if (auto call_expr = dyn_cast<clang::CallExpr>(cfg_statement->getStmt())) {
                            if (auto func_declaration = call_expr->getDirectCallee()) {
                                auto usr = get_usr(func_declaration);
                                auto name_info = func_declaration->getNameInfo();
                                auto name = name_info.getName().getAsString();
                                auto definition_site = get_location_string(SM, func_declaration->getBeginLoc());
                                auto call_site = get_location_string(SM, call_expr->getBeginLoc());

                                block_info.calls.emplace_back(CallInfo(
                                    std::move(usr), std::move(name), std::move(definition_site), std::move(call_site)));
                            }
                        }
                    }
                }
                for (auto pred : cfg_block->preds()) {
                    if (pred) {
                        block_info.preds.push_back(pred->getBlockID());
                    }
                }
                info_map[cfg_block->getBlockID()] = std::move(block_info);
            }

            func_info_map[decl_id] = std::move(info_map);
        }
    }
};

int main(int argc, const char** argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, tt_kernel_init_static_check);
    if (!ExpectedParser) {
        // Fail gracefully for unsupported options.
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser& OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    for (auto& i : OptionsParser.getSourcePathList()) {
        llvm::outs() << i << "\n";
    }

    OpInitCallPrinter Printer;
    MatchFinder Finder;
    Finder.addMatcher(function_declaration_matcher, &Printer);

    return Tool.run(newFrontendActionFactory(&Finder).get());
}