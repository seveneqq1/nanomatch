#pragma once

#include "types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace nanomatch {

// ---------------------------------------------------------------------------
// Aligned arena allocation — used only at engine construction / teardown.
// ---------------------------------------------------------------------------
template <typename T>
[[nodiscard]] inline T* alloc_aligned_array(std::size_t count) {
    static_assert(alignof(T) <= 64 || alignof(T) == 64 || alignof(T) % 64 == 0,
                  "unexpected alignment");
    const std::size_t align = alignof(T) < kCacheLine ? kCacheLine : alignof(T);
    const std::size_t bytes = count * sizeof(T);
    void* p = ::operator new(bytes, std::align_val_t{align});
    return static_cast<T*>(p);
}

template <typename T>
inline void free_aligned_array(T* p, std::size_t count) {
    if (!p) {
        return;
    }
    const std::size_t align = alignof(T) < kCacheLine ? kCacheLine : alignof(T);
    ::operator delete(p, count * sizeof(T), std::align_val_t{align});
}

// Pack a Treiber-stack head: 32-bit slot index + 32-bit ABA tag.
[[nodiscard]] constexpr uint64_t pack_tagged(uint32_t idx, uint32_t tag) noexcept {
    return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(idx);
}
[[nodiscard]] constexpr uint32_t unpack_idx(uint64_t v) noexcept {
    return static_cast<uint32_t>(v);
}
[[nodiscard]] constexpr uint32_t unpack_tag(uint64_t v) noexcept {
    return static_cast<uint32_t>(v >> 32);
}

// Lock-free intrusive index stack. Safe for concurrent alloc/free; the matching
// thread is the usual caller, but producers may also claim slots.
class alignas(kCacheLine) IndexFreeList {
public:
    IndexFreeList() = default;

    void init(uint32_t* next, uint32_t n) noexcept {
        next_ = next;
        n_    = n;
        if (n == 0) {
            head_.store(pack_tagged(kInvalidIndex, 0), std::memory_order_relaxed);
            return;
        }
        for (uint32_t i = 0; i + 1 < n; ++i) {
            next_[i] = i + 1;
        }
        next_[n - 1] = kInvalidIndex;
        head_.store(pack_tagged(0, 0), std::memory_order_release);
    }

    [[nodiscard]] uint32_t pop() noexcept {
        uint64_t packed = head_.load(std::memory_order_acquire);
        for (;;) {
            const uint32_t idx = unpack_idx(packed);
            if (idx == kInvalidIndex) {
                return kInvalidIndex;
            }
            const uint32_t nxt = next_[idx];
            const uint64_t neu = pack_tagged(nxt, unpack_tag(packed) + 1);
            if (head_.compare_exchange_weak(packed, neu,
                                            std::memory_order_release,
                                            std::memory_order_acquire)) {
                next_[idx] = kInvalidIndex;
                return idx;
            }
        }
    }

    void push(uint32_t idx) noexcept {
        uint64_t packed = head_.load(std::memory_order_relaxed);
        for (;;) {
            next_[idx]         = unpack_idx(packed);
            const uint64_t neu = pack_tagged(idx, unpack_tag(packed) + 1);
            if (head_.compare_exchange_weak(packed, neu,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
                return;
            }
        }
    }

    [[nodiscard]] uint32_t remaining_approx() const noexcept {
        uint32_t n   = 0;
        uint32_t idx = unpack_idx(head_.load(std::memory_order_acquire));
        while (idx != kInvalidIndex && n < n_) {
            ++n;
            idx = next_[idx];
        }
        return n;
    }

private:
    alignas(kCacheLine) std::atomic<uint64_t> head_{pack_tagged(kInvalidIndex, 0)};
    uint32_t* next_{nullptr};
    uint32_t  n_{0};
};

// Contiguous 64-byte-strided object pool. Construction allocates; pop/push
// never touch the heap.
template <typename T, uint32_t N>
class alignas(kCacheLine) SlotPool {
    static_assert(N > 0, "pool must be non-empty");
    static_assert(alignof(T) == kCacheLine, "pooled type must be cache-line aligned");

public:
    SlotPool() {
        slots_ = alloc_aligned_array<T>(N);
        next_  = alloc_aligned_array<uint32_t>(N);
        for (uint32_t i = 0; i < N; ++i) {
            ::new (static_cast<void*>(slots_ + i)) T{};
            next_[i] = kInvalidIndex;
        }
        free_.init(next_, N);
    }

