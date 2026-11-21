#pragma once

#include <cstdint>
#include <string_view>

// Single source of plugin identity constants for code and tests.
// The CMake block in the top-level CMakeLists.txt must carry the same values;
// tests/IdentityTests.cpp verifies agreement with the JucePlugin_* macros.
//
// PLACEHOLDER: manufacturer/plugin codes are NOT yet checked for uniqueness or
// registered. Do not ship with these values.
namespace tl::identity
{
constexpr std::uint32_t fourCC (char a, char b, char c, char d) noexcept
{
    return (std::uint32_t (std::uint8_t (a)) << 24) | (std::uint32_t (std::uint8_t (b)) << 16)
         | (std::uint32_t (std::uint8_t (c)) << 8)  |  std::uint32_t (std::uint8_t (d));
}

inline constexpr std::string_view companyName      = "Tone Legion";
inline constexpr std::string_view productName      = "Tone Legion Multiband";
inline constexpr std::string_view pluginName       = "Tone Legion Multiband";
inline constexpr std::string_view bundleId         = "com.tonelegion.multiband";
inline constexpr std::string_view clapId           = "com.tonelegion.multiband";
inline constexpr std::string_view versionString    = "0.1.0";

// PLACEHOLDER codes (see note above).
inline constexpr std::string_view manufacturerCodeString = "Tlgn";
inline constexpr std::string_view pluginCodeString       = "Tlmb";
inline constexpr std::uint32_t    manufacturerCode       = fourCC ('T', 'l', 'g', 'n');
inline constexpr std::uint32_t    pluginCode             = fourCC ('T', 'l', 'm', 'b');

inline constexpr int stateSchemaVersion = 1;
} // namespace tl::identity
