// printer.cpp : formatted output (C++)
#include <iostream>
#include <string>

// @doc Print text with decorative brackets to stdout.
// @param text  null-terminated string to display
extern "C" void
fancy_print(const char *text)
{
	std::string s(text);
	std::cout << ">>> " << s << " <<<" << std::endl;
}
