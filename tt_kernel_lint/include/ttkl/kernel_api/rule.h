#pragma once
#include "ttkl/kernel_api/common.h"
#include <string_view>
#include <vector>

namespace tt_lint {

struct Instruction {
    std::string_view name;
    std::string_view signature;
    std::string_view must_pair_with;
    std::string_view failure_mode;
    std::string_view description;
    Category category;
    ThreadKind thread_affinity;
    State precondition_state;
    State postcondition_state;
    bool allowed_in_loop;
};

}  // namespace tt_lint
