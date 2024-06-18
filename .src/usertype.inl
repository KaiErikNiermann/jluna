//
// Copyright 2022 Clemens Cords
// Created on 25.02.22 by clem (mail@clemens-cords.com)
//
#include <include/exceptions.hpp>

namespace jluna
{
    inline auto single = [](unsafe::Value* in) -> auto {
        return [&in]<typename T, typename U>() {
            unbox<T>(in);
        };
    };

    using wrapper_t = decltype(single);

    template<typename T>
    struct as_julia_type<Usertype<T>>
    {
        static inline const std::string type_name = usertype_enabled<T>::name;
    };

    template<typename T>
    void Usertype<T>::initialize()
    {
        throw_if_uninitialized();
        _name = std::make_unique<Symbol>(get_name());
    }

    template <typename T>
    template <typename... AT>
    void Usertype<T>::initialize_map() {
        Usertype<T>::type_to_info[usertype_enabled<T>::name] = typeid(T).hash_code();

        Usertype<T>::seeker<wrapper_t> = [](const std::string& t_name, const wrapper_t&& op, unsafe::Value* in) {
            std::size_t type_info = type_to_info[t_name];

            auto lambda = [&op, &type_info, &in]<typename... ATs>() {
                if (type_info == typeid(T).hash_code()) {
                    std::cout << "actual type " << typeid(T).name() << std::endl;
                    op(in).template operator()<T, T>(); // calls unbox on T
                } else {
                    ([&type_info, &op, &in]() -> void {
                        if (type_info == typeid(ATs).hash_code()) {
                            std::cout << "actual type " << typeid(ATs).name() << std::endl;
                            op(in).template operator()<ATs, T>(); // calls unbox on ATs
                        }
                    }(), ...);
                }
            };

            lambda.template operator()<AT...>();
        };

        (initialize_additional_t_map<AT>(), ...);
    }

    template <typename T>
    template <typename U>
    void Usertype<T>::initialize_additional_t_map() {
        Usertype<T>::type_to_info[usertype_enabled<U>::name] = typeid(U).hash_code();
    }

    template<typename T>
    template<typename lambda_t>
    void Usertype<T>::dispatch_method(const std::string& t_name, lambda_t&& op, unsafe::Value* in) {
        Usertype<T>::seeker<wrapper_t>(t_name, op, in);
    }

    template<typename T>
    template<typename Field_t, typename... Derived_t>
    void Usertype<T>::add_property(
        const std::string& name,
        std::function<Field_t(T&)> box_get,
        std::function<void(T&, Field_t)> unbox_set)
    {
        if (_name.get() == nullptr)
            initialize();

        auto symbol = Symbol(name);

        if (_mapping.find(name) == _mapping.end())
            _fieldnames_in_order.push_back(symbol);

        _mapping.insert({symbol, {
            [box_get](T& instance) -> unsafe::Value* {
                return jluna::box<Field_t>(box_get(instance));
            },
            [unbox_set](T& instance, unsafe::Value* value, std::string jl_type_as_str, bool is_primitive) -> void {
                
                if (Usertype<T>::type_to_info[jl_type_as_str] == typeid(Field_t).hash_code()) {
                    std::cout << "a\n";
                    unbox_set(instance, jluna::unbox<Field_t>(value));
                } else {
                    ([=]() -> void {
                        if (Usertype<T>::type_to_info[jl_type_as_str] == typeid(Derived_t).hash_code()) {
                            std::cout << "b\n";
                            Derived_t unboxed_val = jluna::unbox<Derived_t>(value);
                            unbox_set(instance, &unboxed_val);
                        }
                    }(), ...);
                }

                // unbox_set(instance, jluna::unbox<Field_t>(value));
            },
            Type((jl_datatype_t*) jl_eval_string(as_julia_type<Field_t>::type_name.c_str()))
        }});
    }

    template<typename T>
    std::string Usertype<T>::get_name()
    {
        return usertype_enabled<T>::name;
    }

    template<typename T>
    bool Usertype<T>::is_abstract()
    {
        return usertype_enabled<T>::abstract;
    }

    template<typename T>
    bool Usertype<T>::is_enabled()
    {
        return usertype_enabled<T>::value;
    }

    template<typename T>
    void Usertype<T>::implement(unsafe::Module* module)
    {
        if (_name.get() == nullptr)
            initialize();

        gc_pause;
        static jl_function_t* implement = unsafe::get_function("jluna"_sym, "implement"_sym);
        static jl_function_t* new_proxy = unsafe::get_function("jluna"_sym, "new_proxy"_sym);
        static jl_function_t* setfield = jl_get_function(jl_base_module, "setindex!");
        static jl_value_t* _abstract = jl_box_bool(Usertype<T>::is_abstract());

        auto default_instance = T();
        auto* template_proxy = jluna::safe_call(new_proxy, _name->operator unsafe::Value*());

        for (auto& field_name : _fieldnames_in_order)
            jluna::safe_call(setfield, template_proxy, (unsafe::Value*) std::get<0>(_mapping.at(field_name))(default_instance), (unsafe::Value*) field_name);
        
        _type = std::make_unique<Type>((jl_datatype_t*) jluna::safe_call(implement, template_proxy, module, _abstract));

        _implemented = true;
        gc_unpause;
    }

    template<typename T>
    bool Usertype<T>::is_implemented()
    {
        return _implemented;
    }
    
    template<typename T>
    unsafe::Value* Usertype<T>::box(T& in)
    {
        if (not _implemented)
            implement();

        gc_pause;
        static jl_function_t* setfield = jl_get_function(jl_base_module, "setfield!");

        unsafe::Value* out = jl_call0(_type->operator unsafe::Value*());

        for (auto& pair : _mapping)
            jluna::safe_call(setfield, out, (unsafe::Value*) pair.first, std::get<0>(pair.second)(in));

        gc_unpause;
        return out;
    }

    template<typename T>
    T Usertype<T>::unbox(unsafe::Value* in)
    {
        if (not _implemented)
            implement();

        gc_pause;
        static jl_function_t* getfield = jl_get_function(jl_base_module, "getfield");

        using _setter_t = std::function<void(T&, unsafe::Value*, std::string, bool)>;
        auto out = T();

        // `pair.first` is a `jl_sym_t*`
        // `pair.second` is a `std::tuple<getter, setter, Type>`
        std::cout << "unboxing -> " << jl_string_ptr(jl_call1(jl_get_function(jl_base_module, "string"), in)) << std::endl;
        std::cout << "unboxing_t -> " << typeid(self).name() << std::endl;
        for (auto& pair : _mapping) {

            jl_value_t* val = jluna::safe_call(getfield, in, (unsafe::Value*) pair.first);
            jluna::Type val_t = jluna::Type((jl_datatype_t*) jl_typeof(val));

            _setter_t set = std::get<1>(pair.second);

            set(out, val, val_t.get_name(), !(val_t.get_name().compare("String")));
        }

        gc_unpause;
        return out;
    }

    template<typename T>
    void Usertype<T>::set_manually_abstract(bool value) { _manual_abstract = value; }


    template<is_usertype T>
    T unbox(unsafe::Value* in)
    {
       return Usertype<T>::unbox(in);
    }

    template<is_usertype T>
    unsafe::Value* box(T in)
    {
        return Usertype<T>::box(in);
    }
}