//
// Created by qianlinluo@foxmail.com on 22-8-5.
//

#include "../src//ScopeGuard.h"

#include <exception>
#include <assert.h>

using namespace pm_tiny;

void test_ScopeGuardTest_InnerScope() {
    int x = 5;

    {
        PM_TINY_SCOPE_EXIT {
                               x++;
                           };
    }

    assert(x == 6);
}

void test_ScopeGuardTest_FunctionReturn() {
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

void test_ScopeGuardTest_ExceptionContext() {
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

void test_ScopeGuardTest_MultipleGuards() {
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
    test_ScopeGuardTest_ExceptionContext();
    test_ScopeGuardTest_FunctionReturn();
    test_ScopeGuardTest_InnerScope();
    test_ScopeGuardTest_MultipleGuards();
}