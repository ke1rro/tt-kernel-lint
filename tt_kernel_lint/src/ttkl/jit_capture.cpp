#include "ttkl/jit_capture.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"

namespace tt::lint {

namespace {

// jit_build/build.cpp: log_info(LogBuildKernels, "    g++ compile cmd: {}", fmt::join(args, " ")).
constexpr std::string_view kCompileCommandMarker{"g++ compile cmd: "};
constexpr char kEscape{'\x1b'};

constexpr std::string_view kCompileOnly{"-c"};
constexpr std::string_view kOutput{"-o"};
constexpr std::string_view kDefinePrefix{"-D"};

constexpr std::string_view kKernelBuildDefine{"KERNEL_BUILD"};
constexpr std::string_view kTriscDefine{"COMPILE_FOR_TRISC"};
constexpr std::string_view kKernelNameDefine{"FULL_KERNEL_NAME"};
constexpr auto kRoleDefines = std::to_array<std::string_view>({"UCK_CHLKC_UNPACK", "UCK_CHLKC_MATH", "UCK_CHLKC_PACK"});

constexpr const char* kMalformedLineMessage{
    "log line {0}: expected '-c -o <object> <source>' in the compile command, got '{1}'"};
constexpr const char* kRelativeObjectMessage{"log line {0}: the object path '{1}' is not absolute"};
constexpr const char* kUnknownTriscMessage{"'{0}': COMPILE_FOR_TRISC={1} is not 0, 1 or 2"};
constexpr const char* kRoleDefineMessage{"'{0}': COMPILE_FOR_TRISC={1} requires {2} and no other UCK_CHLKC_* define"};
constexpr const char* kMissingNameMessage{"'{0}': a compute kernel command without FULL_KERNEL_NAME"};
constexpr const char* kMalformedNameMessage{"'{0}': FULL_KERNEL_NAME={1} is not \"<kernel>/<variant>/\""};

template <typename... Values>
llvm::Error MakeError(const char* format, Values&&... values) {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), llvm::formatv(format, std::forward<Values>(values)...).str());
}

// tt-logger wraps parts of a line in SGR escape sequences when colour is on.
std::string StripColour(std::string_view text) {
    std::string plain;
    plain.reserve(text.size());
    for (std::size_t index{0}; index < text.size(); ++index) {
        if (text[index] == kEscape && index + 1 < text.size() && text[index + 1] == '[') {
            std::size_t end{index + 2};
            while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == ';')) {
                ++end;
            }
            if (end < text.size() && text[end] == 'm') {
                index = end;
                continue;
            }
        }
        plain += text[index];
    }
    return plain;
}

// tt-logger appends the source location, e.g. "(build.cpp:877)".
llvm::StringRef StripSourceLocation(llvm::StringRef text) {
    text = text.rtrim();
    if (!text.ends_with(")")) {
        return text;
    }
    const std::size_t open{text.rfind('(')};
    if (open == llvm::StringRef::npos) {
        return text;
    }
    const auto [file, line] = text.slice(open + 1, text.size() - 1).rsplit(':');
    const bool is_location{
        !file.empty() && !file.contains(' ') && !line.empty() &&
        line.find_first_not_of("0123456789") == llvm::StringRef::npos};
    return is_location ? text.take_front(open).rtrim() : text;
}

