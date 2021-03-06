#pragma once

#include <stdexcept>
#include <string>
#include <memory>
#include <iostream>
#include <regex>

#include <cpprest/details/basic_types.h>

#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#define SPDLOG_FMT_EXTERNAL
#include <spdlog/spdlog.h>


#include "Deleters.h"
#include "Literals.h"
#include "ErrorHandling.h"

template <typename Arg>
void _print_assert_comment(Arg&& arg)
{
	std::cerr << '\t' << std::forward<Arg>(arg) << '\n';
}

template <typename ... Args>
void _print_failed_assert(const char* file, int line, const char* expr, Args&& ... args)
{
	std::cerr << "Check failed at " << file << ':' << line << ": " << expr << " is false\n";
	(_print_assert_comment(args), ...);
}

#ifdef NDEBUG

#define ASSERT(isTrue, ...) (void)(isTrue)
#define WARNING(isTrue, ...) (void)(isTrue)

#else // _DEBUG

#define STRINGIFY(x) #x
#define ASSERT(isFalse, ...) \
	if (!(bool)(isFalse)) { \
		_print_failed_assert(STRINGIFY(__FILE__), __LINE__, STRINGIFY(isFalse), __VA_ARGS__); \
		__debugbreak(); \
	}

#define WARNING(isFalse, ...) \
	if (!(bool)(isFalse)) { \
		_print_failed_assert(STRINGIFY(__FILE__), __LINE__, STRINGIFY(isFalse), __VA_ARGS__); \
	}

#endif // _DEBUG
