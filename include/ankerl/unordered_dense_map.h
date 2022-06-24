///////////////////////// ankerl::unordered_dense_map /////////////////////////

// A fast & densely stored hashmap based on robin-hood backward shift deletion.
// Version 0.0.1
// https://github.com/martinus/unordered_dense_map
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2022 Martin Leitner-Ankerl <martin.ankerl@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef ANKERL_UNORDERED_DENSE_MAP_H
#define ANKERL_UNORDERED_DENSE_MAP_H

// see https://semver.org/spec/v2.0.0.html
#define ANKERL_UNORDERED_DENSE_MAP_VERSION_MAJOR 0 // incompatible API changes
#define ANKERL_UNORDERED_DENSE_MAP_VERSION_MINOR 0 // add functionality in a backwards compatible manner
#define ANKERL_UNORDERED_DENSE_MAP_VERSION_PATCH 1 // backwards compatible bug fixes

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ankerl {

template <class Key, class T, class Hash = std::hash<Key>, class Pred = std::equal_to<Key>>
class unordered_dense_map {
public:
    using value_type = std::pair<Key, T>;
    using size_type = size_t;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

private:
    struct Bucket {
        static constexpr uint32_t Inc = 256;
        uint32_t info{}; // lower 8 bits are hash, upper 24 bits are offset to original bucket
        uint32_t idx{};  // index into m_values
    };

    std::vector<value_type> m_values{};
    Bucket* m_buckets_start{};
    Bucket* m_buckets_end{};
    uint32_t m_shifts{};
    Hash m_hash{};
    Pred m_equals{};

    [[nodiscard]] auto next(Bucket const* bucket) const -> Bucket const* {
        if (++bucket == m_buckets_end) {
            return m_buckets_start;
        }
        return bucket;
    }

    [[nodiscard]] auto next(Bucket* bucket) const -> Bucket* {
        if (++bucket == m_buckets_end) {
            return m_buckets_start;
        }
        return bucket;
    }

    auto next_while_less(size_t hash) -> std::pair<uint32_t, Bucket*> {
        auto const& pair = std::as_const(*this).next_while_less(hash);
        return {pair.first, const_cast<Bucket*>(pair.second)};
    }

    auto next_while_less(size_t hash) const -> std::pair<uint32_t, Bucket const*> {
        uint64_t h = static_cast<uint64_t>(hash) * UINT64_C(0x9E3779B97F4A7C15);

        // use lowest 8 bit for the info hash
        auto info = Bucket::Inc | (h & UINT64_C(0xFF));

        // use upper bits for the bucket index
        auto const* bucket = m_buckets_start + (h >> m_shifts);
        while (info < bucket->info) {
            ++info;
            bucket = next(bucket);
        }
        return {info, bucket};
    }

    void shift_up(Bucket* start, Bucket* end) {
        if (end < start) {
            std::move_backward(m_buckets_start, end - 1, end);
            *m_buckets_start = *(m_buckets_end - 1);
            std::move_backward(start, m_buckets_end - 1, m_buckets_end);
        } else {
            std::move_backward(start, end - 1, end);
        }
    }

public:
    unordered_dense_map() {
        m_buckets_start = new Bucket[1024];
        m_buckets_end = m_buckets_start + 1024;
        m_shifts = 64 - 10;
    }

    template <typename K>
    auto find(K const& key) const -> std::pair<Key, T> const* {
        auto [info, bucket] = nextWhileLess(m_hash(key));

        while (info == bucket->info) {
            if (m_equals(key, m_values[bucket->idx].first)) {
                return &m_values[bucket->idx];
            }
            ++info;
            bucket = next(bucket);
        }
        return nullptr;
    }

    auto operator[](Key const& key) -> T& {
        return try_emplace(key).first->second;
    }

    auto insert(value_type const& value) -> std::pair<iterator, bool> {
        return try_emplace(value.first, value.second);
    }

    template <typename K, typename... Args>
    auto try_emplace(K&& key, Args&&... args) -> std::pair<iterator, bool> {
        auto [info, bucket] = next_while_less(m_hash(key));

        while (info == bucket->info) {
            if (m_equals(key, m_values[bucket->idx].first)) {
                // key found!
                return std::make_pair(m_values.begin() + bucket->idx, false);
            }

            ++info;
            bucket = next(bucket);
        }

        // key not found, so we are now exactly where we want to insert it
        auto* const insertion_bucket = bucket;
        (void)insertion_bucket;

        // emplace the new value. If that throws an exception, no harm done; index is still in a
        // valid state
        m_values.emplace_back(std::piecewise_construct,
                              std::forward_as_tuple(std::forward<K>(key)),
                              std::forward_as_tuple(std::forward<Args>(args)...));

        // place element and shift up until we find an empty spot
        auto tmp = Bucket{info, static_cast<uint32_t>(m_values.size()) - 1};
        while (0 != bucket->info) {
            tmp = std::exchange(*bucket, tmp);
            tmp.info += Bucket::Inc;
            bucket = next(bucket);
        }
        *bucket = tmp;

        return std::make_pair(m_values.begin() + bucket->idx, true);
    }

    size_t erase(Key const& key) {
        auto [info, bucket] = nextWhileLess(m_hash(key));

        while (info == bucket->info && !m_equals(key, m_values[bucket->idx].first)) {
            ++info;
            bucket = next(bucket);
        }

        if (info != bucket->info) {
            // not found
            return 0;
        }

        // found it!
    }

    [[nodiscard]] auto size() const -> size_t {
        return m_values.size();
    }
};

} // namespace ankerl

#endif