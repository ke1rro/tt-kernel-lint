#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Analysis/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::tooling;
using namespace llvm;
using namespace clang::ast_matchers;

static cl::OptionCategory tt_kernel_init_static_check("tt-kernel-init-static-check options");

auto function_declaration_matcher = functionDecl(isDefinition(), hasName("kernel_main")).bind("function_declaration");

class OpInitCallPrinter : public MatchFinder::MatchCallback {
public:
    virtual void run(const MatchFinder::MatchResult& Result) {
        if (const auto FD = Result.Nodes.getNodeAs<clang::FunctionDecl>("function_declaration")) {
            if (!FD || !FD->hasBody()) {
                return;
            }
            std::unique_ptr<clang::CFG> cfg =
                clang::CFG::buildCFG(FD, FD->getBody(), Result.Context, clang::CFG::BuildOptions());
            if (!cfg) {
                return;
            }

            for (const auto cfg_block: *cfg) {
            for (const auto block_element: *cfg_block) {
                block_element->dump();
                if (auto statement = block_element.getAs<clang::CFGStmt>()){
                    // statement->dump();
                }
            }

            }
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