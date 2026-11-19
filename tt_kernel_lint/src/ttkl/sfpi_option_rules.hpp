#pragma once

#include <array>
#include <string_view>

namespace tt::lint::rules {

inline constexpr std::string_view kCompilerLauncher{"ccache"};
inline constexpr std::string_view kClangDriver{"clang++"};

inline constexpr std::string_view kGccStandard{"-std=c++17"};
inline constexpr std::string_view kClangStandard{"-std=c++20"};
inline constexpr std::string_view kStandardPrefix{"-std="};

inline constexpr std::string_view kCpuPrefix{"-mcpu="};
inline constexpr std::string_view kTargetPrefix{"--target="};
inline constexpr std::string_view kMarchPrefix{"-march="};
inline constexpr std::string_view kMabiPrefix{"-mabi="};

inline constexpr std::string_view kCompileOnly{"-c"};
inline constexpr std::string_view kSyntaxOnly{"-fsyntax-only"};
inline constexpr std::string_view kOptimizeFast{"-Ofast"};
inline constexpr std::string_view kOptimizeFastLevel{"-O3"};
inline constexpr std::string_view kFastMath{"-ffast-math"};

inline constexpr std::string_view kNoStandardIncludes{"-nostdinc"};
inline constexpr std::string_view kSystemInclude{"-isystem"};
inline constexpr std::string_view kUndefinePrefix{"-U"};
inline constexpr std::string_view kDefinePrefix{"-D"};
inline constexpr std::string_view kErrorLimit{"-ferror-limit=0"};
inline constexpr std::string_view kSfpiIncludeDirectory{"include"};

inline constexpr std::string_view kLauncherReason{"compiler launcher, the analysis runs Clang in process"};
inline constexpr std::string_view kDependencyFileReason{"writes a dependency file, the analysis produces no files"};
inline constexpr std::string_view kCxx20BackportReason{
    "SFPI-only C++20 backport, the command is translated to -std=c++20 instead"};

inline constexpr std::string_view kCompilerReplacedReason{"the analysis parses with Clang, not with the SFPI GCC"};
inline constexpr std::string_view kCpuReplacedReason{
    "Clang has no TT CPUs, the profile gives the same base ISA and ABI. The vendor Tensix extension cannot be enabled, "
    "so the SFPI headers use their analysis fallback"};
inline constexpr std::string_view kStandardReplacedReason{
    "Clang needs real C++20 for the features that the dropped -ftt-* options backport. __cplusplus becomes 202002L "
    "where SFPI GCC keeps 201703L"};
inline constexpr std::string_view kSyntaxOnlyReason{"the analysis parses the translation unit and emits no object"};
inline constexpr std::string_view kOptimizeFastReason{"the -Ofast spelling is deprecated in Clang, same meaning"};
inline constexpr std::string_view kSystemIncludesReason{
    "the SFPI GCC system include directories in GCC search order, instead of Clang's host and builtin headers"};
inline constexpr std::string_view kTypeModelReason{
    "SFPI GCC predefines these for newlib ilp32 (int32_t is long), Clang's riscv32 defaults differ"};
inline constexpr std::string_view kErrorLimitReason{"Clang stops parsing after 20 errors, the analysis keeps going"};

struct OptionRule {
    std::string_view name;
    std::string_view reason;
};

// Options removed when they match exactly.
inline constexpr auto kDroppedFlags = std::to_array<OptionRule>({
    {"-MMD", kDependencyFileReason},
    {"-MD", kDependencyFileReason},
    {"-MP", kDependencyFileReason},
    {"-ftt-nttp", kCxx20BackportReason},
    {"-ftt-constinit", kCxx20BackportReason},
    {"-ftt-consteval", kCxx20BackportReason},
    {"-ftt-no-dyninit",
     "SFPI-only check that rejects dynamic initialization, it does not change how accepted code parses"},
    {"-mno-tt-fix-whbhebreak", "SFPI code generation option, no effect on parsing"},
    {"-fno-tree-loop-distribute-patterns", "GCC optimization option, no effect on parsing"},
    {"--param=min-pagesize=0", "GCC diagnostic heuristic, no effect on parsing"},
    {"-Wfatal-errors", "stops Clang at the first error, the analysis keeps going"},
});

// Options removed when they start with the name, e.g. -flto=1 and -flto-partition=one.
inline constexpr auto kDroppedPrefixes = std::to_array<OptionRule>({
    {"-flto", "link-time optimization does not affect parsing and Clang rejects the GCC spellings"},
    {"-fdump-", "GCC debug dump, writes files"},
    {"-save-temps", "writes intermediate files"},
    {"-Werror", "the analysis keeps warnings as warnings"},
});

// Options removed together with the next argv element.
inline constexpr auto kDroppedWithOperand = std::to_array<OptionRule>({
    {"-o", "no object file is produced"},
    {"-MF", kDependencyFileReason},
});

// Their presence means the command needs -std=c++20 under Clang.
inline constexpr auto kStandardBackports =
    std::to_array<std::string_view>({"-ftt-nttp", "-ftt-constinit", "-ftt-consteval"});

inline constexpr auto kKeptFlags = std::to_array<std::string_view>({
    "-g",
    "-O0",
    "-O1",
    "-O2",
    "-O3",
    "-Os",
    "-Oz",
    "-Og",
    "-ffast-math",
    "-fno-finite-math-only",
    "-fsigned-zeros",
    "-fno-associative-math",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-use-cxa-atexit",
});

// Options whose value is the next argv element.
inline constexpr auto kKeptWithOperand = std::to_array<std::string_view>({"-include", "-isystem", "-I", "-D", "-U"});

// Joined spellings such as -DNAME=1, -I., -Wall. Warning options do not change the AST.
inline constexpr auto kKeptPrefixes = std::to_array<std::string_view>({"-D", "-U", "-I", "-W"});

// -W prefixes that forward options to another tool, so the -W rule above must not keep them.
inline constexpr auto kForwardingPrefixes = std::to_array<std::string_view>({"-Wp,", "-Wa,", "-Wl,"});

// Diagnostic options added to every command. They only silence warnings that the translation itself causes.
inline constexpr auto kAddedDiagnostics = std::to_array<OptionRule>({
    {"-Wno-unknown-warning-option", "the command keeps GCC warning options that Clang does not know"},
    {"-Wno-ignored-gch",
     "TT-Metal force-includes a header next to its GCC precompiled header, which Clang skips to parse the header"},
    {"-Wno-missing-template-arg-list-after-template-kw", "an error by default in Clang 20 that SFPI GCC accepts"},
});

inline constexpr OptionRule kCxx20Diagnostic{
    "-Wno-deprecated-enum-enum-conversion",
    "a C++20-only deprecation that SFPI headers trigger after the -std=c++20 translation"};

// Message formats for llvm::formatv, which takes a null-terminated string.
inline constexpr const char* kKernelContextMessage{"kernel '{0}' ({1}): {2}"};
inline constexpr const char* kNoArgumentsMessage{"the command has no arguments"};
inline constexpr const char* kNoCompilerMessage{"the command has only a compiler launcher"};
inline constexpr const char* kUnsupportedCompilerMessage{"expected the SFPI compiler '{0}', got '{1}'"};
inline constexpr const char* kMissingOperandMessage{"option '{0}' requires an operand"};
inline constexpr const char* kUnexpectedPositionalMessage{
    "unexpected positional argument '{0}', the source file is '{1}'"};
inline constexpr const char* kRepeatedSourceMessage{"the source file is given more than once"};
inline constexpr const char* kMissingSourceMessage{"the source file '{0}' is not on the command line"};
inline constexpr const char* kMissingStandardMessage{
    "the SFPI C++20 backports need {0}, which the verified Wormhole command set always passes"};
inline constexpr const char* kMissingCompileMessage{"no -c option, the command does not compile a translation unit"};
inline constexpr const char* kRepeatedCpuMessage{"more than one -mcpu option"};
inline constexpr const char* kMissingCpuMessage{"no -mcpu option, the target cannot be determined"};
inline constexpr const char* kUnsupportedCpuMessage{"unsupported -mcpu={0}, profile '{1}' supports: {2}"};
inline constexpr const char* kUnsupportedStandardMessage{
    "unsupported option '{0}', only {1} is part of the verified Wormhole command set"};
inline constexpr const char* kUnsupportedOptionMessage{
    "unsupported option '{0}': it is not part of the verified Wormhole command set. Verify its Clang semantics and add "
    "a rule"};

}  // namespace tt::lint::rules
