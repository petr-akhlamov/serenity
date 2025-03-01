/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/Traits.h>
#include <AK/kmalloc.h>

// NOTE: We can't include <initializer_list> during the toolchain bootstrap,
//       since it's part of libstdc++, and libstdc++ depends on LibC.
//       For this reason, we don't support Vector(initializer_list) in LibC.
#ifndef SERENITY_LIBC_BUILD
#    include <initializer_list>
#endif

#ifndef __serenity__
#    include <new>
#endif

namespace AK {

template<typename VectorType, typename ElementType>
class VectorIterator {
public:
    bool operator!=(const VectorIterator& other) const { return m_index != other.m_index; }
    bool operator==(const VectorIterator& other) const { return m_index == other.m_index; }
    bool operator<(const VectorIterator& other) const { return m_index < other.m_index; }
    bool operator>(const VectorIterator& other) const { return m_index > other.m_index; }
    bool operator>=(const VectorIterator& other) const { return m_index >= other.m_index; }
    ALWAYS_INLINE VectorIterator& operator++()
    {
        ++m_index;
        return *this;
    }
    VectorIterator& operator--()
    {
        --m_index;
        return *this;
    }
    VectorIterator operator-(size_t value) { return { m_vector, m_index - value }; }
    VectorIterator operator+(size_t value) { return { m_vector, m_index + value }; }
    VectorIterator& operator=(const VectorIterator& other)
    {
        m_index = other.m_index;
        return *this;
    }
    ALWAYS_INLINE ElementType& operator*() { return m_vector[m_index]; }
    size_t operator-(const VectorIterator& other) { return m_index - other.m_index; }

    bool is_end() const { return m_index == m_vector.size(); }
    size_t index() const { return m_index; }

private:
    friend VectorType;
    VectorIterator(VectorType& vector, size_t index)
        : m_vector(vector)
        , m_index(index)
    {
    }
    VectorType& m_vector;
    size_t m_index { 0 };
};

template<typename T>
class TypedTransfer {
public:
    static void move(T* destination, T* source, size_t count)
    {
        if (!count)
            return;
        if constexpr (Traits<T>::is_trivial()) {
            __builtin_memmove(destination, source, count * sizeof(T));
            return;
        }
        for (size_t i = 0; i < count; ++i)
            new (&destination[i]) T(AK::move(source[i]));
    }

    static void copy(T* destination, const T* source, size_t count)
    {
        if (!count)
            return;
        if constexpr (Traits<T>::is_trivial()) {
            __builtin_memmove(destination, source, count * sizeof(T));
            return;
        }
        for (size_t i = 0; i < count; ++i)
            new (&destination[i]) T(source[i]);
    }

    static bool compare(const T* a, const T* b, size_t count)
    {
        if (!count)
            return true;

        if constexpr (Traits<T>::is_trivial())
            return !__builtin_memcmp(a, b, count * sizeof(T));

        for (size_t i = 0; i < count; ++i) {
            if (a[i] != b[i])
                return false;
        }
        return true;
    }
};

template<typename T, size_t inline_capacity>
class Vector {
public:
    Vector()
        : m_capacity(inline_capacity)
    {
    }

    ~Vector()
    {
        clear();
    }

#ifndef SERENITY_LIBC_BUILD
    Vector(std::initializer_list<T> list)
    {
        ensure_capacity(list.size());
        for (auto& item : list)
            unchecked_append(item);
    }
#endif

    Vector(Vector&& other)
        : m_size(other.m_size)
        , m_capacity(other.m_capacity)
        , m_outline_buffer(other.m_outline_buffer)
    {
        if constexpr (inline_capacity > 0) {
            if (!m_outline_buffer) {
                for (size_t i = 0; i < m_size; ++i) {
                    new (&inline_buffer()[i]) T(move(other.inline_buffer()[i]));
                    other.inline_buffer()[i].~T();
                }
            }
        }
        other.m_outline_buffer = nullptr;
        other.m_size = 0;
        other.reset_capacity();
    }

