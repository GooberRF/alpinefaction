#pragma once

#include <vector>
#include <memory>

template <typename T, size_t N>
class MemPool {
    struct Slot {
        alignas(T) std::byte buf[sizeof(T)];
    };

    class Delete {
        MemPool* pool{};

    public:
        explicit Delete(MemPool* pool)
            : pool(pool) {
        }

        void operator()(this const Delete& self, T* const ptr) {
            self.pool->release(ptr);
        }
    };

    std::vector<Slot*> free_slots{};
    std::vector<std::unique_ptr<Slot[]>> pages{};

public:
    using Pointer = std::unique_ptr<T, Delete>;

    MemPool() = default;
    MemPool(const MemPool&) = delete;
    MemPool& operator=(const MemPool&) = delete;

    Pointer alloc(this MemPool& self) {
        return Pointer{self.acquire(), Delete{&self}};
    }

private:
    void alloc_page(this MemPool& self)  {
        self.pages.push_back(std::make_unique<Slot[]>(N));
        const std::unique_ptr<Slot[]>& page = self.pages.back();

        self.free_slots.reserve(self.free_slots.size() + N);
        for (std::size_t i = 0; i < N; ++i) {
            Slot& slot = page[i];
            self.free_slots.push_back(&slot);
        }
    }

    T* acquire(this MemPool& self) {
        if (self.free_slots.empty()) {
            self.alloc_page();
        }
        Slot* const slot = self.free_slots.back();
        self.free_slots.pop_back();
        T* const ptr = reinterpret_cast<T*>(slot->buf);
        return std::construct_at(ptr);
    }

    void release(this MemPool& self, T* const ptr) {
        std::destroy_at(ptr);
        Slot* const slot = reinterpret_cast<Slot*>(ptr);
        self.free_slots.push_back(slot);
    }
};
