#include <array>
#include <string_view>

#include "ttkl/target_profile.hpp"

namespace tt::lint {

namespace {

// The SFPI GCC driver maps -mcpu=tt-wh-tensix to the wh-ilp32 multilib (MULTILIB_MATCHES "mcpu=tt-wh-tensix
// mcpu=tt-wh"). The order follows GCC's cppdefault.cc: libstdc++, its multilib and backward directories, GCC's own
// headers, fixincludes, then the newlib sysroot configured as --with-native-system-header-dir=/include.
constexpr auto kWormholeTensixIncludes = std::to_array<std::string_view>({
    "compiler/riscv-tt-elf/include/c++/15.1.0",
    "compiler/riscv-tt-elf/include/c++/15.1.0/riscv-tt-elf/wh-ilp32",
    "compiler/riscv-tt-elf/include/c++/15.1.0/backward",
    "compiler/lib/gcc/riscv-tt-elf/15.1.0/include",
    "compiler/lib/gcc/riscv-tt-elf/15.1.0/include-fixed",
    "compiler/riscv-tt-elf/include",
});

// SFPI GCC expands tt-wh-tensix to rv32im_xtttensixwh and is configured with --with-isa-spec=2.2, under which I implies
// Zicsr and Zifencei. Clang cannot enable the vendor Tensix extension, which is what makes the SFPI headers select
// their analysis fallback (tensix_builtins.h derives __riscv_xtttensixwh from ARCH_WORMHOLE instead).
constexpr auto kWormholeCpus = std::to_array<CpuMapping>({
    {"tt-wh-tensix", "riscv32-unknown-elf", "rv32im_zicsr_zifencei", "ilp32", kWormholeTensixIncludes},
});

// GCC's newlib-stdint.h for riscv*-elf with ilp32. Clang's riscv32 defaults differ only in these macros: int32_t is
// long rather than int, which also changes the type of INT32_MAX and INT32_C, and the fast 8- and 16-bit types are int.
constexpr auto kNewlibIlp32Macros = std::to_array<MacroOverride>({
    {"__INT32_TYPE__", "long int"},
    {"__INT32_MAX__", "0x7fffffffL"},
    {"__INT32_C(c)", "c ## L"},
    {"__UINT32_TYPE__", "long unsigned int"},
    {"__UINT32_MAX__", "0xffffffffUL"},
    {"__UINT32_C(c)", "c ## UL"},
    {"__INT_LEAST32_TYPE__", "long int"},
    {"__INT_LEAST32_MAX__", "0x7fffffffL"},
    {"__UINT_LEAST32_TYPE__", "long unsigned int"},
    {"__UINT_LEAST32_MAX__", "0xffffffffUL"},
    {"__INT_FAST8_TYPE__", "int"},
    {"__INT_FAST8_MAX__", "0x7fffffff"},
    {"__INT_FAST8_WIDTH__", "32"},
    {"__UINT_FAST8_TYPE__", "unsigned int"},
    {"__UINT_FAST8_MAX__", "0xffffffffU"},
    {"__INT_FAST16_TYPE__", "int"},
    {"__INT_FAST16_MAX__", "0x7fffffff"},
    {"__INT_FAST16_WIDTH__", "32"},
    {"__UINT_FAST16_TYPE__", "unsigned int"},
    {"__UINT_FAST16_MAX__", "0xffffffffU"},
});

// The SFPI 7.90.0 (build 1068) release as pinned by tt_metal/sfpi-version.
constexpr auto kSfpi7900Headers = std::to_array<HeaderDigest>({
    {"lltt.h", "a3284689b921cb80805bcac8d9f93d485a6ee1d450809c10cda673311c5fe76c"},
    {"sfpi.h", "c7c9701723972b87fd38f2faf30f8efbfca8883257b5e39752175d5221cbb1e0"},
    {"sfpi_builtins.h", "a84c3d6145082682248ccd37f44d286d5fe0235aa90e7d3dd73b65bc19a3b81b"},
    {"sfpi_classes.h", "038a2e8fde047728bee3918aee2a945cf70f27c979d60daeb7be325d42c8f194"},
    {"sfpi_constants.h", "392ab7b8ec0c01a8e9b8eb3090b057b1c36fce638666aa4d2c925b10abde3510"},
    {"sfpi_fp16.h", "a84c3d6145082682248ccd37f44d286d5fe0235aa90e7d3dd73b65bc19a3b81b"},
    {"sfpi_funcs.h", "3c7a416371390bed854cc8b523fd6254055d6b1ed49f31d3f8f95b70b2542a76"},
    {"sfpi_lib.h", "b7a6fa998c890887f1d345dc09ce290713a562b1e4ad263a0293580a0e467210"},
    {"tensix_builtins.def", "b4f83759f7d5350219d8d9bf884327d6dc5337bb63d3ecff73b940d390111496"},
    {"tensix_builtins.h", "27b7c2e52ea6d15df24e1898fcd9e7027633e32c2a176992cb73ad228f5c4bdd"},
});

constexpr auto kSfpi7900Patches = std::to_array<HeaderPatch>({
    {
        "tensix_builtins.h",
        "using __xtt_vector4 = unsigned __attribute__((vector_size (sizeof (unsigned) * 32 * 4)));\n",
        "using __xtt_vector4 = unsigned __attribute__((vector_size (sizeof (unsigned) * 32 * 4)));\n"
        "using __xtt_vector8 = unsigned __attribute__((vector_size (sizeof (unsigned) * 32 * 8)));\n",
        "the analysis fallback declares __xtt_vector, __xtt_vector2 and __xtt_vector4, but tensix_builtins.def also "
        "uses __xtt_vector8, a builtin type of SFPI GCC",
    },
    {
        "sfpi_classes.h",
        "  sfpi_inline vNarrow &operator= (vNarrow const &) = default;\n",
        "  sfpi_inline vNarrow &operator= (vNarrow const &) = default;\n"
        "#if __cplusplus >= 202002L\n"
        "  explicit operator vInt () const requires std::is_base_of_v<vUInt, Vector> = delete;\n"
        "#endif\n",
        "Clang finds vInt(vMag) and the inherited vUInt::operator vInt() ambiguous for vMag, vUInt16 and vUInt8. SFPI "
        "GCC selects the constructor, and the operator has no definition in SFPI 7.90.0. The explicit operator hides "
        "the inherited one in vNarrow, so Clang selects the same constructor",
    },
});

constexpr ToolchainProfile kWormholeSfpi7900{
    "wormhole-sfpi-7.90.0",
    "7.90.0",
    "riscv-tt-elf-g++",
    kWormholeCpus,
    kNewlibIlp32Macros,
    kSfpi7900Headers,
    kSfpi7900Patches,
};

}  // namespace

const ToolchainProfile& WormholeSfpiProfile() noexcept { return kWormholeSfpi7900; }

}  // namespace tt::lint