    ~SlotPool() {
        if (slots_) {
            for (uint32_t i = 0; i < N; ++i) {
                slots_[i].~T();
            }
        }
        free_aligned_array(slots_, N);
        free_aligned_array(next_, N);
        slots_ = nullptr;
        next_  = nullptr;
    }

    SlotPool(const SlotPool&)            = delete;
    SlotPool& operator=(const SlotPool&) = delete;
    SlotPool(SlotPool&&)                 = delete;
    SlotPool& operator=(SlotPool&&)      = delete;

    [[nodiscard]] uint32_t acquire() noexcept { return free_.pop(); }
    void release(uint32_t idx) noexcept { free_.push(idx); }

    [[nodiscard]] T&       operator[](uint32_t i) noexcept { return slots_[i]; }
    [[nodiscard]] const T& operator[](uint32_t i) const noexcept { return slots_[i]; }
    [[nodiscard]] T*       data() noexcept { return slots_; }
    [[nodiscard]] const T* data() const noexcept { return slots_; }
    [[nodiscard]] static constexpr uint32_t capacity() noexcept { return N; }

private:
    T*           slots_{nullptr};
    uint32_t*    next_{nullptr};
    IndexFreeList free_{};
};

// SPSC bounded ring. Producer/consumer counters live on separate cache lines
// to kill false sharing. Capacity is a power of two; one slot is left empty.
template <typename T, uint32_t Capacity>
class alignas(kCacheLine) SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "ring capacity must be a power of two");

public:
    SpscRing() {
        slots_ = alloc_aligned_array<T>(Capacity);
        for (uint32_t i = 0; i < Capacity; ++i) {
            ::new (static_cast<void*>(slots_ + i)) T{};
        }
    }

    ~SpscRing() {
        if (slots_) {
            for (uint32_t i = 0; i < Capacity; ++i) {
                slots_[i].~T();
            }
        }
        free_aligned_array(slots_, Capacity);
        slots_ = nullptr;
    }

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] bool try_push(const T& v) noexcept {
        const uint32_t w    = write_.load(std::memory_order_relaxed);
        const uint32_t next = (w + 1) & kMask;
        if (next == read_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[w] = v;
        write_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const uint32_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[r];
        read_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) ==
               write_.load(std::memory_order_acquire);
    }

    static constexpr uint32_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr uint32_t kMask = Capacity - 1;

    T* slots_{nullptr};
    alignas(kCacheLine) std::atomic<uint32_t> write_{0};
    alignas(kCacheLine) std::atomic<uint32_t> read_{0};
};

// Bounded MPSC ring (Dmitry Vyukov). Multiple gateways may enqueue; a single
// matching thread dequeues. Cells carry a sequence number to avoid wrapping
// races without a heap.
template <typename T, uint32_t Capacity>
class alignas(kCacheLine) MpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "ring capacity must be a power of two");

    struct Cell {
        std::atomic<uint32_t> seq;
        T                     data;
    };

