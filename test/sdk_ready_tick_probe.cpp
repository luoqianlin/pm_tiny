#include "AppClient.h"

#include <iostream>
#include <chrono>
#include <thread>

int main() {
    pm_tiny::AppClient client;
    if (!client.is_enable()) {
        std::cerr << "SDK disabled" << std::endl;
        return 2;
    }
    client.ready();
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        client.tick();
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << client.get_app_name() << std::endl;
    return 0;
}
