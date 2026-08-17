#include "pm_tiny_sdk.hpp"

#include <iostream>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
    int final_wait_ms = 2000;
    int ready_delay_ms = 0;
    int first_ready_delay_ms = -1;
    int tick_count = 3;
    int tick_interval_ms = 200;
    std::string marker_file;
    std::string generation_counter_file;
    if (argc > 1 && argv[1][0] != '-') final_wait_ms = std::atoi(argv[1]);
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--ready-delay-ms" && i + 1 < argc) ready_delay_ms = std::atoi(argv[++i]);
        else if (option == "--first-ready-delay-ms" && i + 1 < argc)
            first_ready_delay_ms = std::atoi(argv[++i]);
        else if (option == "--final-wait-ms" && i + 1 < argc) final_wait_ms = std::atoi(argv[++i]);
        else if (option == "--marker" && i + 1 < argc) marker_file = argv[++i];
        else if (option == "--generation-counter" && i + 1 < argc)
            generation_counter_file = argv[++i];
        else if (option == "--tick-count" && i + 1 < argc) tick_count = std::atoi(argv[++i]);
        else if (option == "--tick-interval-ms" && i + 1 < argc)
            tick_interval_ms = std::atoi(argv[++i]);
    }
    int fixture_generation = 1;
    if (!generation_counter_file.empty()) {
        int previous_generation = 0;
        std::ifstream input(generation_counter_file);
        if (input) input >> previous_generation;
        fixture_generation = previous_generation + 1;
        std::ofstream output(generation_counter_file, std::ios::trunc);
        output << fixture_generation;
    }
    if (fixture_generation == 1 && first_ready_delay_ms >= 0)
        ready_delay_ms = first_ready_delay_ms;
    pm_tiny::client client;
    if (!client.status().enabled) {
        std::cerr << "SDK disabled" << std::endl;
        return 2;
    }
    if (ready_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ready_delay_ms));
    client.ready();
    if (!marker_file.empty()) {
        std::ofstream marker(marker_file, std::ios::app);
        marker << "ready\n";
    }
    for (int i = 0; i < tick_count; ++i) {
        if (tick_interval_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_interval_ms));
        client.tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(final_wait_ms));
    if (!client.flush(std::chrono::seconds(2))) return 3;
    std::cout << client.status().app_name << std::endl;
    return 0;
}
