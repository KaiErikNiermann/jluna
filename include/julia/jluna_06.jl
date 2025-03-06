

module cppcall

    #const _lib = "<call jluna::initialize to initialize this field>";

    """
    check if jluna shared library is setup correctly
    """
    function verify_library() ::Bool
        out = false
        try
            out = @ccall cppcall._lib.jluna_verify()::Bool
        catch e
            println(Base.stderr, "[JULIA][ERROR] In jluna.verify_library: Unable to locate jluna shared library at `", cppcall._lib, "`. You can specify the correct path manually when calling jluna::initialize.`")
        end
        return out
    end

    """
    object that is callable like a function, but executes C++-side code
    """
    mutable struct UnnamedFunction{NArgs}

        _native_handle::Ptr{Cvoid}
        # points to C-side function

        _n_args::Cint
        #  0: (void) -> Any
        #  1: (Any) -> Any
        #  2: (Any, Any) -> Any
        #  3: (Any, Any, Any) -> Any
        # all others invalid

        function UnnamedFunction{N}(ptr::Ptr{Cvoid}) where N

            out = new{N}(ptr, N)
            finalizer(function (t::UnnamedFunction{N})
                @ccall cppcall._lib.jluna_free_lambda(t._native_handle::Csize_t, t._n_args::Cint)::Cvoid
            end, out);

            return out;
        end
    end

    """
    `make_unnamed_function(::Ptr{Cvoid}, n::Integer) -> UnnamedFunction{n}

    wrapper for UnnamedFunction ctor
    """
    function make_unnamed_function(ptr::Ptr{Cvoid}, n::Integer) ::UnnamedFunction{n}
        return UnnamedFunction{n}(ptr)
    end

    """
    `invoke_function(::UnnamedFunction) -> Ptr{Any}`

    invoke function with 0 args
    """
    function invoke_function(f::UnnamedFunction{0}) ::Ptr{Any}
        return @ccall cppcall._lib.jluna_invoke_lambda_0(f._native_handle::Ptr{Cvoid})::Ptr{Any}
    end

    # overload for 1 arg
    function invoke_function(f::UnnamedFunction{1}, arg1::Ptr{Any}) ::Ptr{Any}
        return @ccall cppcall._lib.jluna_invoke_lambda_1(f._native_handle::Ptr{Cvoid}, arg1::Ptr{Any})::Ptr{Any}
    end

    # overload for 2 args
    function invoke_function(f::UnnamedFunction{2}, arg1::Ptr{Any}, arg2::Ptr{Any}) ::Ptr{Any}
        return @ccall cppcall._lib.jluna_invoke_lambda_2(f._native_handle::Ptr{Cvoid}, arg1::Ptr{Any}, arg2::Ptr{Any})::Ptr{Any}
    end

    # overload for 3 args
    function invoke_function(f::UnnamedFunction{3}, arg1::Ptr{Any}, arg2::Ptr{Any}, arg3::Ptr{Any}) ::Ptr{Any}
        return @ccall cppcall._lib.jluna_invoke_lambda_3(f._native_handle::Ptr{Cvoid}, arg1::Ptr{Any}, arg2::Ptr{Any}, arg3::Ptr{Any})::Ptr{Any}
    end

    """
    `to_pointer(::Any) -> Ptr{Any}`

    get pointer to any object (including immutable ones)
    """
    function to_pointer(x) ::Ptr{Any}
        return @ccall cppcall._lib.jluna_to_pointer(x::Any)::Ptr{Cvoid}
    end
    
    """
    `from_pointer(::Ptr{Any}) -> Any`
    
    wrap unsafe_pointer_to_objref
    """
    function from_pointer(ptr::Ptr{T}) :: T where T 
        return unsafe_pointer_to_objref(ptr)
    end
    
    """
    invoke UnnamedFunction, trivial cases
    """
    (f::UnnamedFunction{0})() = from_pointer(invoke_function(f))
    (f::UnnamedFunction{1})(x) = from_pointer(invoke_function(f, to_pointer(x)))
    (f::UnnamedFunction{2})(x, y) = from_pointer(invoke_function(f, to_pointer(x), to_pointer(y)))
    (f::UnnamedFunction{3})(x, y, z) = from_pointer(invoke_function(f, to_pointer(x), to_pointer(y), to_pointer(z)))

    """
    `UnnamedFunction(xs...) -> Any`

    invoke UnnamedFunction, checks for correct number of arguments
    """
    function (f::UnnamedFunction{N1})(xs::Vararg{Any, N2}) where {N1, N2}
        if N1 != 0 && N1 != 1 && N1 != 2 & N1 != 3
            return from_pointer(invoke_function(f, to_pointer([xs...])));
        else
            throw(ErrorException(
                "MethodError: when trying to invoke <C++ Lambda#" * string(f._native_handle) * ">" *
                ": wrong number of arguments. expected " * string(N1) * ", got " * string(N2) * "."
               *  (N2 <= 3 ? "" : "\n\nTo create a C++-function that can take n > 3 arguments, simply make a 1-argument function with the only argument being an n-sized tuple or collection.")
            ))
        end
    end


    """
    `make_task(::UInt64) -> Task`
    """
    function make_task(ptr::UInt64)
        return Task() do;
            res_ptr = @ccall cppcall._lib.jluna_invoke_from_task(ptr::Csize_t)::Csize_t
            return unsafe_pointer_to_objref(Ptr{Any}(res_ptr))
        end
    end
