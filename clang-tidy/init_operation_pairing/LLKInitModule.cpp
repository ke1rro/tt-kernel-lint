#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Frontend/FrontendActions.h"
#include "llvm/Support/CommandLine.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::tooling;
using namespace llvm;
using namespace clang::ast_matchers;

static cl::OptionCategory tt_kernel_init_static_check("tt-kernel-init-static-check options");

StatementMatcher OpInitMatcher = callExpr(callee(functionDecl(matchesName("add_init")))).bind("opInitCall");

class OpInitCallPrinter : public MatchFinder::MatchCallback {
public:
    virtual void run(const MatchFinder::MatchResult& Result) {
        if (const clang::CallExpr* CE = Result.Nodes.getNodeAs<clang::CallExpr>("opInitCall")) {
            CE->getBeginLoc().print(llvm::outs(), *Result.SourceManager);
            llvm::outs() << "\n";
            CE->dump();
        }
    }
};

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, tt_kernel_init_static_check);
    if (!ExpectedParser) {
        // Fail gracefully for unsupported options.
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser& OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());

    OpInitCallPrinter Printer;
    MatchFinder Finder;
    Finder.addMatcher(OpInitMatcher, &Printer);

    return Tool.run(newFrontendActionFactory(&Finder).get());
}