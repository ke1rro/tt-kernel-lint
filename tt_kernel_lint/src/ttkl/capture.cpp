#include "ttkl/capture.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "llvm/ADT/StringExtras.h"

namespace tt::lint {
namespace {

constexpr std::string_view kCommandMarker{"g++ compile cmd: "};

std::string StripAnsi(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\x1b' && index + 1 < input.size() && input[index + 1] == '[') {
            index += 2;
            while (index < input.size() && input[index] != 'm') {
                ++index;
            }
            continue;
        }
        result.push_back(input[index]);
    }

    return result;
}

std::string Trim(std::string_view input) {
    std::size_t begin = 0;
    while (begin < input.size() && llvm::isSpace(input[begin])) {
        ++begin;
    }

    std::size_t end = input.size();
    while (end > begin && llvm::isSpace(input[end - 1])) {
        --end;
    }

    return std::string(input.substr(begin, end - begin));
}

std::vector<std::string> SplitWhitespace(std::string_view input) {
    std::vector<std::string> result;
    std::size_t index = 0;
    while (index < input.size()) {
        while (index < input.size() && llvm::isSpace(input[index])) {
            ++index;
        }
        const std::size_t begin = index;
        while (index < input.size() && !llvm::isSpace(input[index])) {
            ++index;
        }
        if (begin != index) {
            result.emplace_back(input.substr(begin, index - begin));
        }
    }
    return result;
}

// The JIT log joins argv with spaces and does not quote. Recover -D values whose
// template arguments contain spaces, such as -DVALUE=foo<1, int>.
std::vector<std::string> RejoinSplitDefines(std::vector<std::string> arguments) {
    std::vector<std::string> result;
    result.reserve(arguments.size());

    for (std::size_t index = 0; index < arguments.size();) {
        const std::string& argument = arguments[index];
        const auto opens = static_cast<std::size_t>(std::count(argument.begin(), argument.end(), '<'));
        const auto closes = static_cast<std::size_t>(std::count(argument.begin(), argument.end(), '>'));
        if (argument.starts_with("-D") && opens > closes) {
            std::string merged = argument;
            std::size_t next = index + 1;
            bool balanced = false;
            while (next < arguments.size() && !arguments[next].starts_with('-')) {
                merged += " " + arguments[next];
                ++next;
                const auto merged_opens = static_cast<std::size_t>(std::count(merged.begin(), merged.end(), '<'));
                const auto merged_closes = static_cast<std::size_t>(std::count(merged.begin(), merged.end(), '>'));
                if (merged_opens == merged_closes) {
                    balanced = true;
                    break;
                }
            }
            if (balanced) {
                result.push_back(std::move(merged));
                index = next;
                continue;
            }
        }

        result.push_back(argument);
        ++index;
    }

    return result;
}

std::string Unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return std::string(value.substr(1, value.size() - 2));
    }
    return std::string(value);
}

std::optional<std::string> FindDefine(const std::vector<std::string>& arguments, std::string_view name) {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        std::string definition;
        if (arguments[index] == "-D") {
            if (index + 1 == arguments.size()) {
                return std::nullopt;
            }
            definition = arguments[++index];
        } else if (arguments[index].starts_with("-D") && arguments[index].size() > 2) {
            definition = arguments[index].substr(2);
        } else {
            continue;
        }

        const std::size_t equals = definition.find('=');
        const std::string_view key =
            equals == std::string::npos ? std::string_view(definition) : std::string_view(definition).substr(0, equals);
        if (key != name) {
            continue;
        }

        if (equals == std::string::npos) {
            return std::string{};
        }
        return Unquote(definition.substr(equals + 1));
    }
    return std::nullopt;
}

std::string StripSlashes(std::string value) {
    while (!value.empty() && (value.front() == '/' || value.front() == '\\')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }
    return value;
}

