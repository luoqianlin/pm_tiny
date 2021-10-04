//
// Created by qianlinluo@foxmail.com on 23-6-21.
//

#ifndef PM_TINY_GRAPH_H
#define PM_TINY_GRAPH_H

#include <list>
#include <queue>
#include <string>

namespace pm_tiny {
    template<typename VertexType>
    class Graph {
    public:
        using vertex_index_t = size_t;
        using adj_vertices_t = std::list<vertex_index_t>;
        static const vertex_index_t npos = static_cast<vertex_index_t>(-1);
    private:
        size_t vertex_count_;
        std::vector<adj_vertices_t> adj_;
        std::vector<adj_vertices_t> reverse_adj_;
        std::vector<VertexType> vertices_;
    public:
        explicit Graph(size_t vertex_count);

        void add_edge(vertex_index_t v, vertex_index_t w);

        VertexType &vertex(vertex_index_t v);

        const VertexType &vertex(vertex_index_t v) const;

        template<typename Predicate>
        vertex_index_t vertex_index(Predicate pred);

        const std::vector<VertexType> &vertices() const;

        bool topological_sort() const;

        const std::vector<adj_vertices_t> &adj() const;

        const std::vector<adj_vertices_t> &reverse_adj() const;

        std::vector<size_t> in_degree() const;

        size_t in_degree(vertex_index_t v) const;

        size_t vertex_count() const;

        const adj_vertices_t &in_vertices(vertex_index_t v) const;

        const adj_vertices_t &out_vertices(vertex_index_t v) const;

        size_t out_degree(vertex_index_t v) const;

        void remove_vertex(vertex_index_t v);

        std::string dump_vertex_in_out_degree() const;

        std::vector<vertex_index_t> bfs(vertex_index_t v) const;
        bool empty()const;
    };


}

#include "graph.tpp"

#endif //PM_TINY_GRAPH_H
