#pragma once

#include <cstdlib>
#include <cstring>

namespace swscolor {

inline bool matrixEnabledFromEnv()
{
    const char* v = std::getenv("VEDITOR_COLOR_MATRIX");
    return v && v[0] && std::strcmp(v, "0") != 0;
}

} // namespace swscolor
