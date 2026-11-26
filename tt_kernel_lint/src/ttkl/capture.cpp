#include "ttkl/capture.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"

#include "ttkl/jit_capture.hpp"

namespace tt::lint {

bool MatchesCaptureFilter(const KernelId& kernel, const CaptureFilter& filter) {
    if (filter.role && kernel.role != *filter.role) {
        return false;
    }
    const llvm::StringRef requested{llvm::StringRef{filter.kernel}.rtrim('/')};
    return requested.empty() || requested == kernel.name || requested == kernel.name + "/" + kernel.variant;
}

llvm::Expected<std::vector<Context>> ReadCaptureLog(llvm::StringRef path, const CaptureFilter& filter) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> log{llvm::MemoryBuffer::getFile(path)};
    if (!log) {
        return llvm::createStringError(log.getError(), "cannot open capture log '%s'", path.str().c_str());
    }
    llvm::Expected<std::vector<CompileCommand>> commands{ParseJitBuildLog((*log)->getBuffer())};
    if (!commands) {
        return commands.takeError();
    }
    std::vector<Context> contexts;
    for (CompileCommand& command : *commands) {
        llvm::Expected<std::optional<Context>> context{IdentifyComputeContext(std::move(command))};
        if (!context) {
            return context.takeError();
        }
        if (*context && MatchesCaptureFilter((*context)->kernel(), filter)) {
            contexts.push_back(std::move(**context));
        }
    }
    return contexts;
}

}  // namespace tt::lint
