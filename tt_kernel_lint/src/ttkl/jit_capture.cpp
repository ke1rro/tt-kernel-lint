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

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

#include "error_util.hpp"

namespace tt::lint {

namespace {

// jit_build/build.cpp: log_info(LogBuildKernels, "    g++ compile cmd: {}", fmt::join(args, " ")).
constexpr std::string_view kCompileCommandMarker{"g++ compile cmd: "};
constexpr char kEscape{'\x1b'};

constexpr std::string_view kCompileOnly{"-c"};
constexpr std::string_view kOutput{"-o"};
constexpr std::string_view kDefinePrefix{"-D"};
constexpr std::string_view kUndefinePrefix{"-U"};

constexpr std::string_view kKernelBuildDefine{"KERNEL_BUILD"};
constexpr std::string_view kTriscDefine{"COMPILE_FOR_TRISC"};
constexpr std::string_view kKernelNameDefine{"FULL_KERNEL_NAME"};
constexpr auto kRoleDefines = std::to_array<std::string_view>({"UCK_CHLKC_UNPACK", "UCK_CHLKC_MATH", "UCK_CHLKC_PACK"});

constexpr const char* kMalformedLineMessage{
    "log line {0}: expected '-c -o <object> <source>' in the compile command, got '{1}'"};
constexpr const char* kRelativeObjectMessage{"log line {0}: the object path '{1}' is not absolute"};
constexpr const char* kSplitArgumentMessage{
    "log line {0}: '{1}' has an unbalanced quote, the log split an argument that contains a space"};
constexpr const char* kUnknownTriscMessage{"'{0}': COMPILE_FOR_TRISC={1} is not 0, 1 or 2"};
constexpr const char* kRoleDefineMessage{"'{0}': COMPILE_FOR_TRISC={1} requires {2} and no other UCK_CHLKC_* define"};
constexpr const char* kMissingNameMessage{"'{0}': a compute kernel command without FULL_KERNEL_NAME"};
constexpr const char* kMalformedNameMessage{"'{0}': FULL_KERNEL_NAME={1} is not \"<kernel>/<variant>/\""};

// tt-logger wraps parts of a line in SGR escape sequences when colour is on.
[[nodiscard]] std::string StripColour(std::string_view text) {
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

// tt-logger appends the source location as a separate token, e.g. " (build.cpp:877)".
[[nodiscard]] llvm::StringRef StripSourceLocation(llvm::StringRef text) {
    text = text.rtrim();
    if (!text.ends_with(")")) {
        return text;
    }
    const std::size_t open{text.rfind('(')};
    if (open == llvm::StringRef::npos || open == 0 || text[open - 1] != ' ') {
        return text;
    }
    const auto [file, line] = text.slice(open + 1, text.size() - 1).rsplit(':');
    const bool is_location{
        !file.empty() && !file.contains(' ') && !line.empty() &&
        line.find_first_not_of("0123456789") == llvm::StringRef::npos};
    return is_location ? text.take_front(open).rtrim() : text;
}

[[nodiscard]] std::vector<std::string> SplitArguments(llvm::StringRef text) {
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
[[nodiscard]] llvm::Expected<CompileCommand> MakeCommand(std::vector<std::string> arguments, std::size_t line_number) {
    for (const std::string& argument : arguments) {
        if (llvm::count(argument, '"') % 2 != 0) {
            return MakeError(kSplitArgumentMessage, line_number, argument);
        }
    }
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
    return MakeError(kMalformedLineMessage, line_number, llvm::join(arguments, " "));
}

// Value of NAME after every -D and -U in order, as GCC applies them; an empty value for -DNAME.
[[nodiscard]] std::optional<std::string_view> FindDefine(
    std::span<const std::string> arguments, std::string_view name) {
    std::optional<std::string_view> value;
    for (std::size_t index{0}; index < arguments.size(); ++index) {
        std::string_view macro{arguments[index]};
        const bool is_define{macro.starts_with(kDefinePrefix)};
        if (!is_define && !macro.starts_with(kUndefinePrefix)) {
            continue;
        }
        macro.remove_prefix(kDefinePrefix.size());
        if (macro.empty() && index + 1 < arguments.size()) {
            macro = arguments[++index];
        }
        const std::size_t equals{macro.find('=')};
        if (macro.substr(0, equals) != name) {
            continue;
        }
        if (is_define) {
            value = equals == std::string_view::npos ? std::string_view{} : macro.substr(equals + 1);
        } else {
            value.reset();
        }
    }
    return value;
}

[[nodiscard]] llvm::Expected<ThreadRole> IdentifyRole(const CompileCommand& command, std::string_view trisc) {
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
[[nodiscard]] llvm::Expected<std::pair<std::string, std::string>> IdentifyKernel(const CompileCommand& command) {
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
