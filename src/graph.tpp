//
// Created by qianlinluo@foxmail.com on 23-6-21.
//

#include "graph.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>
namespace pm_tiny {

    template<typename VertexType>
    const typename Graph<VertexType>::vertex_index_t
            Graph<VertexType>::npos;

    template<typename VertexType>
    Graph<VertexType>::Graph(size_t vertex_count) {
        vertex_count_ = vertex_count;
        adj_.resize(vertex_count);
        reverse_adj_.resize(vertex_count);
        vertices_.resize(vertex_count);
    }

    template<typename VertexType>
    void Graph<VertexType>::add_edge(typename Graph<VertexType>::vertex_index_t v,
                                     typename Graph<VertexType>::vertex_index_t w) {
        adj_[v].push_back(w);
        reverse_adj_[w].push_back(v);
    }

    template<typename VertexType>
    bool Graph<VertexType>::topological_sort() const {
        auto indegree = in_degree();
        std::queue<vertex_index_t> q;      // 维护一个入度为0的顶点的集合
        const auto &adj = adj_;
        for (size_t i = 0; i < vertex_count_; ++i) {
            if (indegree[i] == 0)
                q.push(i);         // 将所有入度为0的顶点入队
        }
        size_t count = 0;             // 计数，记录当前已经输出的顶点数
        while (!q.empty()) {
            vertex_index_t v = q.front();      // 从队列中取出一个顶点
            q.pop();

//            std::cout << v << " ";      // 输出该顶点
            ++count;
            // 将所有v指向的顶点的入度减1，并将入度减为0的顶点入栈
            for (auto beg: adj[v]) {
                if (indegree[beg] != 0 && !(--indegree[beg])) {
                    q.push(beg);   // 若入度为0，则入栈
                }
            }
        }
        if (count < vertex_count_)
            return false;           // 没有输出全部顶点，有向图中有回路
        else
            return true;            // 拓扑排序成功
    }

    template<typename VertexType>
    const std::vector<typename Graph<VertexType>::adj_vertices_t> &
    Graph<VertexType>::adj() const {
        return this->adj_;
    }

    template<typename VertexType>
    const std::vector<typename Graph<VertexType>::adj_vertices_t> &
    Graph<VertexType>::reverse_adj() const {
        return this->reverse_adj_;
    }

    template<typename VertexType>
    std::vector<size_t> Graph<VertexType>::in_degree() const {
        std::vector<size_t> in_degree(this->vertex_count_, 0);
        for (size_t i = 0; i < this->vertex_count(); i++) {
            const auto &in_vertices = this->reverse_adj_[i];
            in_degree[i] = in_vertices.size();
        }
        return in_degree;
    }

    template<typename VertexType>
    size_t Graph<VertexType>::vertex_count() const {
        return this->vertex_count_;
    }

    template<typename VertexType>
    size_t Graph<VertexType>::out_degree(typename Graph<VertexType>::vertex_index_t v) const {
        return this->adj_[v].size();
    }

    template<typename VertexType>
    void Graph<VertexType>::remove_vertex(typename Graph<VertexType>::vertex_index_t v) {
        auto &from_vertices = this->reverse_adj_[v];
        for (auto fv: from_vertices) {
            auto &adj = this->adj_[fv];
            adj.erase(std::find(adj.cbegin(), adj.cend(), v));
        }
        auto &to_vertices = this->adj_[v];
        for (auto tv: to_vertices) {
            auto &adj = this->reverse_adj_[tv];
            adj.erase(std::find(adj.cbegin(), adj.cend(), v));
        }
        to_vertices.clear();
        from_vertices.clear();
        this->reverse_adj_.erase(this->reverse_adj_.cbegin() + v);
        this->adj_.erase(this->adj_.cbegin() + v);
        auto sub_one = [](std::vector<adj_vertices_t> &vl, vertex_index_t v) {
            for (auto &ps: vl) {
                std::for_each(ps.begin(), ps.end(), [v](auto &i) {
                    if (i > v) {
                        i--;
                    }
                });
            }
        };
        sub_one(this->adj_, v);
        sub_one(this->reverse_adj_, v);
        this->vertices_.erase(this->vertices_.cbegin() + v);
        this->vertex_count_--;
    }

