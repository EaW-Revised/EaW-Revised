#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace eawr::sim::galactic {

// A row table that copies share until one of them is written (copy on write). The production offer table of a planet is
// authored once at setup and then only read, yet every publication used to deep-copy it for every planet and every
// asynchronous consumer used to compare it row by row (#5982: about 1150 offers on each of 145 planets in a large mod).
// Reads never detach. Element writes go through the mutators below, which copy the rows first when another table still
// shares them; a table that was never shared is written in place.
template <class T> class SharedRows {
public:
    static constexpr bool shares_rows = true;
    using value_type = T;
    using const_iterator = typename std::vector<T>::const_iterator;
    using iterator = const_iterator;
    using size_type = std::size_t;
    SharedRows() = default;
    SharedRows(std::vector<T> rows) { assign(std::move(rows)); }
    SharedRows(std::initializer_list<T> rows) { assign(std::vector<T>(rows)); }
    SharedRows& operator=(std::vector<T> rows) { assign(std::move(rows)); return *this; }
    SharedRows& operator=(std::initializer_list<T> rows) { assign(std::vector<T>(rows)); return *this; }
    [[nodiscard]] const std::vector<T>& rows() const noexcept { return rows_ ? *rows_ : empty_rows(); }
    operator const std::vector<T>&() const noexcept { return rows(); }
    [[nodiscard]] bool shares_storage_with(const SharedRows& other) const noexcept { return rows_ == other.rows_; }
    [[nodiscard]] std::size_t size() const noexcept { return rows().size(); }
    [[nodiscard]] bool empty() const noexcept { return rows().empty(); }
    [[nodiscard]] const_iterator begin() const noexcept { return rows().begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return rows().end(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return rows().begin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return rows().end(); }
    [[nodiscard]] const T* data() const noexcept { return rows().data(); }
    [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return rows()[index]; }
    [[nodiscard]] const T& at(std::size_t index) const { return rows().at(index); }
    [[nodiscard]] const T& front() const noexcept { return rows().front(); }
    [[nodiscard]] const T& back() const noexcept { return rows().back(); }
    // Writers: unshare first.
    [[nodiscard]] std::vector<T>& mutable_rows() {
        if (!rows_) rows_ = std::make_shared<std::vector<T>>();
        else if (rows_.use_count() > 1) rows_ = std::make_shared<std::vector<T>>(*rows_);
        return *rows_;
    }
    [[nodiscard]] T& operator[](std::size_t index) { return mutable_rows()[index]; }
    [[nodiscard]] T& front() { return mutable_rows().front(); }
    [[nodiscard]] T& back() { return mutable_rows().back(); }
    void push_back(const T& row) { mutable_rows().push_back(row); }
    void push_back(T&& row) { mutable_rows().push_back(std::move(row)); }
    template <class... Args> T& emplace_back(Args&&... args) { return mutable_rows().emplace_back(std::forward<Args>(args)...); }
    // `position` is an iterator of this table; it is turned into an index before the rows are unshared.
    const_iterator insert(const_iterator position, const T& row) {
        const auto index = position - rows().begin();
        auto& writable = mutable_rows();
        return writable.insert(writable.begin() + index, row);
    }
    void reserve(std::size_t count) { mutable_rows().reserve(count); }
    void resize(std::size_t count) { mutable_rows().resize(count); }
    void clear() noexcept { rows_.reset(); }
    // Tables that share storage are equal without a walk.
    friend bool operator==(const SharedRows& left, const SharedRows& right) {
        return left.rows_ == right.rows_ || left.rows() == right.rows();
    }
private:
    static const std::vector<T>& empty_rows() noexcept { static const std::vector<T> none; return none; }
    void assign(std::vector<T> rows) {
        if (rows.empty()) rows_.reset(); else rows_ = std::make_shared<std::vector<T>>(std::move(rows));
    }
    std::shared_ptr<std::vector<T>> rows_;
};

} // namespace eawr::sim::galactic
