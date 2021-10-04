//
// Created by qianlinluo@foxmail.com on 23-7-3.
//
#include "../src/string_utils.h"
#include <cassert>
#include <iostream>

int main(int argc, char *argv[]) {
    std::string filename = "a.txt";
    std::string name, ext;
    std::tie(name, ext) = mgr::utils::splitext(filename);
    assert(name == "a" && ext == ".txt");
    std::tie(name, ext) = mgr::utils::splitext("a.");
    assert(name == "a" && ext == ".");
    std::tie(name, ext) = mgr::utils::splitext("a");
    assert(name == "a" && ext == "");
    std::tie(name, ext) = mgr::utils::splitext(".txt");
    assert(name == ".txt" && ext == "");
    std::tie(name, ext) = mgr::utils::splitext("a..txt");
    assert(name == "a." && ext == ".txt");
    std::tie(name, ext) = mgr::utils::splitext("..txt");
    assert(name == "..txt" && ext == "");
    std::tie(name, ext) = mgr::utils::splitext("prog.cfg");
    assert(name == "prog" && ext == ".cfg");
    return 0;
}