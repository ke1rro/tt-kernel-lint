#pragma once
#include "includes/common.h"
#include <string_view>
#include <vector>

// STRUCT THAT DESCRIBES AN INSTRUCTION
// FROM api_spec.yaml

namespace tt_lint {

struct Instruction
{
    std::string_view name;
    std::string_view signature;
    Category category;
    ThreadKind thread_affinity;
    State precondition_state;
    State postcondition_state;
    std::string_view must_pair_with; // e.g. "tilize_uninit", or "" if none
    bool allowed_in_loop;
    std::string_view failure_mode;
    std::string_view description;
};

} // namespace tt_lint
