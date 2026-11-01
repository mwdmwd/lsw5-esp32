// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "support.h"

struct Case
{
	char const *name;
	void (*run)();
};
inline std::vector<Case> &cases()
{
	static std::vector<Case> result;
	return result;
}
struct Register
{
	Register(char const *n, void (*f)())
	{
		cases().push_back({ n, f });
	}
};
#define TEST(name) void name(); Register reg_##name(#name,name); void name()
#define CHECK(expr) do { if(!(expr)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #expr); } while(false)
