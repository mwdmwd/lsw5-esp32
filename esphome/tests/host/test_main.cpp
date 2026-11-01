// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
using namespace esphome;

int main()
{
	unsigned failures = 0;
	for(auto c : cases())
	{
		test_now = 0;
		try
		{
			c.run();
			std::cout << "PASS " << c.name << '\n';
		}
		catch(std::exception const &e)
		{
			++failures;
			std::cout << "FAIL " << c.name << ": " << e.what() << '\n';
		}
	}
	std::cout << (cases().size() - failures) << "/" << cases().size() << " host tests passed\n";
	return failures ? 1 : 0;
}
