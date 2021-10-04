//
// Created by qianlinluo@foxmail.com on 23-6-21.
//
#include <chrono>
#include <unistd.h>
#include <iostream>
#include "../src/time_util.h"
#include "../sdk/AppClient.h"

int main(int argc, char *argv[]) {
    auto start = std::chrono::steady_clock::now();
    for (auto env = environ; (*env) != nullptr; env++) {
        printf("%s\n", *env);
    }
    auto end = std::chrono::steady_clock::now();
    auto cost = end - start;
    auto res = std::chrono::duration_cast<std::chrono::duration<double, std::ratio<60>>>(cost);
    std::cout << "cost:" << res.count() << "min" << std::endl;
    pm_tiny::AppClient appclient;
    if (appclient.is_enable()) {
        std::cout << "APP Name:" << appclient.get_app_name() << std::endl;
    } else {
        std::cout << "App is not managed by PM_Tiny" << std::endl;
    }
    TIME_THIS(
            appclient.ready()
    )

    for (int i = 0; i < 30; i++) {
        if (i < 25) {
            sleep(1);
        } else {
            sleep(11);
        }
        TIME_THIS(appclient.tick())
    }
    return 0;
}