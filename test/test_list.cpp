//
// Created by qianlinluo@foxmail.com on 23-7-27.
//
#include <list>
#include <cstdio>
#include <iostream>
#include <iterator>

int main() {
    std::list<int> l{1, 2, 3, 4, 5};
    std::list<int> l2{6, 7, 8, 9, 10};
    std::list<int> l4{61, 71, 81, 91, 11};
    auto &l3 = l;
    std::copy(l3.cbegin(), l3.cend(), std::ostream_iterator<int>(std::cout, " "));
    std::cout << std::endl;
    printf("l:%p,l2:%p,l3:%p\n", &l, &l2, &l3);
    l = l2;
    printf("l:%p,l2:%p,l3:%p\n", &l, &l2, &l3);
    std::copy(l3.cbegin(), l3.cend(), std::ostream_iterator<int>(std::cout, " "));
    std::cout << std::endl;
    l3 = l4;

    printf("l:%p,l2:%p,l3:%p\n", &l, &l2, &l3);
    std::copy(l3.cbegin(), l3.cend(), std::ostream_iterator<int>(std::cout, " "));
    std::cout << std::endl;

    std::copy(l.cbegin(), l.cend(), std::ostream_iterator<int>(std::cout, " "));
    std::cout << std::endl;
    l = std::move(l2);
    std::cout << "l2 size:" << l2.size() << std::endl;
    return 0;
}