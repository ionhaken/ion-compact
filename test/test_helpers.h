// Tiny self-contained test harness. No external dependencies.
// Use TEST(MyName) { ... } to register a test; REQUIRE(cond) inside fails the test.
// Implement main() as: int main() { return ::test::RunAll(); }
#pragma once
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <vector>

namespace test
{
struct TestCase
{
	const char* name;
	void (*fn)();
};

inline std::vector<TestCase>& Registry()
{
	static std::vector<TestCase> reg;
	return reg;
}

struct Reg
{
	Reg(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline int RunAll()
{
	auto& r = Registry();
	std::printf("Running %zu tests\n", r.size());
	int passed = 0;
	int failed = 0;
	for (auto& t : r)
	{
		std::printf("[ RUN  ] %s\n", t.name);
		try
		{
			t.fn();
			std::printf("[  OK  ] %s\n", t.name);
			++passed;
		}
		catch (const std::exception& e)
		{
			std::printf("[ FAIL ] %s: %s\n", t.name, e.what());
			++failed;
		}
	}
	std::printf("\n%d passed, %d failed (out of %d)\n", passed, failed, int(r.size()));
	return failed == 0 ? 0 : 1;
}
}  // namespace test

#define TEST(name)                                              \
	static void name();                                         \
	static ::test::Reg name##_reg(#name, name);                 \
	static void name()

#define REQUIRE(cond)                                                                                  \
	do                                                                                                 \
	{                                                                                                  \
		if (!(cond))                                                                                   \
		{                                                                                              \
			char _req_buf[1024];                                                                       \
			std::snprintf(_req_buf, sizeof(_req_buf), "%s:%d REQUIRE(%s)", __FILE__, __LINE__, #cond); \
			throw std::runtime_error(_req_buf);                                                        \
		}                                                                                              \
	} while (0)
