//
// Copyright 2022 Clemens Cords
// Created on 25.02.22 by clem (mail@clemens-cords.com)
//
#include <include/exceptions.hpp>
#include <charconv>
#include <iomanip>
#include <functional>
#include <iostream>
#include <execution>
#include <optional>
#include <ranges>

namespace jluna {
    template <typename T>
    struct as_julia_type<Usertype<T>> {
        static inline const std::string type_name = usertype_enabled<T>::name;
    };

    template <typename T>
    void Usertype<T>::initialize() {
        throw_if_uninitialized();
        _name = std::make_unique<Symbol>(get_name());
    }

    template <template <class, class, class...> class C, typename K, typename V, typename... Args>
    static inline auto get_or_def(const C<K, V, Args...>& m, const K& key, const V& defval) -> V {
        typename C<K, V, Args...>::const_iterator it = m.find(key);
        return it == m.end() ? defval : it->second;
    }

    template <typename... L>
    struct match_fn : L... {
        using L::operator()...;
        constexpr match_fn(L... lambda)
            : L(std::move(lambda))... { }
    };

    template <typename H, typename F, typename ML>
    struct iterate_h {
        static inline auto with(F&& f, ML&& ml) { f(std::move(ml.template operator()<>())); }
    };

    template <typename H, typename F, typename ML>
    struct iterate_h<HList<H>, F, ML> {
        static inline auto with(F&& f, ML&& ml) { f(std::move(ml.template operator()<H>())); }
    };

    template <typename H, typename... T, typename F, typename ML>
        requires(sizeof...(T) > 0)
    struct iterate_h<HList<H, T...>, F, ML> {
        static inline auto with(F&& f, ML&& ml) {
            f(std::move(ml.template operator()<H>()));
            iterate_h<HList<T...>, F, ML>::with(std::forward<F>(f), std::forward<ML>(ml));
        }
    };

    // base case : empty list, A has to be primitive or boxable
    template <typename H, typename F, typename A, typename ML>
    struct fold_sc_h {
        template <typename Arg>
            requires(is_primitive<Arg> or is_boxable<Arg>)
        static inline auto with(Arg&& arg, F&& f, ML&& ml) {
            return f(std::make_optional(ml.template operator()<A>(arg)));
        }
    };

    // trivial case : one element
    template <typename H, typename F, typename A, typename ML>
    struct fold_sc_h<HList<H>, F, A, ML> {
        static inline auto with(A&& arg, F&& f, ML&& ml) {
            return f(std::make_optional(ml.template operator()<H>(arg)));
        }
    };

    // recursive case : non-empty list
    template <typename H, typename... T, typename F, is_not_boxable A, typename ML>
        requires(sizeof...(T) > 0)
    struct fold_sc_h<HList<H, T...>, F, A, ML> {
        static inline auto with(A&& arg, F&& f, ML&& ml) {
            if (auto maybe_value = ml.template operator()<H>(arg))
                return f(std::make_optional(maybe_value));

            return fold_sc_h<HList<T...>, F, A, ML>::with(
                std::forward<A>(arg), std::forward<F>(f), std::forward<ML>(ml)
            );
        }
    };

    template <typename HL, typename F, typename A, typename ML>
    static inline auto fold_sc(A&& arg, F&& f, ML&& ml) {
        return fold_sc_h<HL, F, A, ML>::with(
            std::forward<A>(arg), std::forward<F>(f), std::forward<ML>(ml)
        );
    }

    /* clang-format off */
    template <typename T, typename F, typename ML>
    static inline auto iterate(F&& f, ML&& ml) {
        iterate_h<T, F, ML>::with(
            std::forward<F>(f), std::forward<ML>(ml)
        );
    }

    template <typename T> 
    inline std::string type_name(unsafe::Value* val = nullptr) {
        return as_julia_type<T>::type_name.c_str();
    }

    template <is_usertype T> 
    inline std::string type_name(unsafe::Value* val = nullptr) {
        return val ? Type((jl_datatype_t*)jl_typeof(val)).get_name() : Usertype<T>::get_name();
    }

    template <is_usertype T>
    static void dump_dispatch_map(auto& map) {
        std::cerr << "[C++] Dumping dispatch map for type: " << Usertype<T>::get_name()
                  << std::endl;
        for (auto& [key, value] : map) {
            std::cerr << "[C++] " << key.first << " -> " << key.second << std::endl;
        }
    }

    /* clang-format off */
    template <typename T>
    template <typename... Field, typename... DerivedType>
    void Usertype<T>::initialize_dispatch_map(TL<Field...>, TL<DerivedType...>) {
        (iterate<filter_types<typename Field::type, std::is_base_of, DerivedType...>>(
             [](auto&& item) { _type_dispatch_map.insert(item); },
             std::move(match_fn {
                [&]<typename FT = typename Field::type>() -> dispatch_map_kv<T> {
                    return {
                        {Field::get_name(), type_name<FT>()},
                        [&](T& instance, unsafe::Value* value) -> void {
                            Field::setter(instance, jluna::unbox<FT>(value));
                        }
                    };
                },
                [&]<is_base_of<typename Field::type> DT>() -> dispatch_map_kv<T> {
                    return {
                        {Field::get_name(), Usertype<DT>::get_name()},
                        [&](T& instance, unsafe::Value* value) -> void {
                            Field::setter(instance, std::make_shared<DT>(jluna::unbox<DT>(value)));
                        }
                    };
                }
             })
         ),
         ...);
    }

