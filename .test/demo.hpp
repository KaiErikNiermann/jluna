#pragma once

#include <exception>
#include <string>
#include <iostream>
#include <map>
#include <mutex>
#include <iostream>
#include <include/julia_wrapper.hpp>
#include <ptrhash.h>
#include <jluna.hpp>
#include <.test/test.hpp>
#include <jluna.hpp>
#include <thread>
#include <include/multi_threading.hpp>
#include <include/box.hpp>
#include <chrono>
#include <.src/cppcall.inl>

static const char number[] = "number";
static const char aux[]    = "aux";
static const char ptr[]    = "ptr";
static const char left[]   = "left";
static const char right[]  = "right";
static const char name[]   = "name";

struct Obj {
    int number;
};

struct Basic {
    int number;
};

set_usertype_enabled(Basic);
make_usertype_implicitly_convertible(Basic);
set_usertype_enabled(Obj);
make_usertype_implicitly_convertible(Obj);

struct A {
    virtual void foo() = 0;
};

set_usertype_enabled(A);

struct B : A {
    virtual void foo() override { }
    std::shared_ptr<A> ptr;
    int number;
};

set_usertype_enabled(B);
make_usertype_implicitly_convertible(B);

struct C : A {
    virtual void foo() override { }
    int number;
};

/* clang-format off */
static_assert(std::is_same_v<jluna::filter_types<A, std::is_base_of, B, C, A, int, float>, jluna::HList<B, C, A>>);
static_assert(std::is_same_v<jluna::filter_types<int, std::is_base_of, B, C>, jluna::HList<>>);
static_assert(std::is_same_v<jluna::filter_types<A, std::is_base_of, B>, jluna::HList<B>>);
static_assert(std::is_same_v<jluna::filter_types<int, std::is_base_of>, jluna::HList<>>);

class BaseC {
    public:
        BaseC()                                                  = default;
        virtual ~BaseC()                                         = default;
        virtual bool foo() const = 0;
};

class Tree : public BaseC {
public:
    std::shared_ptr<BaseC> left;
    std::shared_ptr<BaseC> right;
    std::string name;
    bool foo() const override { 
        return true; 
    }
};

class LeafA : public BaseC {
public:
    std::string name;
    bool foo() const override { 
        return true; 
    }
};


class LeafB : public BaseC {
public:
    std::string name;
    bool foo() const override { 
        return true; 
    }
};

static_assert(std::is_same_v<jluna::filter_types<BaseC, std::is_base_of, Tree, LeafA, LeafB>, jluna::HList<Tree, LeafA, LeafB>>);

set_usertype_enabled(BaseC);
set_usertype_enabled(Tree);
make_usertype_implicitly_convertible(Tree);
set_usertype_enabled(LeafA);
make_usertype_implicitly_convertible(LeafA);
set_usertype_enabled(LeafB);
make_usertype_implicitly_convertible(LeafB);

set_usertype_enabled(C);
make_usertype_implicitly_convertible(C);
