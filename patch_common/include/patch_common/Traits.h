#pragma once

#ifdef _MSC_VER
#define FASTCALL_LAMBDA
#define STDCALL_LAMBDA
#else
#define FASTCALL_LAMBDA __attribute__((fastcall))
#define STDCALL_LAMBDA __attribute__((stdcall))
#endif

// For generic types that are functors, delegate to its 'operator()'
template<typename T>
struct function_traits : public function_traits<decltype(&T::operator())>
{
};

// for pointers to member function
template<typename ClassType, typename ReturnType, typename... Args>
struct function_traits<ReturnType (ClassType::*)(Args...) const>
{
    // enum { arity = sizeof...(Args) };
    using f_type = ReturnType(Args...);
};

// for pointers to member function
template<typename ClassType, typename ReturnType, typename... Args>
struct function_traits<ReturnType (ClassType::*)(Args...)>
{
    using f_type = ReturnType(Args...);
};

// for cdecl function pointers
template<typename ReturnType, typename... Args>
struct function_traits<ReturnType (__cdecl*)(Args...)>
{
    using f_type = ReturnType __cdecl(Args...);
};

// for fastcall function pointers
template<typename ReturnType, typename... Args>
struct function_traits<ReturnType (__fastcall*)(Args...)>
{
    using f_type = ReturnType __fastcall(Args...);
};