    template<typename VertexType>
    size_t Graph<VertexType>::in_degree(typename Graph<VertexType>::vertex_index_t v) const {
        return this->reverse_adj_[v].size();
    }

    template<typename VertexType>
    const typename Graph<VertexType>::adj_vertices_t &
    Graph<VertexType>::in_vertices(typename Graph<VertexType>::vertex_index_t v) const {
        return this->reverse_adj_[v];
    }

    template<typename VertexType>
    const typename Graph<VertexType>::adj_vertices_t &
    Graph<VertexType>::out_vertices(typename Graph<VertexType>::vertex_index_t v) const {
        return this->adj_[v];
    }

    template<typename VertexType>
    VertexType &Graph<VertexType>::vertex(typename Graph<VertexType>::vertex_index_t v) {
        return this->vertices_[v];
    }

    template<typename VertexType>
    const VertexType &Graph<VertexType>::vertex(typename Graph<VertexType>::vertex_index_t v) const {
        return const_cast<Graph *>(this)->vertex(v);
    }

    template<typename VertexType>
    template<typename Predicate>
    typename Graph<VertexType>::vertex_index_t
    Graph<VertexType>::vertex_index(Predicate pred) {
        auto iter = std::find_if(vertices_.cbegin(), vertices_.cend(), pred);
        if (iter == vertices_.cend()) {
            return Graph::npos;
        }
        return std::distance(vertices_.cbegin(),iter);
    }

    template<typename VertexType>
    const std::vector<VertexType> &Graph<VertexType>::vertices() const {
        return vertices_;
    }

    template<typename VertexType>
    std::string Graph<VertexType>::dump_vertex_in_out_degree() const {
        std::stringstream ss;
        ss << "adj size " << this->adj_.size() << std::endl;
        ss << "reverse adj size " << this->reverse_adj_.size() << std::endl;
        ss << "===in degree===" << std::endl;
        for (size_t i = 0; i < this->vertex_count(); i++) {
            ss << this->vertex(i) << "=" << this->in_degree(i);
            if (i != this->vertex_count() - 1) {
                ss << ",";
            }
        }
        ss<< std::endl;
        ss << "===out degree===" << std::endl;
        for (size_t i = 0; i < this->vertex_count(); i++) {
            ss << this->vertex(i) << "=" << this->out_degree(i);
            if (i != this->vertex_count() - 1) {
                ss << ",";
            }
        }
        ss << std::endl;
        for (size_t i = 0; i < adj_.size(); i++) {
            ss << i << "->" << this->vertex(i) << " : ";
            auto &adj_vertices = adj_[i];
            for (auto iter = adj_vertices.cbegin(); iter != adj_vertices.cend(); iter++) {
                ss<< this->vertex(*iter);
                if (std::next(iter) != adj_vertices.cend()) {
                    ss<< ",";
                }
            }
            ss << std::endl;
        }
        return ss.str();
    }

    template<typename VertexType>
    std::vector<typename Graph<VertexType>::vertex_index_t>
            Graph<VertexType>::bfs(typename Graph<VertexType>::vertex_index_t v) const {
        std::vector<vertex_index_t> vertices;
        std::unordered_set<vertex_index_t> visited;
        std::queue<vertex_index_t> ver_queue;
        ver_queue.push(v);
        visited.insert(v);
        while (!ver_queue.empty()) {
            auto vv = ver_queue.front();
            vertices.push_back(vv);
            ver_queue.pop();
            const auto &out_vertices1 = this->out_vertices(vv);
            for (vertex_index_t ver: out_vertices1) {
                if (visited.count(ver) == 0) {
                    ver_queue.push(ver);
                    visited.insert(ver);
                }
            }
        }
        return vertices;
    }
    template<typename VertexType>
    bool Graph<VertexType>::empty() const {
        return this->vertex_count() == 0;
    }

}