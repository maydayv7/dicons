#pragma once

#include <glib-object.h>
#include <memory>

template <auto Release>
struct GlibDeleter
{
    void operator()(auto *handle) const noexcept { Release(handle); }
};

template <class T>
using ObjectPtr = std::unique_ptr<T, GlibDeleter<g_object_unref>>;

// Adopt a full reference with ObjectPtr<T>{...}
template <class T>
[[nodiscard]] ObjectPtr<T> retain(T *borrowed)
{
    return ObjectPtr<T>{g_object_ref(borrowed)};
}
