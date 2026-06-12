#pragma once

#include <chrono>

namespace hft {

using Clock = std::chrono::steady_clock;

inline double seconds_elapsed(const Clock::time_point& start, const Clock::time_point& end) {
    return std::chrono::duration<double>(end - start).count();
}

}
