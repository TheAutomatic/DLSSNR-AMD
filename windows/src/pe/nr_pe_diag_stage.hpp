#pragma once

namespace nr::pe {

inline unsigned diagnostic_stage(const char* value) {
    return value && value[0] >= '0' && value[0] <= '4' && value[1] == '\0'
        ? unsigned(value[0] - '0') : 0u;
}

}
