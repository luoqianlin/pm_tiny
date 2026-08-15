//
// Created by qianlinluo@foxmail.com on 22-8-5.
//

#include "core/ScopeGuard.h"

#include <exception>
#include <stdexcept>
#include <assert.h>

using namespace pm_tiny;

void test_inner_scope() {
    int x = 5;

    {
        PM_TINY_SCOPE_EXIT {
                               x++;
                           };
    }

    assert(x == 6);
}

void test_function_return() {
    int x = 5;

    [&]() {
        PM_TINY_SCOPE_EXIT {
                               x++;
                           };

        x++;
        return;
    }();

    assert(x == 7);
}

void test_exception_context() {
    int x = 5;

    try {
        [&]() {
            PM_TINY_SCOPE_EXIT {
                                   x++;
                               };

            throw std::runtime_error("exception");
        }();
    } catch (const std::exception &) {
    }

    assert(x == 6);
}

void test_multiple_guards() {
    int x = 5;

    {
        PM_TINY_SCOPE_EXIT {
                               x++;
                           };

        PM_TINY_SCOPE_EXIT {
                               x++;
                           };
    }

    assert(x == 7);
}

int main() {
    test_exception_context();
    test_function_return();
    test_inner_scope();
    test_multiple_guards();
}
