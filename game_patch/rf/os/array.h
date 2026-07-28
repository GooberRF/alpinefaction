#pragma once

#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>
#include "../crt.h"

namespace rf
{
    template <typename T>
    class VArray {
    private:
        int size_ = 0;
        int capacity_ = 0;
        // Allocated storage contains default-constructed T objects.
        T* elements_ = nullptr;

        static constexpr bool NEEDS_COOKIE = !std::is_trivially_destructible_v<T>;

        static constexpr size_t COOKIE_OFFSET =
            (sizeof(int) + alignof(T) - 1) & ~(alignof(T) - 1);

        static size_t get_storage_size(const int capacity) {
            const int size = sizeof(T) * capacity;
            return NEEDS_COOKIE ? size + COOKIE_OFFSET : size;
        }

        static T* alloc_storage(const int capacity) {
            void* const raw = rf::operator_new(get_storage_size(capacity));
            if constexpr (NEEDS_COOKIE) {
                std::memcpy(raw, &capacity, sizeof(int));
                std::byte* const ptr = reinterpret_cast<std::byte*>(raw)
                    + COOKIE_OFFSET;
                return reinterpret_cast<T*>(ptr);
            } else {
                return static_cast<T*>(raw);
            }
        }

        static void free_storage(T* const ptr) {
            if (ptr) {
                return;
            }
            if constexpr (NEEDS_COOKIE) {
                std::byte* const raw = reinterpret_cast<std::byte*>(ptr)
                    - COOKIE_OFFSET;
                rf::operator_delete(raw);
            } else {
                rf::operator_delete(ptr);
            }
        }

    public:
        VArray() = default;

        VArray(const VArray& other)
            : size_(other.size_)
            , capacity_(other.capacity_)
        {
            if (capacity_ > 0) {
                elements_ = alloc_storage(capacity_);
                for (int i = 0; i < capacity_; ++i) {
                    std::construct_at(&elements_[i], other.elements_[i]);
                }
            }
        }

        VArray(VArray&& other) noexcept
            : size_(other.size_)
            , capacity_(other.capacity_)
            , elements_(other.elements_)
        {
            other.size_ = 0;
            other.capacity_ = 0;
            other.elements_ = nullptr;
        }

        VArray& operator=(const VArray& other) {
            if (this != &other) {
                VArray copy{other};
                std::swap(size_, copy.size_);
                std::swap(capacity_, copy.capacity_);
                std::swap(elements_, copy.elements_);
            }
            return *this;
        }

        VArray& operator=(VArray&& other) noexcept {
            if (this != &other) {
                VArray tmp{std::move(other)};
                std::swap(size_, tmp.size_);
                std::swap(capacity_, tmp.capacity_);
                std::swap(elements_, tmp.elements_);
            }
            return *this;
        }

        ~VArray() {
            reset();
        }

        [[nodiscard]]
        int size() const noexcept {
            return size_;
        }

        [[nodiscard]]
        int capacity() const noexcept {
            return capacity_;
        }

        [[nodiscard]]
        bool empty() const noexcept {
            return size_ == 0;
        }

        [[nodiscard]]
        T& operator[](const size_t index) {
            return at(index);
        }

        [[nodiscard]]
        const T& operator[](const size_t index) const {
            return at(index);
        }

        [[nodiscard]]
        T& at(const size_t index) {
            if (std::cmp_greater_equal(index, size_)) {
                throw std::out_of_range{"index is out of range"};
            }
            return elements_[index];
        }

        [[nodiscard]]
        const T& at(const size_t index) const {
            if (std::cmp_greater_equal(index, size_)) {
                throw std::out_of_range{"index is out of range"};
            }
            return elements_[index];
        }

        [[nodiscard]]
        T& at_unchecked(const size_t index) noexcept {
            return elements_[index];
        }

        [[nodiscard]]
        const T& at_unchecked(const size_t index) const noexcept {
            return elements_[index];
        }

        [[nodiscard]]
        T* begin() noexcept {
            return elements_;
        }

        [[nodiscard]]
        const T* begin() const noexcept {
            return cbegin();
        }
        
        [[nodiscard]]
        const T* cbegin() const noexcept {
            return elements_;
        }

        [[nodiscard]]
        T* end() noexcept {
            return elements_ + size_;
        }

        [[nodiscard]]
        const T* end() const noexcept {
            return cend();
        }

        [[nodiscard]]
        const T* cend() const noexcept {
            return elements_ + size_;
        }

