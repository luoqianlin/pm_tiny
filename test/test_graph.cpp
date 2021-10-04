//
// Created by qianlinluo@foxmail.com on 23-7-15.
//
#include "../src/graph.h"
#include <iostream>
#include <iterator>

enum family {
    Jeanie,
    Debbie,
    Rick,
    John,
    Amanda,
    Margaret,
    Benjamin,
    N
};
using namespace pm_tiny;
std::vector<const char *> NAMES = {"Jeanie", "Debbie", "Rick", "John", "Amanda",
                                   "Margaret", "Benjamin"};

struct VertexData {
    std::string name;
    void *ptr{};
};

std::ostream &operator<<(std::ostream &os, const VertexData &vertexData) {
    return os << vertexData.name;
}

int main() {
    std::vector<int> nums(10);
    for (int i = 0; i < 10; i++) {
        nums[i] = i;
    }
    std::copy(nums.cbegin(), nums.cend(), std::ostream_iterator<int>(std::cout, " "));
    std::cout << std::endl;
    std::sort(nums.begin(), nums.end(), std::greater<>());
    std::copy(nums.cbegin(), nums.cend(), std::ostream_iterator<int>(std::cout, " "));
    std::cout << std::endl;
    using VertexType = VertexData;
    Graph<VertexType> g(N);
    g.add_edge(Jeanie, Debbie);
    g.add_edge(Jeanie, Rick);
    g.add_edge(Jeanie, John);
    g.add_edge(Debbie, Amanda);
    g.add_edge(Rick, Margaret);
    g.add_edge(John, Benjamin);
    for (int i = 0; i < NAMES.size(); i++) {
        g.vertex(i).name = NAMES[i];
    }
    std::cout << std::boolalpha << g.topological_sort() << std::endl;
    g.dump_vertex_in_out_degree();
    std::string vertex = "Rick";
    std::cout << "remove vertex:" << vertex << std::endl;
    auto get_pred = [](const std::string &name) {
        return [&name](const auto &vertex) {
            return vertex.name == name;
        };
    };

    auto get_vertex_i = [&g, get_pred](const std::string &name) {
        return g.vertex_index(get_pred(name));
    };

    g.remove_vertex(get_vertex_i(vertex));
    g.dump_vertex_in_out_degree();

    auto print_vertices = [&g](const std::vector<Graph<VertexType>::vertex_index_t> &vertices) {
        std::for_each(vertices.cbegin(), vertices.cend(), [&g](auto &vertex) {
            std::cout << g.vertex(vertex) << " ";
        });
    };

    auto bfs_traversal = [&](const std::string &name) {
        auto vertices = g.bfs(get_vertex_i(name));
        print_vertices(vertices);
    };
    auto bfs_traversal_i = [&](Graph<VertexType>::vertex_index_t v) {
        auto vertices = g.bfs(v);
        print_vertices(vertices);
    };
    std::cout << "bfs:";
    bfs_traversal("Jeanie");
    std::cout << std::endl;
    vertex = "Jeanie";
    std::cout << "remove vertex:" << vertex << std::endl;
    g.remove_vertex(get_vertex_i(vertex));
    g.dump_vertex_in_out_degree();
    std::cout << "bfs:";
    bfs_traversal("John");
    std::cout << std::endl;
    std::cout << "vertex count: " << g.vertex_count() << std::endl;
    for (int i = static_cast<int>(g.vertex_count()) - 1; i >= 0; i--) {
        std::cout << "remove " << g.vertex(i) << std::endl;
        g.remove_vertex(i);
        if (g.vertex_count() > 0) {
            std::cout << "bfs:";
            bfs_traversal_i(0);
            std::cout << std::endl;
        }
    }
    std::cout << "vertex count: " << g.vertex_count() << std::endl;
    for (Graph<VertexType>::vertex_index_t i = 0; i < g.vertex_count(); i++) {
        std::cout << g.vertex(i) << " ";
    }
    std::cout << std::endl;
    g.dump_vertex_in_out_degree();
    return 0;
}