std::vector<std::string> SplitArguments(llvm::StringRef text) {
    std::vector<std::string> arguments;
    llvm::SmallVector<llvm::StringRef, 128> tokens;
    text.split(tokens, ' ', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
    arguments.reserve(tokens.size());
    for (const llvm::StringRef token : tokens) {
        arguments.emplace_back(token);
    }
    return arguments;
}

// build_gpp_argv emits "-c -o <object> <source> -MF <depfile>", and the compiler runs in the object's directory.
llvm::Expected<CompileCommand> MakeCommand(std::vector<std::string> arguments, std::size_t line_number) {
    for (std::size_t index{arguments.size()}; index-- > 1;) {
        if (arguments[index] != kOutput || arguments[index - 1] != kCompileOnly || index + 2 >= arguments.size()) {
            continue;
        }
        const llvm::StringRef object{arguments[index + 1]};
        if (!llvm::sys::path::is_absolute(object)) {
            return MakeError(kRelativeObjectMessage, line_number, object);
        }
        std::string directory{llvm::sys::path::parent_path(object)};
        std::string source{arguments[index + 2]};
        return CompileCommand{std::move(directory), std::move(source), std::move(arguments)};
    }
    std::string text;
    for (const std::string& argument : arguments) {
        text += text.empty() ? "" : " ";
        text += argument;
    }
    return MakeError(kMalformedLineMessage, line_number, text);
}

// Value of -DNAME=VALUE or -D NAME=VALUE; an empty value for -DNAME.
std::optional<std::string_view> FindDefine(std::span<const std::string> arguments, std::string_view name) {
    for (std::size_t index{0}; index < arguments.size(); ++index) {
        std::string_view definition{arguments[index]};
        if (definition == kDefinePrefix && index + 1 < arguments.size()) {
            definition = arguments[++index];
        } else if (definition.starts_with(kDefinePrefix)) {
            definition.remove_prefix(kDefinePrefix.size());
        } else {
            continue;
        }
        const std::size_t equals{definition.find('=')};
        if (definition.substr(0, equals) == name) {
            return equals == std::string_view::npos ? std::string_view{} : definition.substr(equals + 1);
        }
    }
    return std::nullopt;
}

llvm::Expected<ThreadRole> IdentifyRole(const CompileCommand& command, std::string_view trisc) {
    std::size_t index{0};
    if (trisc == "0") {
        index = 0;
    } else if (trisc == "1") {
        index = 1;
    } else if (trisc == "2") {
        index = 2;
    } else {
        return MakeError(kUnknownTriscMessage, command.directory, llvm::StringRef{trisc});
    }
    for (std::size_t other{0}; other < kRoleDefines.size(); ++other) {
        if (FindDefine(command.arguments, kRoleDefines[other]).has_value() != (other == index)) {
            return MakeError(
                kRoleDefineMessage, command.directory, llvm::StringRef{trisc}, llvm::StringRef{kRoleDefines[index]});
        }
    }
    return static_cast<ThreadRole>(index);
}

// FULL_KERNEL_NAME="<kernel>/<variant>/", see Program::compile and JitBuildState::export_target_recipe.
llvm::Expected<std::pair<std::string, std::string>> IdentifyKernel(const CompileCommand& command) {
    const std::optional<std::string_view> define{FindDefine(command.arguments, kKernelNameDefine)};
    if (!define) {
        return MakeError(kMissingNameMessage, command.directory);
    }
    llvm::StringRef full_name{*define};
    if (full_name.size() >= 2 && full_name.starts_with("\"") && full_name.ends_with("\"")) {
        full_name = full_name.drop_front().drop_back();
    }
    const auto [name, variant] = full_name.rtrim('/').rsplit('/');
    if (name.empty() || variant.empty()) {
        return MakeError(kMalformedNameMessage, command.directory, llvm::StringRef{*define});
    }
    return std::pair{std::string{name}, std::string{variant}};
}

}  // namespace

llvm::Expected<std::vector<CompileCommand>> ParseJitBuildLog(std::string_view log) {
    std::vector<CompileCommand> commands;
    llvm::StringSet<> seen;
    llvm::StringRef rest{log};
    for (std::size_t line_number{1}; !rest.empty(); ++line_number) {
        auto [line, next] = rest.split('\n');
        rest = next;
        const std::size_t marker{line.find(kCompileCommandMarker)};
        if (marker == llvm::StringRef::npos) {
            continue;
        }
        const std::string plain{StripColour(line.drop_front(marker + kCompileCommandMarker.size()))};
        const llvm::StringRef command_text{StripSourceLocation(plain)};
        if (!seen.insert(command_text).second) {
            continue;
        }
        llvm::Expected<CompileCommand> command{MakeCommand(SplitArguments(command_text), line_number)};
        if (!command) {
            return command.takeError();
        }
        commands.push_back(std::move(*command));
    }
    return commands;
}

llvm::Expected<std::optional<Context>> IdentifyComputeContext(CompileCommand command) {
    const std::optional<std::string_view> trisc{FindDefine(command.arguments, kTriscDefine)};
    if (!FindDefine(command.arguments, kKernelBuildDefine) || !trisc) {
        return std::nullopt;
    }
    llvm::Expected<ThreadRole> role{IdentifyRole(command, *trisc)};
    if (!role) {
        return role.takeError();
    }
    llvm::Expected<std::pair<std::string, std::string>> kernel{IdentifyKernel(command)};
    if (!kernel) {
        return kernel.takeError();
    }
    auto& [name, variant] = *kernel;
    return Context{KernelId{std::move(name), std::move(variant), *role}, std::move(command)};
}

}  // namespace tt::lint
