#pragma once

#include <utility>

namespace ProviderCallbackBindings {

template <typename T, auto Method>
struct Member;

template <typename T, auto Method>
struct MemberDiscardReturn;

template <typename T, typename Return, typename... Args, Return (T::*Method)(Args...)>
struct Member<T, Method> {
    static Return invoke(void* ctx, Args... args) {
        return (static_cast<T*>(ctx)->*Method)(std::forward<Args>(args)...);
    }
};

template <typename T, typename Return, typename... Args, Return (T::*Method)(Args...) const>
struct Member<T, Method> {
    static Return invoke(void* ctx, Args... args) {
        return (static_cast<const T*>(ctx)->*Method)(std::forward<Args>(args)...);
    }
};

template <typename T, typename Return, typename... Args, Return (T::*Method)(Args...)>
struct MemberDiscardReturn<T, Method> {
    static void invoke(void* ctx, Args... args) {
        (void)(static_cast<T*>(ctx)->*Method)(std::forward<Args>(args)...);
    }
};

template <typename T, typename Return, typename... Args, Return (T::*Method)(Args...) const>
struct MemberDiscardReturn<T, Method> {
    static void invoke(void* ctx, Args... args) {
        (void)(static_cast<const T*>(ctx)->*Method)(std::forward<Args>(args)...);
    }
};

template <typename T, auto Method>
inline constexpr auto member = &Member<T, Method>::invoke;

template <typename T, auto Method>
inline constexpr auto memberDiscardReturn = &MemberDiscardReturn<T, Method>::invoke;

}  // namespace ProviderCallbackBindings
