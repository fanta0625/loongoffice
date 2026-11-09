/* SPDX-License-Identifier: MPL-2.0 */
#include <page_selection.hxx>
#include <iostream>
#include <stdexcept>

int main()
{
    using namespace std::literals;
    auto require = [](bool condition) { if (!condition) throw std::runtime_error("page selection"); };
    require(pdfi::selectedPage(""sv, 3) == 0);
    require(pdfi::selectedPage("SkipImages"sv, 3) == 0);
    require(pdfi::selectedPage("Page=2"sv, 3) == 2);
    require(pdfi::selectedPage(L"Page=3"sv, 3) == 3);
    for (auto bad : { "Page="sv, "Page=0"sv, "Page=-1"sv, "Page=4"sv,
                      "Page=1-2"sv, "Page=2;SkipImages"sv, "Page=2 "sv,
                      "Page=2147483648"sv, "Page=999999999999999999999"sv })
        require(!pdfi::selectedPage(bad, 3));
    require(!pdfi::selectedPage(L"Page=0"sv, 3));
    std::cout << "Strict selected-page conversion checks passed\n";
}