    template <typename T>
    template <typename... Field, typename... DerivedType>
    void Usertype<T>::initialize_type(TL<Field...>, TL<DerivedType...>) {
        Usertype<T>::initialize_dispatch_map(TL<Field...>(), TL<DerivedType...>());
        /* clang-format off */
        (
            [&]() -> void {
                auto symbol     = Symbol(Field::get_name());
                using FieldType = typename Field::type;

                if (_mapping.find(symbol) == _mapping.end())
                    _fieldnames_in_order.push_back(symbol);

                _mapping.insert(
                    {symbol,
                    {[&](T& instance) -> unsafe::Value* {
                        return fold_sc<filter_types<FieldType, std::is_base_of, DerivedType...>>(
                            std::move(Field::getter(instance)),
                            [&](std::optional<unsafe::Value*>&& val) -> auto {
                                return std::move(val.value_or(nullptr));
                            },
                            std::move(match_fn {
                                [&]<typename DT = FieldType, is_boxable FT>(FT& value) -> unsafe::Value* {
                                    return jluna::box<FT>(std::move(value));
                                },
                                [&]<is_base_of<FieldType> DT, is_not_boxable FT>(FT& value) -> unsafe::Value* {
                                    if (!_implemented)
                                        return Usertype<FieldType>::_type->operator unsafe::Value*();

                                    if (auto val = dynamic_cast<DT*>(value.get()))
                                        return jluna::box<DT>(*val);

                                    return nullptr;
                                }
                            }));
                      },
                      [&](T& instance, unsafe::Value* value) -> void {
                            get_or_def(_type_dispatch_map, 
                                {Field::get_name(), type_name<FieldType>(value)}, 
                                std::function<void(T&, unsafe::Value*)>(
                                    std::move([&](auto... args) -> void {
                                        std::cerr << "[unbox] For UDT member: " << Usertype<T>::get_name() << "." << Field::get_name()  
                                                << " no dispatch found for type\n";

                                        dump_dispatch_map<T>(_type_dispatch_map);
                                        throw MissingTypeSetterDispatch();
                                    }
                                ) 
                            ))(instance, value);
                      },
                      Type((jl_datatype_t*)jl_eval_string(as_julia_type<FieldType>::type_name.c_str()))}}
                );
            }(),
            ...
        );
    }

    template <typename T>
    std::string Usertype<T>::get_name() {
        return usertype_enabled<T>::name;
    }

    template <typename T>
    bool Usertype<T>::is_abstract() {
        return usertype_enabled<T>::abstract;
    }

    template <typename T>
    bool Usertype<T>::is_enabled() {
        return usertype_enabled<T>::value;
    }

    template <typename T>
    template <typename U>
    void Usertype<T>::implement(unsafe::Module* module) {
        if (_name.get() == nullptr)
            initialize();

        gc_pause;
        auto constructor = "Base.missing"_eval;

        auto template_proxy = "new_proxy"_Jluna(_name->operator unsafe::Value*());

        if constexpr (not std::is_abstract_v<T>) {
            T in;
            for (auto& [label, lens] : _mapping) 
                "setindex!"_Base(template_proxy, std::get<0>(lens)(in), label);
        }


        if constexpr (std::is_base_of_v<U, T> and !std::is_same_v<U, T>) 
            constructor = Usertype<U>::_type->operator unsafe::Value*();
      
        _type = std::make_unique<Type>((jl_datatype_t*)"implement"_Jluna(
            template_proxy, module, jluna::box<bool>(is_abstract()), constructor
        ));

        _implemented = true;
        gc_unpause;
    }

    template <typename T>
    bool Usertype<T>::is_implemented() {
        return _implemented;
    }

    template <typename T>
    unsafe::Value* Usertype<T>::box(T& in) {
        if (not _implemented) [[unlikely]]
            implement();

        gc_pause;
        unsafe::Value* out = jl_call0(_type->operator unsafe::Value*());

        for (auto& [label, lens] : _mapping) 
            "setfield!"_Base(out, label, std::get<0>(lens)(in));

        gc_unpause;
        return out;
    }

    template <typename T>
    T Usertype<T>::unbox(unsafe::Value* in) {
        if (not _implemented) [[unlikely]]
            implement();

        gc_pause;
        T out;

        for (auto& [label, lens] : _mapping) 
            std::get<1>(lens)(out, "getfield"_Base(in, (unsafe::Value*)label));

        gc_unpause;
        return out;
    }

    template <is_usertype T>    
    T unbox(unsafe::Value* in) {
        return Usertype<T>::unbox(in);
    }

    template <is_usertype T>
    unsafe::Value* box(T in) {
        return Usertype<T>::box(in);
    }

} // namespace jluna