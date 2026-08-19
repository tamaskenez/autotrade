#pragma once

#include <meadow/cppext.h>

// Stores a Key -> T interval map, append-only, meaning a new key must be greater than the previous
// ones.
template<class Key, class T>
class AppendableIntervals
{
public:
    using key_type = Key;
    using mapped_type = T;
    using const_iterator = vector<pair<Key, T>>::const_iterator;
    using iterator = const_iterator;

    explicit AppendableIntervals(const T& before_begin_arg)
        : before_begin(before_begin_arg)
    {
    }

    // key must be greater than the previous ones.
    // The previous, last interval gets truncated to [key_prev, key)
    // and a new [key, end) interval is added.
    template<class M>
    void insert_at_end(const Key& key, M&& value)
    {
        if (changes.empty()) {
            if (value != before_begin) {
                changes.emplace_back(key, std::forward<M>(value));
            }
            return;
        }
        CHECK(changes.back().first < key);
        if (changes.back().second != value) {
            changes.emplace_back(key, std::forward<M>(value));
        }
    }

    // Erase all intervals from or above key.
    void erase_from(const Key& key)
    {
        while (!changes.empty() && key <= changes.back().first) {
            changes.pop_back();
        }
    }

    // Return vector where v[0] corresponds to the value at `key_begin`, v[1] to the value at `key_begin + 1`, etc..
    // `Key` must be incrementable and `key_end - key_begin` must be an integral.
    // For now, Key is required to be an integral, this could be relaxed to input iterator or random access iterator.
    NODIS vector<T> get_values_for_key_range(Key key_begin, const Key& key_end) const
        requires std::integral<Key>
    {
        if (key_begin == key_end) {
            return {};
        }
        CHECK(key_begin < key_end);
        vector<T> result;
        result.reserve(iicast<size_t>(key_end - key_begin));
        auto it = ra::upper_bound(changes, key_begin, {}, &stored_value_type::first);
        if (it == changes.begin()) {
            const auto stop = it == changes.end() || key_end <= it->first ? key_end : it->first;
            result.assign(iicast<size_t>(stop - key_begin), before_begin);
            if (stop == key_end) {
                return result;
            }
            key_begin = stop;
        } else {
            --it;
        }
        for (;;) {
            CHECK(it != changes.end());
            const auto next = std::next(it);
            const auto stop = next == changes.end() || key_end <= next->first ? key_end : next->first;
            CHECK(key_begin < stop);
            result.insert(result.end(), iicast<size_t>(stop - key_begin), it->second);
            if (stop == key_end) {
                break;
            }
            it = next;
            key_begin = stop;
        }
        return result;
    }

    NODIS const_iterator begin() const
    {
        return changes.begin();
    }

    NODIS const_iterator end() const
    {
        return changes.end();
    }

private:
    T before_begin;
    using stored_value_type = pair<Key, T>;
    vector<stored_value_type> changes;
};