    Vector(const Vector& other)
    {
        ensure_capacity(other.size());
        TypedTransfer<T>::copy(data(), other.data(), other.size());
        m_size = other.size();
    }

    template<size_t other_inline_capacity>
    Vector(const Vector<T, other_inline_capacity>& other)
    {
        ensure_capacity(other.size());
        TypedTransfer<T>::copy(data(), other.data(), other.size());
        m_size = other.size();
    }

    // FIXME: What about assigning from a vector with lower inline capacity?
    Vector& operator=(Vector&& other)
    {
        if (this != &other) {
            clear();
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            m_outline_buffer = other.m_outline_buffer;
            if constexpr (inline_capacity > 0) {
                if (!m_outline_buffer) {
                    for (size_t i = 0; i < m_size; ++i) {
                        new (&inline_buffer()[i]) T(move(other.inline_buffer()[i]));
                        other.inline_buffer()[i].~T();
                    }
                }
            }
            other.m_outline_buffer = nullptr;
            other.m_size = 0;
            other.reset_capacity();
        }
        return *this;
    }

    void clear()
    {
        clear_with_capacity();
        if (m_outline_buffer) {
            kfree(m_outline_buffer);
            m_outline_buffer = nullptr;
        }
        reset_capacity();
    }

    void clear_with_capacity()
    {
        for (size_t i = 0; i < m_size; ++i)
            data()[i].~T();
        m_size = 0;
    }

    bool operator==(const Vector& other) const
    {
        if (m_size != other.m_size)
            return false;
        return TypedTransfer<T>::compare(data(), other.data(), size());
    }

    bool operator!=(const Vector& other) const
    {
        return !(*this == other);
    }

    bool contains_slow(const T& value) const
    {
        for (size_t i = 0; i < size(); ++i) {
            if (at(i) == value)
                return true;
        }
        return false;
    }

    // NOTE: Vector::is_null() exists for the benefit of String::copy().
    bool is_null() const { return false; }
    bool is_empty() const { return size() == 0; }
    ALWAYS_INLINE size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }

    T* data()
    {
        if constexpr (inline_capacity > 0)
            return m_outline_buffer ? m_outline_buffer : inline_buffer();
        return m_outline_buffer;
    }
    const T* data() const
    {
        if constexpr (inline_capacity > 0)
            return m_outline_buffer ? m_outline_buffer : inline_buffer();
        return m_outline_buffer;
    }

    ALWAYS_INLINE const T& at(size_t i) const
    {
        ASSERT(i < m_size);
        return data()[i];
    }
    ALWAYS_INLINE T& at(size_t i)
    {
        ASSERT(i < m_size);
        return data()[i];
    }

    ALWAYS_INLINE const T& operator[](size_t i) const { return at(i); }
    ALWAYS_INLINE T& operator[](size_t i) { return at(i); }

    const T& first() const { return at(0); }
    T& first() { return at(0); }

    const T& last() const { return at(size() - 1); }
    T& last() { return at(size() - 1); }

    T take_last()
    {
        ASSERT(!is_empty());
        T value = move(last());
        last().~T();
        --m_size;
        return value;
    }

    T take_first()
    {
        ASSERT(!is_empty());
        T value = move(first());
        remove(0);
        return value;
    }

    T take(size_t index)
    {
        T value = move(at(index));
        remove(index);
        return value;
    }

    void unstable_remove(size_t index)
    {
        ASSERT(index < m_size);
        swap(at(index), at(m_size - 1));
        take_last();
    }

    void remove(size_t index)
    {
        ASSERT(index < m_size);

        if constexpr (Traits<T>::is_trivial()) {
            TypedTransfer<T>::copy(slot(index), slot(index + 1), m_size - index - 1);
        } else {
            at(index).~T();
            for (size_t i = index + 1; i < m_size; ++i) {
                new (slot(i - 1)) T(move(at(i)));
                at(i).~T();
            }
        }

        --m_size;
    }

