// 
// Copyright 2022 Clemens Cords
// Created on 25.02.22 by clem (mail@clemens-cords.com)
//

#include <include/exceptions.hpp>

namespace jluna
{
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

    template<typename T>
    template<typename Field_t>
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
            [unbox_set](T& instance, unsafe::Value* value) -> void {
                unbox_set(instance, jluna::unbox<Field_t>(value));
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


        if (!Usertype<T>::is_abstract()) {
            auto default_instance = T();
            auto* template_proxy = jluna::safe_call(new_proxy, _name->operator unsafe::Value*());

            for (auto& field_name : _fieldnames_in_order)
                jluna::safe_call(setfield, template_proxy, (unsafe::Value*) std::get<0>(_mapping.at(field_name))(default_instance), (unsafe::Value*) field_name);
            _type = std::make_unique<Type>((jl_datatype_t*) jluna::safe_call(implement, template_proxy, module, _abstract));
        } else {
            auto* template_proxy = jluna::safe_call(new_proxy, _name->operator unsafe::Value*());
            _type = std::make_unique<Type>((jl_datatype_t*) jluna::safe_call(implement, template_proxy, module, _abstract));
        }
        
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

    class leaf_finder {
        public:
            static void find_leaf_type(jl_value_t* val) {
                static jl_function_t* getfield = jl_get_function(jl_base_module, "getfield");
                jluna::Type val_type = jluna::Type((jl_datatype_t*) jl_typeof(val));
                bool has_struct_members = false;
                for (auto& field : val_type.get_field_names()) {
                    jl_value_t* field_val = jluna::safe_call(getfield, val, (unsafe::Value*) Symbol(field));
                    jluna::Type val_type = jluna::Type((jl_datatype_t*) jl_typeof(field_val));
                    if (jl_is_structtype(jl_typeof(field_val)) && !val_type.typename_is("String")) {
                        has_struct_members = true;  
                        find_leaf_type(field_val);
                    } else {
                        jluna::Type field_type = jluna::Type((jl_datatype_t*) jl_typeof(field_val));
                    }
                }
                // prints from inner -> outer
                std::cout << "type " << val_type.get_name() << "\n";
            }
    };

    template<typename T>
    T Usertype<T>::unbox(unsafe::Value* in)
    {
        if (not _implemented)
            implement();

        gc_pause;
        static jl_function_t* getfield = jl_get_function(jl_base_module, "getfield");

        auto out = T();

        // `pair.first` is a `jl_sym_t*`
        // `pair.second` is a `std::tuple<getter, setter, Type>`
        std::cout << "mapping struct " << get_name() << std::endl;
        for (auto& pair : _mapping) {
            using _setter_t = std::function<void(T&, unsafe::Value*)>;
            jl_sym_t* sym = (jl_sym_t*) pair.first;
            std::cout << "field: " << jl_symbol_name(sym) << std::endl;

            // calls the getter with symbol val
            jl_value_t* val = jluna::safe_call(getfield, in, (unsafe::Value*) pair.first);
            jluna::Type val_t = jluna::Type((jl_datatype_t*) jl_typeof(val));

            // --------------
            // testing

            std::cout << "val_t: " << val_t.get_name() << std::endl;
            std::cout << "is struct? " << val_t.is_struct_type() << std::endl;

            leaf_finder::find_leaf_type(val);


            







            // ---------------

            jluna::Type expected_t = std::get<2>(pair.second);
            std::cout << val_t.get_name() << "->" << expected_t.get_name() << std::endl;

            std::cout << 
                jl_string_ptr(
                    jl_call1(
                        jl_get_function(jl_base_module, "string"), 
                        val
                    )
                )
            << std::endl;
            
            _setter_t set = std::get<1>(pair.second);

            set(out, val);
        }

        gc_unpause;
        return out;
    }

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