public:
    MpscRing() {
        cells_ = alloc_aligned_array<Cell>(Capacity);
        for (uint32_t i = 0; i < Capacity; ++i) {
            ::new (static_cast<void*>(cells_ + i)) Cell{};
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    ~MpscRing() {
        if (cells_) {
            for (uint32_t i = 0; i < Capacity; ++i) {
                cells_[i].~Cell();
            }
        }
        free_aligned_array(cells_, Capacity);
        cells_ = nullptr;
    }

    MpscRing(const MpscRing&)            = delete;
    MpscRing& operator=(const MpscRing&) = delete;

    [[nodiscard]] bool try_push(const T& v) noexcept {
        uint32_t pos = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            Cell&            cell = cells_[pos & kMask];
            const uint32_t   seq  = cell.seq.load(std::memory_order_acquire);
            const int32_t    dif  = static_cast<int32_t>(seq) - static_cast<int32_t>(pos);
            if (dif == 0) {
                if (enqueue_.compare_exchange_weak(pos, pos + 1,
                                                   std::memory_order_relaxed)) {
                    cell.data = v;
                    cell.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = enqueue_.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const uint32_t pos  = dequeue_.load(std::memory_order_relaxed);
        Cell&          cell = cells_[pos & kMask];
        const uint32_t seq  = cell.seq.load(std::memory_order_acquire);
        const int32_t  dif  = static_cast<int32_t>(seq) - static_cast<int32_t>(pos + 1);
        if (dif < 0) {
            return false;
        }
        out = cell.data;
        cell.seq.store(pos + Capacity, std::memory_order_release);
        dequeue_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    static constexpr uint32_t capacity() noexcept { return Capacity; }

private:
    static constexpr uint32_t kMask = Capacity - 1;

    Cell* cells_{nullptr};
    alignas(kCacheLine) std::atomic<uint32_t> enqueue_{0};
    alignas(kCacheLine) std::atomic<uint32_t> dequeue_{0};
};

// Open-addressing map: uint64_t -> uint32_t. Pre-sized, linear probe.
// Deletes back-shift the cluster so we never need tombstones or a rehash.
// Key 0 is reserved.
template <uint32_t Capacity>
class alignas(kCacheLine) IdHash {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);

public:
    static constexpr uint64_t kEmpty = 0;

    IdHash() {
        keys_   = alloc_aligned_array<uint64_t>(Capacity);
        vals_   = alloc_aligned_array<uint32_t>(Capacity);
        for (uint32_t i = 0; i < Capacity; ++i) {
            keys_[i] = kEmpty;
            vals_[i] = kInvalidIndex;
        }
    }

    ~IdHash() {
        free_aligned_array(keys_, Capacity);
        free_aligned_array(vals_, Capacity);
        keys_ = nullptr;
        vals_ = nullptr;
    }

    IdHash(const IdHash&)            = delete;
    IdHash& operator=(const IdHash&) = delete;

    bool insert(uint64_t key, uint32_t val) noexcept {
        if (key == kEmpty) {
            return false;
        }
        uint32_t i = hash(key);
        for (uint32_t n = 0; n < Capacity; ++n) {
            const uint64_t k = keys_[i];
            if (k == key) {
                return false;
            }
            if (k == kEmpty) {
                keys_[i] = key;
                vals_[i] = val;
                ++size_;
                return true;
            }
            i = (i + 1) & kMask;
        }
        return false;
    }

    [[nodiscard]] uint32_t find(uint64_t key) const noexcept {
        if (key == kEmpty) {
            return kInvalidIndex;
        }
        uint32_t i = hash(key);
        for (uint32_t n = 0; n < Capacity; ++n) {
            const uint64_t k = keys_[i];
            if (k == kEmpty) {
                return kInvalidIndex;
            }
            if (k == key) {
                return vals_[i];
            }
            i = (i + 1) & kMask;
        }
        return kInvalidIndex;
    }

    bool erase(uint64_t key) noexcept {
        uint32_t i = hash(key);
        for (uint32_t n = 0; n < Capacity; ++n) {
            const uint64_t k = keys_[i];
            if (k == kEmpty) {
                return false;
            }
            if (k == key) {
                keys_[i] = kEmpty;
                vals_[i] = kInvalidIndex;
                --size_;
                uint32_t j = (i + 1) & kMask;
                for (;;) {
                    const uint64_t kj = keys_[j];
                    if (kj == kEmpty) {
                        break;
                    }
                    const uint32_t vj = vals_[j];
                    keys_[j]          = kEmpty;
                    vals_[j]          = kInvalidIndex;
                    --size_;
                    insert(kj, vj);
                    j = (j + 1) & kMask;
                }
                return true;
            }
            i = (i + 1) & kMask;
        }
        return false;
    }

    [[nodiscard]] uint32_t size() const noexcept { return size_; }

private:
    static constexpr uint32_t kMask = Capacity - 1;

    [[nodiscard]] static uint32_t hash(uint64_t x) noexcept {
        // SplitMix64 low bits — cheap and well-distributed for sequential ids.
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<uint32_t>(x) & kMask;
    }

    uint64_t* keys_{nullptr};
    uint32_t* vals_{nullptr};
    uint32_t  size_{0};
};

// Price -> level index. Price 0 is a valid tick, so occupancy is explicit.
template <uint32_t Capacity>
class alignas(kCacheLine) PriceHash {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);

    enum class St : uint8_t { Empty = 0, Live = 1 };

    struct alignas(16) Entry {
        int64_t  price{0};
        uint32_t level{kInvalidIndex};
        St       state{St::Empty};
    };

public:
    PriceHash() {
        entries_ = alloc_aligned_array<Entry>(Capacity);
        for (uint32_t i = 0; i < Capacity; ++i) {
            ::new (static_cast<void*>(entries_ + i)) Entry{};
        }
    }

    ~PriceHash() {
        if (entries_) {
            for (uint32_t i = 0; i < Capacity; ++i) {
                entries_[i].~Entry();
            }
        }
        free_aligned_array(entries_, Capacity);
        entries_ = nullptr;
    }

    PriceHash(const PriceHash&)            = delete;
    PriceHash& operator=(const PriceHash&) = delete;

    bool insert(int64_t price, uint32_t level) noexcept {
        uint32_t i = hash(price);
        for (uint32_t n = 0; n < Capacity; ++n) {
            Entry& e = entries_[i];
            if (e.state == St::Live && e.price == price) {
                return false;
            }
            if (e.state == St::Empty) {
                e.price = price;
                e.level = level;
                e.state = St::Live;
                return true;
            }
            i = (i + 1) & kMask;
        }
        return false;
    }

    [[nodiscard]] uint32_t find(int64_t price) const noexcept {
        uint32_t i = hash(price);
        for (uint32_t n = 0; n < Capacity; ++n) {
            const Entry& e = entries_[i];
            if (e.state == St::Empty) {
                return kInvalidIndex;
            }
            if (e.state == St::Live && e.price == price) {
                return e.level;
            }
            i = (i + 1) & kMask;
        }
        return kInvalidIndex;
    }

    bool erase(int64_t price) noexcept {
        uint32_t i = hash(price);
        for (uint32_t n = 0; n < Capacity; ++n) {
            Entry& e = entries_[i];
            if (e.state == St::Empty) {
                return false;
            }
            if (e.state == St::Live && e.price == price) {
                e.state = St::Empty;
                e.level = kInvalidIndex;
                uint32_t j = (i + 1) & kMask;
                for (;;) {
                    Entry& f = entries_[j];
                    if (f.state == St::Empty) {
                        break;
                    }
                    const int64_t  pj = f.price;
                    const uint32_t lj = f.level;
                    f.state           = St::Empty;
                    f.level           = kInvalidIndex;
                    insert(pj, lj);
                    j = (j + 1) & kMask;
                }
                return true;
            }
            i = (i + 1) & kMask;
        }
        return false;
    }

private:
    static constexpr uint32_t kMask = Capacity - 1;

    [[nodiscard]] static uint32_t hash(int64_t p) noexcept {
        uint64_t x = static_cast<uint64_t>(p) * 0x9E3779B97F4A7C15ULL;
        x ^= x >> 32;
        return static_cast<uint32_t>(x) & kMask;
    }

    Entry* entries_{nullptr};
};

}  // namespace nanomatch