    void insert(size_t index, T&& value)
    {
        ASSERT(index <= size());
        if (index == size())
            return append(move(value));
        grow_capacity(size() + 1);
        ++m_size;
        if constexpr (Traits<T>::is_trivial()) {
            TypedTransfer<T>::move(slot(index + 1), slot(index), m_size - index - 1);
        } else {
            for (size_t i = size() - 1; i > index; --i) {
                new (slot(i)) T(move(at(i - 1)));
                at(i - 1).~T();
            }
        }
        new (slot(index)) T(move(value));
    }

    void insert(size_t index, const T& value)
    {
        insert(index, T(value));
    }

    template<typename C>
    void insert_before_matching(T&& value, C callback, size_t first_index = 0, size_t* inserted_index = nullptr)
    {
        for (size_t i = first_index; i < size(); ++i) {
            if (callback(at(i))) {
                insert(i, move(value));
                if (inserted_index)
                    *inserted_index = i;
                return;
            }
        }
        append(move(value));
        if (inserted_index)
            *inserted_index = size() - 1;
    }

    Vector& operator=(const Vector& other)
    {
        if (this != &other) {
            clear();
            ensure_capacity(other.size());
            TypedTransfer<T>::copy(data(), other.data(), other.size());
            m_size = other.size();
        }
        return *this;
    }

    template<size_t other_inline_capacity>
    Vector& operator=(const Vector<T, other_inline_capacity>& other)
    {
        clear();
        ensure_capacity(other.size());
        TypedTransfer<T>::copy(data(), other.data(), other.size());
        m_size = other.size();
        return *this;
    }

    void append(Vector&& other)
    {
        if (is_empty()) {
            *this = move(other);
            return;
        }
        auto other_size = other.size();
        Vector tmp = move(other);
        grow_capacity(size() + other_size);
        TypedTransfer<T>::move(data() + m_size, tmp.data(), other_size);
        m_size += other_size;
    }

    void append(const Vector& other)
    {
        grow_capacity(size() + other.size());
        TypedTransfer<T>::copy(data() + m_size, other.data(), other.size());
        m_size += other.m_size;
    }

    template<typename Callback>
    void remove_first_matching(Callback callback)
    {
        for (size_t i = 0; i < size(); ++i) {
            if (callback(at(i))) {
                remove(i);
                return;
            }
        }
    }

    template<typename Callback>
    void remove_all_matching(Callback callback)
    {
        for (size_t i = 0; i < size();) {
            if (callback(at(i))) {
                remove(i);
            } else {
                ++i;
            }
        }
    }

    ALWAYS_INLINE void unchecked_append(T&& value)
    {
        ASSERT((size() + 1) <= capacity());
        new (slot(m_size)) T(move(value));
        ++m_size;
    }

    ALWAYS_INLINE void unchecked_append(const T& value)
    {
        unchecked_append(T(value));
    }

    template<class... Args>
    void empend(Args&&... args)
    {
        grow_capacity(m_size + 1);
        new (slot(m_size)) T(forward<Args>(args)...);
        ++m_size;
    }

    ALWAYS_INLINE void append(T&& value)
    {
        grow_capacity(size() + 1);
        new (slot(m_size)) T(move(value));
        ++m_size;
    }

    ALWAYS_INLINE void append(const T& value)
    {
        append(T(value));
    }

    void prepend(T&& value)
    {
        insert(0, move(value));
    }

    void prepend(const T& value)
    {
        insert(0, value);
    }

    void prepend(Vector&& other)
    {
        if (other.is_empty())
            return;

        if (is_empty()) {
            *this = move(other);
            return;
        }

        auto other_size = other.size();
        grow_capacity(size() + other_size);

        for (size_t i = size() + other_size - 1; i >= other.size(); --i) {
            new (slot(i)) T(move(at(i - other_size)));
            at(i - other_size).~T();
        }

        Vector tmp = move(other);
        TypedTransfer<T>::move(slot(0), tmp.data(), tmp.size());
        m_size += other_size;
    }

