#include "ttkl/sfpi_toolchain.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/VirtualFileSystem.h"

#include "error_util.hpp"

namespace tt::lint {

namespace {

constexpr std::string_view kBinDirectory{"bin"};
constexpr std::string_view kCompilerDirectory{"compiler"};
constexpr std::string_view kIncludeDirectory{"include"};

constexpr const char* kRelativeCompilerMessage{"the SFPI compiler path '{0}' is not absolute"};
constexpr const char* kCompilerNameMessage{"expected the SFPI compiler {0}, got '{1}'"};
constexpr const char* kCompilerLayoutMessage{"'{0}' is not in <sfpi root>/compiler/bin"};
constexpr const char* kMissingDirectoryMessage{"SFPI at '{0}' does not match profile '{1}': missing directory '{2}'"};
constexpr const char* kUnreadableHeaderMessage{"SFPI at '{0}': cannot read '{1}': {2}"};
constexpr const char* kDigestMismatchMessage{
    "SFPI at '{0}' is not release {1}: '{2}' has SHA-256 {3}, expected {4}. Use the SFPI release pinned by "
    "tt_metal/sfpi-version or add a verified profile"};
constexpr const char* kUnpinnedPatchMessage{"profile '{0}' patches '{1}', which has no pinned digest"};
constexpr const char* kPatchMatchMessage{
    "profile '{0}': the patch for '{1}' must match exactly once, matched {2} times"};

[[nodiscard]] std::string JoinPath(llvm::StringRef base, llvm::StringRef first, llvm::StringRef second = {}) {
    llvm::SmallString<256> path{base};
    llvm::sys::path::append(path, first);
    if (!second.empty()) {
        llvm::sys::path::append(path, second);
    }
    return std::string{path};
}

[[nodiscard]] std::string Sha256Hex(llvm::StringRef contents) {
    const std::array<std::uint8_t, 32> digest{llvm::SHA256::hash(llvm::arrayRefFromStringRef(contents))};
    return llvm::toHex(digest, /*LowerCase=*/true);
}

[[nodiscard]] bool IsPinned(const ToolchainProfile& profile, std::string_view file) {
    for (const HeaderDigest& header : profile.headers) {
        if (header.file == file) {
            return true;
        }
    }
    return false;
}

// Applies every patch of `profile` that targets `file`; std::nullopt when none does.
[[nodiscard]] llvm::Expected<std::optional<std::string>> PatchHeader(
    const ToolchainProfile& profile, std::string_view file, llvm::StringRef contents) {
    std::optional<std::string> patched;
    for (const HeaderPatch& patch : profile.patches) {
        if (patch.file != file) {
            continue;
        }
        std::string& text{patched ? *patched : patched.emplace(contents)};
        const std::size_t matches{llvm::StringRef{text}.count(llvm::StringRef{patch.original})};
        if (matches != 1) {
            return MakeError(kPatchMatchMessage, llvm::StringRef{profile.name}, llvm::StringRef{file}, matches);
        }
        text.replace(text.find(patch.original), patch.original.size(), patch.replacement);
    }
    return patched;
}

}  // namespace

SfpiToolchain::SfpiToolchain(
    const ToolchainProfile& profile, std::string root, std::string compiler, std::vector<FileOverlay> overlays) :
    profile_(&profile), root_(std::move(root)), compiler_(std::move(compiler)), overlays_(std::move(overlays)) {}

llvm::Expected<SfpiToolchain> SfpiToolchain::OpenForCompiler(
    std::string_view compiler, const ToolchainProfile& profile, llvm::vfs::FileSystem& file_system) {
    const llvm::StringRef compiler_path{compiler};
    if (!llvm::sys::path::is_absolute(compiler_path)) {
        return MakeError(kRelativeCompilerMessage, compiler_path);
    }
    if (llvm::sys::path::filename(compiler_path) != llvm::StringRef{profile.compiler}) {
        return MakeError(kCompilerNameMessage, llvm::StringRef{profile.compiler}, compiler_path);
    }
    const llvm::StringRef bin_directory{llvm::sys::path::parent_path(compiler_path)};
    const llvm::StringRef compiler_directory{llvm::sys::path::parent_path(bin_directory)};
    if (llvm::sys::path::filename(bin_directory) != llvm::StringRef{kBinDirectory} ||
        llvm::sys::path::filename(compiler_directory) != llvm::StringRef{kCompilerDirectory}) {
        return MakeError(kCompilerLayoutMessage, compiler_path);
    }
    std::string root{llvm::sys::path::parent_path(compiler_directory)};

    for (const CpuMapping& cpu : profile.cpus) {
        for (const std::string_view directory : cpu.system_include_directories) {
            const llvm::ErrorOr<llvm::vfs::Status> status{file_system.status(JoinPath(root, directory))};
            if (!status || !status->isDirectory()) {
                return MakeError(
                    kMissingDirectoryMessage, root, llvm::StringRef{profile.name}, llvm::StringRef{directory});
            }
        }
    }
    for (const HeaderPatch& patch : profile.patches) {
        if (!IsPinned(profile, patch.file)) {
            return MakeError(kUnpinnedPatchMessage, llvm::StringRef{profile.name}, llvm::StringRef{patch.file});
        }
    }

    std::vector<FileOverlay> overlays;
    for (const auto& [file, sha256] : profile.headers) {
        std::string path{JoinPath(root, kIncludeDirectory, file)};
        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer{file_system.getBufferForFile(path)};
        if (!buffer) {
            return MakeError(kUnreadableHeaderMessage, root, path, buffer.getError().message());
        }
        const llvm::StringRef contents{(*buffer)->getBuffer()};
        if (const std::string actual{Sha256Hex(contents)}; actual != sha256) {
            return MakeError(
                kDigestMismatchMessage,
                root,
                llvm::StringRef{profile.sfpi_version},
                llvm::StringRef{file},
                actual,
                llvm::StringRef{sha256});
        }
        llvm::Expected<std::optional<std::string>> patched{PatchHeader(profile, file, contents)};
        if (!patched) {
            return patched.takeError();
        }
        if (*patched) {
            overlays.push_back(FileOverlay{std::move(path), std::move(**patched)});
        }
    }
    return SfpiToolchain(profile, std::move(root), std::string{compiler}, std::move(overlays));
}

}  // namespace tt::lint
