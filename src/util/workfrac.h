// Copyright (c) The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_UTIL_WORKFRAC_H
#define QUICKSILVER_UTIL_WORKFRAC_H

#include <stdint.h>
#include <compare>
#include <vector>
#include <span.h>
#include <util/check.h>

/** Data structure storing a work and size, ordered by increasing work/size.
 *
 * The size of a WorkFrac cannot be zero unless the work is also zero.
 *
 * WorkFracs have a total ordering, first by increasing workrate (ratio of work over size), and then
 * by decreasing size. The empty WorkFrac (work and size both 0) sorts last. So for example, the
 * following WorkFracs are in sorted order:
 *
 * - work=0 size=1 (workrate 0)
 * - work=1 size=2 (workrate 0.5)
 * - work=2 size=3 (workrate 0.667...)
 * - work=2 size=2 (workrate 1)
 * - work=1 size=1 (workrate 1)
 * - work=3 size=2 (workrate 1.5)
 * - work=2 size=1 (workrate 2)
 * - work=0 size=0 (undefined workrate)
 *
 * A WorkFrac is considered "better" if it sorts after another, by this ordering. All standard
 * comparison operators (<=>, ==, !=, >, <, >=, <=) respect this ordering.
 *
 * The WorkRateCompare, and >> and << operators only compare workrate and treat equal workrate but
 * different size as equivalent. The empty WorkFrac is neither lower or higher in workrate than any
 * other.
 */
struct WorkFrac
{
    /** Fallback version for Mul (see below).
     *
     * Separate to permit testing on platforms where it isn't actually needed.
     */
    static inline std::pair<int64_t, uint32_t> MulFallback(int64_t a, int32_t b) noexcept
    {
        // Otherwise, emulate 96-bit multiplication using two 64-bit multiplies.
        int64_t low = int64_t{static_cast<uint32_t>(a)} * b;
        int64_t high = (a >> 32) * b;
        return {high + (low >> 32), static_cast<uint32_t>(low)};
    }

    // Compute a * b, returning an unspecified but totally ordered type.
#ifdef __SIZEOF_INT128__
    static inline __int128 Mul(int64_t a, int32_t b) noexcept
    {
        // If __int128 is available, use 128-bit wide multiply.
        return __int128{a} * b;
    }
#else
    static constexpr auto Mul = MulFallback;
#endif

    int64_t work;
    int32_t size;

    /** Construct an IsEmpty() WorkFrac. */
    constexpr inline WorkFrac() noexcept : work{0}, size{0} {}

    /** Construct a WorkFrac with specified work and size. */
    constexpr inline WorkFrac(int64_t f, int32_t s) noexcept : work{f}, size{s} {}

    constexpr inline WorkFrac(const WorkFrac&) noexcept = default;
    constexpr inline WorkFrac& operator=(const WorkFrac&) noexcept = default;

    /** Check if this is empty (size and work are 0). */
    bool inline IsEmpty() const noexcept {
        return size == 0;
    }

    /** Add work and size of another WorkFrac to this one. */
    void inline operator+=(const WorkFrac& other) noexcept
    {
        work += other.work;
        size += other.size;
    }

    /** Subtract work and size of another WorkFrac from this one. */
    void inline operator-=(const WorkFrac& other) noexcept
    {
        work -= other.work;
        size -= other.size;
    }

    /** Sum work and size. */
    friend inline WorkFrac operator+(const WorkFrac& a, const WorkFrac& b) noexcept
    {
        return {a.work + b.work, a.size + b.size};
    }

    /** Subtract both work and size. */
    friend inline WorkFrac operator-(const WorkFrac& a, const WorkFrac& b) noexcept
    {
        return {a.work - b.work, a.size - b.size};
    }

    /** Check if two WorkFrac objects are equal (both same work and same size). */
    friend inline bool operator==(const WorkFrac& a, const WorkFrac& b) noexcept
    {
        return a.work == b.work && a.size == b.size;
    }

    /** Compare two WorkFracs just by workrate. */
    friend inline std::weak_ordering WorkRateCompare(const WorkFrac& a, const WorkFrac& b) noexcept
    {
        auto cross_a = Mul(a.work, b.size), cross_b = Mul(b.work, a.size);
        return cross_a <=> cross_b;
    }

    /** Check if a WorkFrac object has strictly lower workrate than another. */
    friend inline bool operator<<(const WorkFrac& a, const WorkFrac& b) noexcept
    {
        auto cross_a = Mul(a.work, b.size), cross_b = Mul(b.work, a.size);
        return cross_a < cross_b;
    }

    /** Check if a WorkFrac object has strictly higher workrate than another. */
    friend inline bool operator>>(const WorkFrac& a, const WorkFrac& b) noexcept
    {
        auto cross_a = Mul(a.work, b.size), cross_b = Mul(b.work, a.size);
        return cross_a > cross_b;
    }

    /** Compare two WorkFracs. <, >, <=, and >= are auto-generated from this. */
    friend inline std::strong_ordering operator<=>(const WorkFrac& a, const WorkFrac& b) noexcept
    {
        auto cross_a = Mul(a.work, b.size), cross_b = Mul(b.work, a.size);
        if (cross_a == cross_b) return b.size <=> a.size;
        return cross_a <=> cross_b;
    }

    /** Swap two WorkFracs. */
    friend inline void swap(WorkFrac& a, WorkFrac& b) noexcept
    {
        std::swap(a.work, b.work);
        std::swap(a.size, b.size);
    }
};

/** Compare the workrate diagrams implied by the provided sorted chunks data.
 *
 * The implied diagram for each starts at (0, 0), then contains for each chunk the cumulative work
 * and size up to that chunk, and then extends infinitely to the right with a horizontal line.
 *
 * The caller must guarantee that the sum of the WorkFracs in either of the chunks' data set do not
 * overflow (so sum works < 2^63, and sum sizes < 2^31).
 */
std::partial_ordering CompareChunks(Span<const WorkFrac> chunks0, Span<const WorkFrac> chunks1);

#endif // QUICKSILVER_UTIL_WORKFRAC_H
