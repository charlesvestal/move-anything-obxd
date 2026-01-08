/*
 * JUCE Compatibility Header for Move Anything OB-Xd Port
 *
 * Provides replacements for JUCE functions used in the OB-Xd Engine.
 * GPL-3.0 License
 */
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <string>

// Replace juce::float_Pi
namespace juce {
    constexpr float float_Pi = 3.14159265358979323846f;
}

// Make float_Pi available without juce:: prefix
using juce::float_Pi;

// Replace zeromem
#include <cstring>
inline void zeromem(void* ptr, size_t size) {
    memset(ptr, 0, size);
}

// JUCE String replacement (simple wrapper around std::string)
class String : public std::string {
public:
    String() : std::string() {}
    String(const char* s) : std::string(s) {}
    String(const std::string& s) : std::string(s) {}

    bool isEmpty() const { return empty(); }
    const char* toRawUTF8() const { return c_str(); }
};

// Replace roundToInt
inline int roundToInt(float x) {
    return (int)roundf(x);
}

// Replace jmin/jmax
template<typename T>
inline T jmin(T a, T b) {
    return a < b ? a : b;
}

template<typename T>
inline T jmax(T a, T b) {
    return a > b ? a : b;
}

// Replace jlimit (clamp value between min and max)
template<typename T>
inline T jlimit(T minVal, T maxVal, T val) {
    return val < minVal ? minVal : (val > maxVal ? maxVal : val);
}

// Simple Random class replacement
class Random {
private:
    uint64_t state;
public:
    Random() : state(12345678901234567ULL) {}
    Random(int64_t seed) : state((uint64_t)seed) {}

    static Random& getSystemRandom() {
        static Random sysRandom((int64_t)time(nullptr));
        return sysRandom;
    }

    int64_t nextInt64() {
        // Simple xorshift64
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return (int64_t)state;
    }

    float nextFloat() {
        return (float)(nextInt64() & 0x7FFFFFFF) / (float)0x7FFFFFFF;
    }
};

// Stub for JUCE leak detector macro (does nothing)
#define JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(className)