std::optional<std::string> FindSourceFile(const std::vector<std::string>& arguments) {
    std::optional<std::string> source;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument == "-o" || argument == "-MF" || argument == "-MT" || argument == "-MQ" || argument == "-MJ" ||
            argument == "-include" || argument == "-imacros" || argument == "-isystem" || argument == "-I") {
            ++index;
            continue;
        }
        if (argument.starts_with('-')) {
            continue;
        }
        if (argument.ends_with(".cc") || argument.ends_with(".cpp")) {
            source = argument;
        }
    }
    return source;
}

std::string FindDirectory(const std::vector<std::string>& arguments) {
    for (std::size_t index = 1; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == "-o") {
            const std::filesystem::path output(arguments[index + 1]);
            const std::filesystem::path parent = output.parent_path();
            if (!parent.empty()) {
                return parent.string();
            }
        }
    }
    return ".";
}

std::optional<ThreadRole> ParseRole(const std::vector<std::string>& arguments) {
    const std::optional<std::string> value = FindDefine(arguments, "COMPILE_FOR_TRISC");
    if (!value.has_value()) {
        return std::nullopt;
    }

    unsigned index = 0;
    if (llvm::StringRef(*value).getAsInteger(10, index)) {
        return std::nullopt;
    }

    switch (index) {
        case 0: return ThreadRole::kUnpack;
        case 1: return ThreadRole::kMath;
        case 2: return ThreadRole::kPack;
        default: return std::nullopt;
    }
}

bool MatchesKernel(
    std::string_view identity, std::string_view name, std::string_view variant, const std::string& requested) {
    if (requested.empty()) {
        return true;
    }

    const std::string wanted = StripSlashes(requested);
    const std::string_view wanted_view(wanted);
    if (wanted_view == identity || wanted_view == name) {
        return true;
    }
    if (!variant.empty() && wanted == std::string(name) + "/" + std::string(variant)) {
        return true;
    }
    return false;
}

}  // namespace

llvm::Expected<std::vector<Context>> ReadCaptureLog(llvm::StringRef path, const CaptureFilter& filter) {
    std::ifstream input(path.str());
    if (!input) {
        return llvm::createStringError(
            std::errc::no_such_file_or_directory, "cannot open capture log '%s'", path.str().c_str());
    }

    std::vector<Context> contexts;
    std::unordered_set<std::string> seen_commands;
    std::string line;

    while (std::getline(input, line)) {
        const std::string clean = StripAnsi(line);
        const std::size_t marker = clean.find(kCommandMarker);
        if (marker == std::string::npos) {
            continue;
        }

        std::string command = Trim(std::string_view(clean).substr(marker + kCommandMarker.size()));
        const std::size_t build_suffix = command.rfind(" (build.cpp:");
        if (build_suffix != std::string::npos) {
            command.erase(build_suffix);
        }
        if (command.empty() || !seen_commands.insert(command).second) {
            continue;
        }

        std::vector<std::string> arguments = RejoinSplitDefines(SplitWhitespace(command));
        if (arguments.empty()) {
            continue;
        }
        if (!FindDefine(arguments, "KERNEL_BUILD").has_value() || FindDefine(arguments, "FW_BUILD").has_value()) {
            continue;
        }

        const std::optional<ThreadRole> role = ParseRole(arguments);
        if (!role.has_value() || (filter.role.has_value() && *filter.role != *role)) {
            continue;
        }

        const std::optional<std::string> full_name = FindDefine(arguments, "FULL_KERNEL_NAME");
        if (!full_name.has_value()) {
            continue;
        }

        const std::string identity = StripSlashes(*full_name);
        if (identity.empty()) {
            continue;
        }

        const std::size_t slash = identity.find('/');
        const std::string name = slash == std::string::npos ? identity : identity.substr(0, slash);
        const std::string variant = slash == std::string::npos ? std::string{} : identity.substr(slash + 1);
        if (!MatchesKernel(identity, name, variant, filter.kernel)) {
            continue;
        }

        const std::optional<std::string> source = FindSourceFile(arguments);
        if (!source.has_value()) {
            continue;
        }

        contexts.emplace_back(
            KernelId{name, variant, *role}, CompileCommand{FindDirectory(arguments), *source, std::move(arguments)});
    }

    return contexts;
}

}  // namespace tt::lint
