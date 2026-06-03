#pragma once

#include <cstdlib>
#include <cstring>

namespace clipidt {

inline bool enabledFromEnv()
{
    const char* v = std::getenv("VEDITOR_HDR_IDT");
    return v && v[0] && std::strcmp(v, "0") != 0;
}

} // namespace clipidt
