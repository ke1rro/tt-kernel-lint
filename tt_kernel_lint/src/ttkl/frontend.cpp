#include "ttkl/frontend.hpp"

#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace tt::lint {

namespace {

constexpr const char* kOverlayMessage{"cannot overlay '{0}': the path is not absolute or is overlaid twice"};
constexpr const char* kWorkingDirectoryMessage{"cannot use '{0}' as the working directory: {1}"};
constexpr const char* kClangFailedMessage{"Clang failed on '{0}' in '{1}'"};

template <typename... Values>
llvm::Error MakeError(const char* format, Values&&... values) {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), llvm::formatv(format, std::forward<Values>(values)...).str());
}

// Private to one run, so concurrent runs never share a working directory. Overlays are copied, so they need not
// outlive an AST that keeps the file system alive.
llvm::Expected<llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>> MakeFileSystem(
    const std::string& working_directory, std::span<const FileOverlay> overlays) {
    auto in_memory{llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>()};
    for (const auto& [path, contents] : overlays) {
        if (!in_memory->addFile(path, /*ModificationTime=*/0, llvm::MemoryBuffer::getMemBufferCopy(contents, path))) {
            return MakeError(kOverlayMessage, path);
        }
    }
    auto overlay{llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::createPhysicalFileSystem())};
    overlay->pushOverlay(std::move(in_memory));
    if (const std::error_code error{overlay->setCurrentWorkingDirectory(working_directory)}) {
        return MakeError(kWorkingDirectoryMessage, working_directory, error.message());
    }
    return overlay;
}

// clang::tooling builds ASTs the same way, but keeps its action private.
class AstBuilder final : public clang::tooling::ToolAction {
    std::unique_ptr<clang::ASTUnit> ast_;

public:
    bool runInvocation(
        std::shared_ptr<clang::CompilerInvocation> invocation,
        clang::FileManager* files,
        std::shared_ptr<clang::PCHContainerOperations> pch_container_operations,
        clang::DiagnosticConsumer* diagnostic_consumer) override {
        llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics{clang::CompilerInstance::createDiagnostics(
            files->getVirtualFileSystem(),
            &invocation->getDiagnosticOpts(),
            diagnostic_consumer,
            /*ShouldOwnClient=*/false)};
        ast_ = clang::ASTUnit::LoadFromCompilerInvocation(
            std::move(invocation), std::move(pch_container_operations), std::move(diagnostics), files);
        return ast_ != nullptr;
    }

    [[nodiscard]] std::unique_ptr<clang::ASTUnit> TakeAst() noexcept { return std::move(ast_); }
};

}  // namespace

llvm::Error RunClangTool(
    const CompileCommand& command,
    std::span<const FileOverlay> overlays,
    clang::tooling::ToolAction& action,
    clang::DiagnosticConsumer* diagnostics) {
    llvm::Expected<llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>> file_system{
        MakeFileSystem(command.directory, overlays)};
    if (!file_system) {
        return file_system.takeError();
    }
    auto files{llvm::makeIntrusiveRefCnt<clang::FileManager>(clang::FileSystemOptions{}, std::move(*file_system))};
    clang::tooling::ToolInvocation invocation(
        command.arguments, &action, files.get(), std::make_shared<clang::PCHContainerOperations>());
    invocation.setDiagnosticConsumer(diagnostics);
    if (!invocation.run()) {
        return MakeError(kClangFailedMessage, command.source_file, command.directory);
    }
    return llvm::Error::success();
}

llvm::Expected<std::unique_ptr<clang::ASTUnit>> BuildAst(
    const CompileCommand& command, std::span<const FileOverlay> overlays, clang::DiagnosticConsumer* diagnostics) {
    AstBuilder builder;
    if (llvm::Error error{RunClangTool(command, overlays, builder, diagnostics)}) {
        return std::move(error);
    }
    return builder.TakeAst();
}

}  // namespace tt::lint
