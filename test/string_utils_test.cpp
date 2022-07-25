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

void test_case3(){
    std::string input1 = "\033[97;42m   333\033[0m";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "   333");
    assert(pair.second == "");
}

void test_case4(){
    std::string input1 = "\033[9742m   333\033[0m";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "\033[9742m   333");
    assert(pair.second == "");
}
void test_case5(){
    std::string input1 = "\033[97;42   333\033[0m";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "\033[97;42   333");
    assert(pair.second == "");
}

void test_case6(){
    std::string input1 = "\033[97;42m   333\033[2";
    auto pair = mgr::utils::remove_ANSI_escape_code(input1);
    assert(pair.first == "   333");
    assert(pair.second == "\033[2");
}

int main(int argc, char *argv[]) {
    std::string s="a\033bc";
    printf("length:%lu size:%lu\n", s.length(), s.size());
    printf("\033[36m remove_ANSI_escape_code test \033[0m\n");
    test_case1();
    test_case2();
    test_case3();
    test_case4();
    test_case5();
    test_case6();
    return 0;
}