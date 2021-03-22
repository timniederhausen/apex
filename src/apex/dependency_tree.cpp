/*
 * Copyright (c) 2014-2021 Kevin Huck
 * Copyright (c) 2014-2021 University of Oregon
 *
 * Distributed under the Boost Software License, Version 1.0. (See accompanying
 * file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */

#include "dependency_tree.hpp"
#include "utils.hpp"
#include <iomanip>

namespace apex {

namespace dependency {

// declare an instance of the statics
std::mutex Node::treeMutex;
size_t Node::nodeCount{0};

Node* Node::appendChild(task_identifier* c) {
    treeMutex.lock();
    auto iter = children.find(*c);
    if (iter == children.end()) {
        auto n = new Node(c,this);
        //std::cout << "Inserting " << c->get_name() << std::endl;
        children.insert(std::make_pair(*c,n));
        treeMutex.unlock();
        return n;
    }
    iter->second->count++;
    treeMutex.unlock();
    return iter->second;
}

Node* Node::replaceChild(task_identifier* old_child, task_identifier* new_child) {
    treeMutex.lock();
    auto olditer = children.find(*old_child);
    // not found? shouldn't happen...
    if (olditer == children.end()) {
        auto n = new Node(new_child,this);
        //std::cout << "Inserting " << new_child->get_name() << std::endl;
        children.insert(std::make_pair(*new_child,n));
        treeMutex.unlock();
        return n;
    }
    olditer->second->count--;
    // if no more references to this node, delete it.
    if (olditer->second->count == 0) {
        children.erase(*old_child);
    }
    auto newiter = children.find(*new_child);
    // not found? shouldn't happen...
    if (newiter == children.end()) {
        auto n = new Node(new_child,this);
        //std::cout << "Inserting " << new_child->get_name() << std::endl;
        children.insert(std::make_pair(*new_child,n));
        treeMutex.unlock();
        return n;
    }
    treeMutex.unlock();
    return newiter->second;
}

void Node::writeNode(std::ofstream& outfile, double total) {
    // Write out the relationships
    if (parent != nullptr) {
        outfile << "  \"" << parent->getIndex() << "\" -> \"" << getIndex() << "\";";
        outfile << std::endl;
    }

    const std::string apex_main_str("APEX MAIN");
    double acc = (data == task_identifier::get_task_id(apex_main_str)) ?
        total : accumulated;
    node_color * c = get_node_color_visible(acc, 0.0, total);
    double ncalls = (calls == 0) ? 1 : calls;

    // write out the nodes
    outfile << "  \"" << getIndex() <<
    "\" [shape=box; style=filled; fillcolor=\"#" <<
            std::setfill('0') << std::setw(2) << std::hex << c->convert(c->red) <<
            std::setfill('0') << std::setw(2) << std::hex << c->convert(c->green) <<
            std::setfill('0') << std::setw(2) << std::hex << c->convert(c->blue) <<
            "\"; label=\"" << data->get_name() <<
    ":\\ltotal calls: " << ncalls << "\\ltotal time: " << acc << "\" ];" << std::endl;

    // do all the children
    for (auto c : children) {
        c.second->writeNode(outfile, total);
    }
}

void Node::addAccumulated(double value, bool is_resume) {
    static std::mutex m;
    m.lock();
    if (!is_resume) { calls+=1; }
    accumulated = accumulated + value;
    m.unlock();
}

} // dependency_tree

} // apex