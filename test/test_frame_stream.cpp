//
// Created by qianlinluo@foxmail.com on 23-9-10.
//
#include "../src/frame_stream.hpp"
#include <iostream>

int main() {
    pm_tiny::frame_t frame{1, 2, 3, 4, 2, 0, 0, 0, 'h'};
    pm_tiny::iframe_stream is(frame);
    int n;
    double d;
    std::string name;
    is >> n;
    std::cout << "n:" << std::hex << n << std::endl;
    std::cout << "before addr:" <<static_cast<const void*>(is.get_addr()) << std::endl;
    try {
        is >> name;
        std::cout << "name:" << name << std::endl;
    } catch (const std::exception &ex) {
        std::cout << ex.what() << std::endl;
    }
    std::cout << "after addr:" << static_cast<const void*>(is.get_addr())  << std::endl;
}