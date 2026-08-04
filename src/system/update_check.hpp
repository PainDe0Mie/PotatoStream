#pragma once

#include <string>

struct UpdateCheckResult {
    bool update_available = false;
    bool failed = false;
    std::string latest_tag;
    std::string error;
};

const char *update_check_current_version();
UpdateCheckResult update_check_run();
