#pragma once

namespace bytes_literals
{
	constexpr auto operator""_B(unsigned long long const x) -> size_t { return x; }
	constexpr auto operator""_KB(unsigned long long const x) -> size_t { return 1024_B * x; }
	constexpr auto operator""_MB(unsigned long long const x) -> size_t { return 1024_KB * x; }
	constexpr auto operator""_GB(unsigned long long const x) -> size_t { return 1024_MB * x; }
	constexpr auto operator""_TB(unsigned long long const x) -> size_t { return 1024_MB * x; }
}