end

# obfuscate internal state to encourage using operator[] sytanx
struct ProxyInternal

    _fieldnames_in_order::Vector{Symbol}
    _fields::Dict{Symbol, Union{Any, Missing}}
    _lock::Base.ReentrantLock

    ProxyInternal() = new(Vector{Symbol}(), Dict{Symbol, Union{Any, Missing}}(), Base.ReentrantLock())
end 

# proxy as deepcopy of cpp-side usertype object
struct Proxy

    _typename::Symbol
    _value::ProxyInternal

    Proxy(name::Symbol) = new(name, ProxyInternal())
end

"""
`new_proxy(::Symbol) -> Proxy`

wrap proxy ctor
"""
new_proxy(name::Symbol) = return Proxy(name)

"""
`implement(::Proxy, ::Module, ::Bool) -> Type`

translate a usertype proxy into an actual julia type
"""
function implement(template::Proxy, m::Module = Main, is_abstract::Bool = false, subtype::Union{DataType, Missing} = missing)::Type
    @lock template._value._lock begin
        out::Expr = :(abstract type $(template._typename) end)

        if !is_abstract
            if subtype !== missing 
                out = :(mutable struct $(template._typename) <: $subtype end)
            else 
                out = :(mutable struct $(template._typename) end)
            end

            # args of body Expr for struct
            struct_member_array = out.args[3].args

            empty_new = Expr(:(=), Expr(:call, template._typename), Expr(:call, :new))

            default_new::Expr = deepcopy(empty_new)

            # add fields to struct and create constructor
            for field_label in template._value._fieldnames_in_order
                type_or_instance = template._value._fields[field_label]
                is_datatype = isa(type_or_instance, DataType)
                field_type = is_datatype ? type_or_instance : typeof(type_or_instance)

                member_expr = :($(field_label)::$(field_type))

                push!(struct_member_array, member_expr)

                # default_new(field_label::field_type, ...)
                push!(default_new.args[1].args, member_expr)
                
                # default_new(...) = new(field_label, ...)
                push!(default_new.args[2].args, field_label)
            end

            push!(struct_member_array, default_new)

            if length(default_new.args[1].args) > 1 # non empty new constructor
                push!(struct_member_array, deepcopy(empty_new))
            end
        end
        Base.eval(m, out)
    end

    return m.eval(template._typename)
end

"""
`setindex!(::Proxy, <:Any, ::Symbol) -> Nothing`
extend base.setindex!
"""
function Base.setindex!(proxy::Proxy, value, key::Symbol) ::Nothing

    @lock proxy._value._lock begin
        if (!haskey(proxy._value._fields, key))
            push!(proxy._value._fieldnames_in_order, key)
        end

        proxy._value._fields[key] = value
    end
    return nothing
end

"""
`getindex(::Proxy, ::Symbol) -> Any`
extend base.getindex
"""
function Base.getindex(proxy::Proxy, value, key::Symbol) #::Auto

    out::Any = undef
    @lock proxy._value._lock begin
        out = proxy._value._fields[key]
    end
    return out
end

end # end of module jluna

return true # used for testing