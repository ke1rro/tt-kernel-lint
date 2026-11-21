#include "ttkl/sfpi_profile.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tt::lint {
namespace {

namespace fs = std::filesystem;

struct TargetInfo {
    std::string_view triple;
    std::string_view march;
    std::string_view mabi;
    std::string_view multilib;
};

std::optional<TargetInfo> FindTarget(std::string_view mcpu) {
    if (mcpu == "tt-wh" || mcpu == "tt-wh-tensix") {
        return TargetInfo{"riscv32-unknown-elf", "rv32im", "ilp32", "wh-ilp32"};
    }
    if (mcpu == "tt-bh" || mcpu == "tt-bh-tensix") {
        return TargetInfo{"riscv32-unknown-elf", "rv32im", "ilp32", "bh-ilp32"};
    }
    if (mcpu == "tt-qsr32-tensix") {
        return TargetInfo{"riscv32-unknown-elf", "rv32im", "ilp32", "qsr32-ilp32"};
    }
    if (mcpu == "tt-qsr64-rocc") {
        return TargetInfo{"riscv64-unknown-elf", "rv64im", "lp64", "qsr64-lp64"};
    }
    return std::nullopt;
}

std::vector<unsigned> VersionParts(std::string_view version) {
    std::vector<unsigned> parts;
    std::size_t begin = 0;
    while (begin < version.size()) {
        std::size_t end = version.find('.', begin);
        if (end == std::string_view::npos) {
            end = version.size();
        }
        unsigned value = 0;
        if (llvm::StringRef(version.substr(begin, end - begin)).getAsInteger(10, value)) {
            return {};
        }
        parts.push_back(value);
        begin = end + 1;
    }
    return parts;
}

bool VersionLess(std::string_view left, std::string_view right) {
    const std::vector<unsigned> lhs = VersionParts(left);
    const std::vector<unsigned> rhs = VersionParts(right);
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

void AddDirectory(std::vector<std::string>& directories, const fs::path& path) {
    if (fs::is_directory(path)) {
        directories.push_back(path.string());
    }
}

std::optional<fs::path> FindCompilerRoot(const fs::path& compiler) {
    fs::path root = compiler.parent_path().parent_path();
    if (fs::is_directory(root / "riscv-tt-elf" / "include")) {
        return root;
    }
    return std::nullopt;
}

std::string LatestCxxVersion(const fs::path& cxx_root) {
    std::vector<std::string> versions;
    if (!fs::is_directory(cxx_root)) {
        return {};
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(cxx_root)) {
        if (entry.is_directory()) {
            versions.push_back(entry.path().filename().string());
        }
    }
    if (versions.empty()) {
        return {};
    }
    return *std::max_element(versions.begin(), versions.end(), VersionLess);
}

void AddInt32Overrides(std::vector<std::string>& flags) {
    flags.insert(
        flags.end(),
        {"-U__INT32_TYPE__",
         "-D__INT32_TYPE__=long int",
         "-U__UINT32_TYPE__",
         "-D__UINT32_TYPE__=long unsigned int",
         "-U__INT_LEAST32_TYPE__",
         "-D__INT_LEAST32_TYPE__=long int",
         "-U__UINT_LEAST32_TYPE__",
         "-D__UINT_LEAST32_TYPE__=long unsigned int"});
}

}  // namespace

ToolchainProfile MakeSfpiProfile(llvm::StringRef compiler, llvm::StringRef mcpu) {
    ToolchainProfile profile;
    profile.name = "sfpi";

    const std::optional<TargetInfo> target = FindTarget(mcpu.str());
    if (target.has_value()) {
        profile.cpus.push_back(
            CpuMapping{
                mcpu.str(),
                std::string(target->triple),
                std::string(target->march),
                std::string(target->mabi),
            });
    }

    const std::optional<fs::path> compiler_root = FindCompilerRoot(fs::path(compiler.str()));
    if (compiler_root.has_value()) {
        const fs::path cxx_root = *compiler_root / "riscv-tt-elf" / "include" / "c++";
        const std::string version = LatestCxxVersion(cxx_root);
        if (!version.empty()) {
            const fs::path gcc_include = *compiler_root / "lib" / "gcc" / "riscv-tt-elf" / version / "include";
            AddDirectory(profile.system_include_directories, cxx_root / version);
            if (target.has_value()) {
                AddDirectory(
                    profile.system_include_directories,
                    cxx_root / version / "riscv-tt-elf" / std::string(target->multilib));
            }
            AddDirectory(profile.system_include_directories, cxx_root / version / "backward");
            AddDirectory(profile.system_include_directories, gcc_include);
            AddDirectory(profile.system_include_directories, *compiler_root / "riscv-tt-elf" / "include");

            profile.clang_flags.insert(profile.clang_flags.end(), {"-nostdinc++", "-nostdlibinc"});
            if (target.has_value() && target->mabi == "ilp32") {
                AddInt32Overrides(profile.clang_flags);
            }
        }
    }

    profile.clang_flags.insert(
        profile.clang_flags.end(), {"-Wno-unknown-attributes", "-Wno-missing-template-arg-list-after-template-kw"});
    return profile;
}

}  // namespace tt::lint