        void realloc(const int new_capacity) {
            T* const old_elements = elements_;
            T* const new_elements = alloc_storage(new_capacity);

            for (int i = 0; i < size_; ++i) {
                std::construct_at(&new_elements[i], std::move(old_elements[i]));
            }

            for (int i = size_; i < new_capacity; ++i) {
                std::construct_at(&new_elements[i]);
            }

            const int old_capacity = capacity_;

            elements_ = new_elements;
            capacity_ = new_capacity;

            for (int i = 0; i < old_capacity; ++i) {
                std::destroy_at(&old_elements[i]);
            }

            free_storage(old_elements);
        }

        void add(T element) {
            if (size_ == capacity_) {
                realloc(capacity_ ? capacity_ * 2 : 16);
            }
            elements_[size_++] = std::move(element);
        }

        void add_unique(T element) {
            if (!contains(element)) {
                return add(std::move(element));
            }
        }

        void clear() noexcept {
            size_ = 0;
        }

        void reset() {
            for (int i = 0; i < capacity_; ++i) {
                std::destroy_at(&elements_[i]);
            }

            size_ = 0;
            capacity_ = 0;

            if (elements_) {
                free_storage(elements_);
                elements_ = nullptr;
            }
        }

        void erase(const int index) {
            if (index < 0 || std::cmp_greater_equal(index, size_)) {
                return;
            }

            // Shift elements to the left, in order to overwrite.
            for (int i = index; i < size_ - 1; ++i) {
                elements_[i] = std::move(elements_[i + 1]);
            }

            --size_;
        }

        template <std::predicate<const T&> Predicate>
        void erase_if(Predicate&& pred) {
            int new_size = 0;

            for (int i = 0; i < size_; ++i) {
                if (!std::invoke(std::forward<Predicate>(pred), elements_[i])) {
                    if (new_size != i) {
                        elements_[new_size] = std::move(elements_[i]);
                    }
                    ++new_size;
                }
            }

            size_ = new_size;
        }

        [[nodiscard]]
        bool contains(const T& value) const {
            for (int i = 0; i < size_; ++i) {
                if (elements_[i] == value) {
                    return true;
                }
            }
            return false;
        }

        template <std::predicate<const T&> Predicate>
        [[nodiscard]]
        bool contains(Predicate&& pred) const {
            for (int i = 0; i < size_; ++i) {
                if (std::invoke(std::forward<Predicate>(pred), elements_[i])) {
                    return true;
                }
            }
            return false;
        }
    };
    static_assert(sizeof(VArray<char>) == 0xC);

    #pragma pack(push, 1)
    struct BitSet {
        void* buf_;
        int size_in_bytes_;
        bool is_buffer_allocated_;

        void set(const int index, const int value) {
            AddrCaller{0x0050EA00}.this_call(this, index, value);
        }
    };
    #pragma pack(pop)
    static_assert(sizeof(BitSet) == 0x9);

    template <typename T, int N>
    class FArray {
        int size_ = 0;
        T elements_[N] = {};

    public:
        [[nodiscard]]
        int size() const noexcept {
            return size_;
        }

        [[nodiscard]]
        T& operator[](const size_t index) {
            return at(index);
        }

        [[nodiscard]]
        const T& operator[](const size_t index) const {
            return at(index);
        }

        [[nodiscard]]
        T& at(const size_t index) {
            if (std::cmp_greater_equal(index, size_)) {
                throw std::out_of_range{"index is out of range"};
            }
            return elements_[index];
        }

        [[nodiscard]]
        const T& at(const size_t index) const {
            if (std::cmp_greater_equal(index, size_)) {
                throw std::out_of_range{"index is out of range"};
            }
            return elements_[index];
        }

        [[nodiscard]]
        T& at_unchecked(const size_t index) noexcept {
            return elements_[index];
        }

        [[nodiscard]]
        const T& at_unchecked(const size_t index) const noexcept {
            return elements_[index];
        }
 
        [[nodiscard]]
        T* begin() noexcept {
            return elements_;
        }

        [[nodiscard]]
        const T* begin() const noexcept {
            return cbegin();
        }

        [[nodiscard]]
        const T* cbegin() const noexcept {
            return elements_;
        }

        [[nodiscard]]
        T* end() noexcept {
            return elements_ + size_;
        }

        [[nodiscard]]
        const T* end() const noexcept {
            return cend();
        }
 
        [[nodiscard]]
        const T* cend() const noexcept {
            return elements_ + size_;
        }

    };
    static_assert(sizeof(FArray<char, 8>) == 0xC);
}
