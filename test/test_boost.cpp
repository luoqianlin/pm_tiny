//
// Created by qianlinluo@foxmail.com on 23-7-15.
//
#include <boost/config.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <boost/tuple/tuple.hpp>
enum family
{
    Jeanie,
    Debbie,
    Rick,
    John,
    Amanda,
    Margaret,
    Benjamin,
    N
};
int main()
{
    using namespace boost;
    std::vector<const char*> name = { "Jeanie", "Debbie", "Rick", "John", "Amanda",
                           "Margaret", "Benjamin" };

    adjacency_list<> g(N);
    add_edge(Jeanie, Debbie, g);
    add_edge(Jeanie, Rick, g);
    add_edge(Jeanie, John, g);
    add_edge(Debbie, Amanda, g);
    add_edge(Rick, Margaret, g);
    add_edge(John, Benjamin, g);
//    clear_vertex(vertex(Jeanie,g), g);
    remove_vertex(vertex(Jeanie,g),g);
    name.erase(name.begin());

//    clear_vertex(vertex(Jeanie,g), g);
//    remove_vertex(Jeanie,g);
//    adjacency_list<> newG(N - 1);
//    graph_traits< adjacency_list<> >::edge_iterator ei, ei_end;
//    for (boost::tie(ei, ei_end) = edges(g); ei != ei_end; ++ei)
//    {
//        auto sourcex = source(*ei, g);
//        auto targetx = target(*ei, g);
//        if (sourcex != Jeanie && targetx != Jeanie)
//            add_edge(sourcex, targetx, newG);
//    }
//    g=newG;

    graph_traits< adjacency_list<> >::vertex_iterator i, end;
    graph_traits< adjacency_list<> >::adjacency_iterator ai, a_end;
    property_map< adjacency_list<>, vertex_index_t >::type index_map
            = get(vertex_index, g);
    for (boost::tie(i, end) = vertices(g); i != end; ++i)
    {
        std::cout << name[get(index_map, *i)];
        boost::tie(ai, a_end) = adjacent_vertices(*i, g);
        if (ai == a_end)
            std::cout << " has no children";
        else
            std::cout << " is the parent of ";
        for (; ai != a_end; ++ai)
        {
            std::cout << name[get(index_map, *ai)];
            if (boost::next(ai) != a_end)
                std::cout << ", ";
        }
        std::cout << std::endl;
    }
    return EXIT_SUCCESS;
}