#include "pm_tiny_sdk.hpp"

#include <iostream>
#include <chrono>
#include <cstdlib>
#include <thread>

int main(int argc, char **argv) {
    const int final_wait_ms = argc > 1 ? std::atoi(argv[1]) : 2000;
    pm_tiny::client client;
    if (!client.status().enabled) {
        std::cerr << "SDK disabled" << std::endl;
        return 2;
    }
    client.ready();
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        client.tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(final_wait_ms));
    if (!client.flush(std::chrono::seconds(2))) return 3;
    std::cout << client.status().app_name << std::endl;
    return 0;
}
