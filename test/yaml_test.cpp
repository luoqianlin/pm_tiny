//
// Created by qianlinluo@foxmail.com on 23-6-17.
//
#include <iostream>
#include <yaml-cpp/yaml.h>
#include <cassert>
#include <fstream>

int main() {
//    YAML::Node config = YAML::LoadFile("prog.yaml");
//
//    std::cout << config["rtsp"] << std::endl;
    YAML::Node node;
    YAML::Node value;
    value["xx"]=1;
    value["yy"]="aabbcc";
    node["rtsp"]=value;
    std::ofstream fout("config.yaml");
    fout << node;
    return 0;
}