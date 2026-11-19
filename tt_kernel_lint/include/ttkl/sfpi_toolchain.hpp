#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "llvm/Support/Error.h"

#include "ttkl/target_profile.hpp"

namespace llvm::vfs {
class FileSystem;
}  // namespace llvm::vfs

namespace tt::lint {

/** In-memory contents of one file, visible to Clang at an absolute path instead of the file on disk. */
struct FileOverlay {
    std::string path;
    std::string contents;
};

/** An SFPI installation verified against a pinned profile. Immutable once opened. */
class SfpiToolchain final {
    const ToolchainProfile* profile_;
    std::string root_;
    std::string compiler_;
    std::vector<FileOverlay> overlays_;

public:
    /**
     * Opens the SFPI installation whose GCC driver is `compiler`, an absolute <root>/compiler/bin/<driver> path.
     *
     * Every system include directory of the profile must exist and every pinned header must match its digest; the
     * profile's header patches are applied in memory.
     *
     * @note `profile` must outlive the toolchain.
     */
    [[nodiscard]] static llvm::Expected<SfpiToolchain> OpenForCompiler(
        std::string_view compiler, const ToolchainProfile& profile, llvm::vfs::FileSystem& file_system);

    [[nodiscard]] const ToolchainProfile& profile() const noexcept { return *profile_; }
    [[nodiscard]] const std::string& root() const noexcept { return root_; }
    [[nodiscard]] const std::string& compiler() const noexcept { return compiler_; }

    /** Patched SFPI headers that replace the installed ones while Clang parses. */
    [[nodiscard]] std::span<const FileOverlay> overlays() const noexcept { return overlays_; }

private:
    SfpiToolchain(
        const ToolchainProfile& profile, std::string root, std::string compiler, std::vector<FileOverlay> overlays);
};

}  // namespace tt::lint