    void append(const T* values, size_t count)
    {
        if (!count)
            return;
        grow_capacity(size() + count);
        TypedTransfer<T>::copy(slot(m_size), values, count);
        m_size += count;
    }

    void grow_capacity(size_t needed_capacity)
    {
        if (m_capacity >= needed_capacity)
            return;
        ensure_capacity(padded_capacity(needed_capacity));
    }

    void ensure_capacity(size_t needed_capacity)
    {
        if (m_capacity >= needed_capacity)
            return;
        size_t new_capacity = needed_capacity;
        auto* new_buffer = (T*)kmalloc(new_capacity * sizeof(T));

        if constexpr (Traits<T>::is_trivial()) {
            TypedTransfer<T>::copy(new_buffer, data(), m_size);
        } else {
            for (size_t i = 0; i < m_size; ++i) {
                new (&new_buffer[i]) T(move(at(i)));
                at(i).~T();
            }
        }
        if (m_outline_buffer)
            kfree(m_outline_buffer);
        m_outline_buffer = new_buffer;
        m_capacity = new_capacity;
    }

    void shrink(size_t new_size, bool keep_capacity = false)
    {
        ASSERT(new_size <= size());
        if (new_size == size())
            return;

        if (!new_size) {
            if (keep_capacity)
                clear_with_capacity();
            else
                clear();
            return;
        }

        for (size_t i = new_size; i < size(); ++i)
            at(i).~T();
        m_size = new_size;
    }

    void resize(size_t new_size, bool keep_capacity = false)
    {
        if (new_size <= size())
            return shrink(new_size, keep_capacity);

        ensure_capacity(new_size);
        for (size_t i = size(); i < new_size; ++i)
            new (slot(i)) T;
        m_size = new_size;
    }

    void resize_and_keep_capacity(size_t new_size)
    {
        return resize(new_size, true);
    }

    using Iterator = VectorIterator<Vector, T>;
    Iterator begin() { return Iterator(*this, 0); }
    Iterator end() { return Iterator(*this, size()); }

    using ConstIterator = VectorIterator<const Vector, const T>;
    ConstIterator begin() const { return ConstIterator(*this, 0); }
    ConstIterator end() const { return ConstIterator(*this, size()); }

    template<typename Finder>
    ConstIterator find(Finder finder) const
    {
        for (size_t i = 0; i < m_size; ++i) {
            if (finder(at(i)))
                return ConstIterator(*this, i);
        }
        return end();
    }

    template<typename Finder>
    Iterator find(Finder finder)
    {
        for (size_t i = 0; i < m_size; ++i) {
            if (finder(at(i)))
                return Iterator(*this, i);
        }
        return end();
    }

    ConstIterator find(const T& value) const
    {
        return find([&](auto& other) { return value == other; });
    }

    Iterator find(const T& value)
    {
        return find([&](auto& other) { return value == other; });
    }

    Optional<size_t> find_first_index(const T& value)
    {
        for (size_t i = 0; i < m_size; ++i) {
            if (value == at(i))
                return i;
        }
        return {};
    }

private:
    void reset_capacity()
    {
        m_capacity = inline_capacity;
    }

    static size_t padded_capacity(size_t capacity)
    {
        return max(static_cast<size_t>(4), capacity + (capacity / 4) + 4);
    }

    T* slot(size_t i) { return &data()[i]; }
    const T* slot(size_t i) const { return &data()[i]; }

    T* inline_buffer()
    {
        static_assert(inline_capacity > 0);
        return reinterpret_cast<T*>(m_inline_buffer_storage);
    }
    const T* inline_buffer() const
    {
        static_assert(inline_capacity > 0);
        return reinterpret_cast<const T*>(m_inline_buffer_storage);
    }

    size_t m_size { 0 };
    size_t m_capacity { 0 };

    alignas(T) unsigned char m_inline_buffer_storage[sizeof(T) * inline_capacity];
    T* m_outline_buffer { nullptr };
};

}

using AK::Vector;
