//
// Created by qianlinluo@foxmail.com on 2022/7/26.
//

#include "../src/string_utils.h"
#include <iostream>
#include <assert.h>

void test_case1() {
    std::string input1 = "\033[10m   333\033[0m";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "   333");
    assert(pair.second == "");
}

void test_case2() {
    std::string input1 = "\033[10   333\033[0m";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "\033[10   333");
    assert(pair.second == "");
}

void test_case3() {
    std::string input1 = "\033[97;42m   333\033[0m";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "   333");
    assert(pair.second == "");
}

void test_case4() {
    std::string input1 = "\033[9742m   333\033[0m";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "\033[9742m   333");
    assert(pair.second == "");
}

void test_case5() {
    std::string input1 = "\033[97;42   333\033[0m";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "\033[97;42   333");
    assert(pair.second == "");
}

void test_case6() {
    std::string input1 = "\033[97;42m   333\033[2";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "   333");
    assert(pair.second == "\033[2");
}

void test_utf8_bytes_boundry() {
    std::string text = "上海上海";
    std::string text2 = "\xe4\xb8\x8a\xe6\xb5\xb7\xe4\xb8\x8a\xe6\xb5\xb7";
    assert(text == text2);
    printf("len:%zu\n", text2.length());
    for (int i = 0; i < text.length(); i++) {
        printf("%02x ", (unsigned char) text[i]);
    }
    printf("\n");
}

int main(int argc, char *argv[]) {
    using namespace std::string_literals;
    std::string s = "a\0bc"s;
    s = "aaa" "bbb" "cc";
    std::cout << "***>" << s << std::endl;
    printf("length:%lu size:%lu\n", s.length(), s.size());
    std::cout << "==>" << s << std::endl;
    printf("\033[36m remove_ANSI_escape_code test \033[0m\n");
    test_case1();
    test_case2();
    test_case3();
    test_case4();
    test_case5();
    test_case6();
    test_utf8_bytes_boundry();
    return 0;
}