// In-memory B+ tree with linked leaves and optional subtree aggregates.
//
// Every internal node caches the aggregate (a monoid fold) of each child
// subtree, so range aggregates cost O(log n) node visits: this is the
// "B-ary segment tree" used by the aggregate indexes (see ADR 0003).
//
// Invariants (checked by validate()):
//   * all leaves are at the same depth and chained left-to-right;
//   * every non-root node holds at least Order/2 keys and at most Order keys;
//   * keys in child i of an internal node lie in [keys[i-1], keys[i]);
//   * every cached aggregate equals the fold of its subtree.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aqo {

/// Aggregate policy that stores nothing; the tree behaves as a plain B+ tree.
template <class K, class V>
struct NoAggregate {
  static constexpr bool kEnabled = false;
  struct Value {};
  static Value identity() { return {}; }
  static Value lift(const K&, const V&) { return {}; }
  static Value combine(const Value&, const Value&) { return {}; }
};

/// Range with optional bounds; nullopt means unbounded on that side.
template <class K>
struct KeyRange {
  std::optional<K> lo;
  bool lo_inclusive = true;
  std::optional<K> hi;
  bool hi_inclusive = true;
};

template <class K, class V, class Agg = NoAggregate<K, V>, std::size_t Order = 64,
          class Compare = std::less<K>>
class BPlusTree {
  static_assert(Order >= 3, "Order must be at least 3");

  struct Node;
  struct Leaf;
  struct Internal;

 public:
  using key_type = K;
  using mapped_type = V;
  using AggValue = typename Agg::Value;

  BPlusTree() : root_(std::make_unique<Leaf>()) { first_leaf_ = static_cast<Leaf*>(root_.get()); }
  BPlusTree(const BPlusTree&) = delete;
  BPlusTree& operator=(const BPlusTree&) = delete;
  BPlusTree(BPlusTree&&) noexcept = default;
  BPlusTree& operator=(BPlusTree&&) noexcept = default;

  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }

  /// Number of levels including the leaf level (an empty tree has height 1).
  [[nodiscard]] std::size_t height() const {
    std::size_t h = 1;
    for (const Node* n = root_.get(); !n->leaf; n = static_cast<const Internal*>(n)->kids[0].get()) ++h;
    return h;
  }

  /// Inserts if absent. Returns false (and keeps the old value) if the key exists.
  bool insert(const K& key, V value) { return put(key, std::move(value), false); }

  /// Inserts or overwrites. Returns true if a new key was inserted.
  bool insert_or_assign(const K& key, V value) { return put(key, std::move(value), true); }

  /// Removes a key. Returns true if it was present.
  bool erase(const K& key) {
    if (!erase_rec(root_.get(), key)) return false;
    --size_;
    if (!root_->leaf) {
      auto* r = static_cast<Internal*>(root_.get());
      if (r->keys.empty()) {
        std::unique_ptr<Node> only = std::move(r->kids[0]);
        root_ = std::move(only);
      }
    }
    return true;
  }

  [[nodiscard]] V* find(const K& key) {
    Leaf* leaf = find_leaf(key);
    std::size_t i = lower_idx(leaf->keys, key);
    return (i < leaf->keys.size() && equal(leaf->keys[i], key)) ? &leaf->vals[i] : nullptr;
  }
  [[nodiscard]] const V* find(const K& key) const { return const_cast<BPlusTree*>(this)->find(key); }
  [[nodiscard]] bool contains(const K& key) const { return find(key) != nullptr; }

  /// Forward iterator over (key, value) pairs in key order, following leaf links.
  /// Any structural modification invalidates iterators.
  class const_iterator {
   public:
    using value_type = std::pair<const K&, const V&>;
    const_iterator() = default;
    [[nodiscard]] const K& key() const { return leaf_->keys[idx_]; }
    [[nodiscard]] const V& value() const { return leaf_->vals[idx_]; }
    [[nodiscard]] value_type operator*() const { return {key(), value()}; }
    const_iterator& operator++() {
      if (++idx_ >= leaf_->keys.size()) {
        leaf_ = leaf_->next;
        idx_ = 0;
        skip_empty();
      }
      return *this;
    }
    bool operator==(const const_iterator& o) const { return leaf_ == o.leaf_ && idx_ == o.idx_; }
    bool operator!=(const const_iterator& o) const { return !(*this == o); }

   private:
    friend class BPlusTree;
    const_iterator(const Leaf* l, std::size_t i) : leaf_(l), idx_(i) { skip_empty(); }
    void skip_empty() {
      while (leaf_ != nullptr && idx_ >= leaf_->keys.size()) {
        leaf_ = leaf_->next;
        idx_ = 0;
      }
    }
    const Leaf* leaf_ = nullptr;
    std::size_t idx_ = 0;
  };

  [[nodiscard]] const_iterator begin() const { return const_iterator(first_leaf_, 0); }
  [[nodiscard]] const_iterator end() const { return const_iterator(nullptr, 0); }

  /// First entry with key >= `key`.
  [[nodiscard]] const_iterator lower_bound(const K& key) const {
    const Leaf* leaf = const_cast<BPlusTree*>(this)->find_leaf(key);
    return const_iterator(leaf, lower_idx(leaf->keys, key));
  }
  /// First entry with key > `key`.
  [[nodiscard]] const_iterator upper_bound(const K& key) const {
    const Leaf* leaf = const_cast<BPlusTree*>(this)->find_leaf(key);
    return const_iterator(leaf, upper_idx(leaf->keys, key));
  }

  /// Calls fn(key, value) for each entry in `range`, in key order, via the leaf chain.
  template <class Fn>
  void scan(const KeyRange<K>& range, Fn&& fn) const {
    const_iterator it = range.lo ? (range.lo_inclusive ? lower_bound(*range.lo) : upper_bound(*range.lo)) : begin();
    for (; it != end(); ++it) {
      if (range.hi && !below_hi(it.key(), range)) break;
      fn(it.key(), it.value());
    }
  }

  /// Fold of lift(k, v) over `range`. O(Order * log n) node work, independent of range size.
  [[nodiscard]] AggValue aggregate(const KeyRange<K>& range) const {
    static_assert(Agg::kEnabled, "aggregate() requires an aggregate policy");
    return agg_range(root_.get(), range);
  }
  [[nodiscard]] AggValue aggregate_all() const { return node_agg(root_.get()); }

  /// Returns an empty string if all invariants hold, otherwise a description of the violation.
  [[nodiscard]] std::string validate() const {
    std::ostringstream err;
    std::size_t depth = 0;
    std::size_t count = 0;
    const Leaf* prev = nullptr;
    std::optional<std::size_t> leaf_depth;
    check(root_.get(), nullptr, nullptr, 0, true, leaf_depth, prev, count, err);
    if (!err.str().empty()) return err.str();
    if (count != size_) err << "size mismatch: counted " << count << " vs " << size_;
    if (prev != nullptr && prev->next != nullptr) err << "last leaf has a next pointer";
    (void)depth;
    return err.str();
  }

 private:
  struct Node {
    explicit Node(bool l) : leaf(l) {}
    virtual ~Node() = default;
    bool leaf;
  };
  struct Leaf : Node {
    Leaf() : Node(true) {}
    std::vector<K> keys;
    std::vector<V> vals;
    Leaf* next = nullptr;
    Leaf* prev = nullptr;
  };
  struct Internal : Node {
    Internal() : Node(false) {}
    std::vector<K> keys;                     // keys[i] = lower bound of kids[i+1]
    std::vector<std::unique_ptr<Node>> kids;  // keys.size() + 1 children
    std::vector<AggValue> aggs;               // aggs[i] = fold of kids[i] (iff Agg::kEnabled)
  };
  struct Split {
    K key;
    std::unique_ptr<Node> right;
  };

  static constexpr std::size_t kMin = Order / 2;

  [[nodiscard]] bool less(const K& a, const K& b) const { return cmp_(a, b); }
  [[nodiscard]] bool equal(const K& a, const K& b) const { return !cmp_(a, b) && !cmp_(b, a); }
  [[nodiscard]] std::size_t lower_idx(const std::vector<K>& v, const K& k) const {
    return static_cast<std::size_t>(std::lower_bound(v.begin(), v.end(), k, cmp_) - v.begin());
  }
  [[nodiscard]] std::size_t upper_idx(const std::vector<K>& v, const K& k) const {
    return static_cast<std::size_t>(std::upper_bound(v.begin(), v.end(), k, cmp_) - v.begin());
  }
  [[nodiscard]] bool below_hi(const K& k, const KeyRange<K>& r) const {
    return r.hi_inclusive ? !less(*r.hi, k) : less(k, *r.hi);
  }
  [[nodiscard]] bool above_lo(const K& k, const KeyRange<K>& r) const {
    return r.lo_inclusive ? !less(k, *r.lo) : less(*r.lo, k);
  }

  Leaf* find_leaf(const K& key) {
    Node* n = root_.get();
    while (!n->leaf) {
      auto* in = static_cast<Internal*>(n);
      n = in->kids[upper_idx(in->keys, key)].get();
    }
    return static_cast<Leaf*>(n);
  }

  // ---- aggregates ----
  AggValue node_agg(const Node* n) const {
    AggValue acc = Agg::identity();
    if constexpr (Agg::kEnabled) {
      if (n->leaf) {
        const auto* l = static_cast<const Leaf*>(n);
        for (std::size_t i = 0; i < l->keys.size(); ++i) acc = Agg::combine(acc, Agg::lift(l->keys[i], l->vals[i]));
      } else {
        for (const auto& a : static_cast<const Internal*>(n)->aggs) acc = Agg::combine(acc, a);
      }
    }
    return acc;
  }
  void refresh(Internal* p, std::size_t i) {
    if constexpr (Agg::kEnabled) p->aggs[i] = node_agg(p->kids[i].get());
  }

  AggValue agg_range(const Node* n, const KeyRange<K>& r) const {
    AggValue acc = Agg::identity();
    if (n->leaf) {
      const auto* l = static_cast<const Leaf*>(n);
      for (std::size_t i = 0; i < l->keys.size(); ++i) {
        if (r.lo && !above_lo(l->keys[i], r)) continue;
        if (r.hi && !below_hi(l->keys[i], r)) break;
        acc = Agg::combine(acc, Agg::lift(l->keys[i], l->vals[i]));
      }
      return acc;
    }
    const auto* in = static_cast<const Internal*>(n);
    std::size_t first = r.lo ? upper_idx(in->keys, *r.lo) : 0;
    std::size_t last = r.hi ? upper_idx(in->keys, *r.hi) : in->kids.size() - 1;
    if (last < first) return acc;
    // Children strictly between first and last lie fully inside the range (see header comment).
    for (std::size_t c = first; c <= last; ++c) {
      if (c > first && c < last) acc = Agg::combine(acc, in->aggs[c]);
      else acc = Agg::combine(acc, agg_range(in->kids[c].get(), r));
    }
    return acc;
  }

  // ---- insert ----
  bool put(const K& key, V value, bool assign) {
    bool inserted = false;
    auto split = insert_rec(root_.get(), key, std::move(value), assign, inserted);
    if (split) {
      auto new_root = std::make_unique<Internal>();
      new_root->keys.push_back(std::move(split->key));
      new_root->kids.push_back(std::move(root_));
      new_root->kids.push_back(std::move(split->right));
      if constexpr (Agg::kEnabled) {
        new_root->aggs.resize(2);
        refresh(new_root.get(), 0);
        refresh(new_root.get(), 1);
      }
      root_ = std::move(new_root);
    }
    if (inserted) ++size_;
    return inserted;
  }

  std::optional<Split> insert_rec(Node* n, const K& key, V&& value, bool assign, bool& inserted) {
    if (n->leaf) {
      auto* l = static_cast<Leaf*>(n);
      std::size_t i = lower_idx(l->keys, key);
      if (i < l->keys.size() && equal(l->keys[i], key)) {
        if (assign) l->vals[i] = std::move(value);
        return std::nullopt;
      }
      l->keys.insert(l->keys.begin() + static_cast<std::ptrdiff_t>(i), key);
      l->vals.insert(l->vals.begin() + static_cast<std::ptrdiff_t>(i), std::move(value));
      inserted = true;
      if (l->keys.size() <= Order) return std::nullopt;
      auto right = std::make_unique<Leaf>();
      std::size_t keep = l->keys.size() / 2;
      right->keys.assign(std::make_move_iterator(l->keys.begin() + static_cast<std::ptrdiff_t>(keep)),
                         std::make_move_iterator(l->keys.end()));
      right->vals.assign(std::make_move_iterator(l->vals.begin() + static_cast<std::ptrdiff_t>(keep)),
                         std::make_move_iterator(l->vals.end()));
      l->keys.resize(keep);
      l->vals.resize(keep);
      right->next = l->next;
      right->prev = l;
      if (l->next != nullptr) l->next->prev = right.get();
      l->next = right.get();
      K sep = right->keys.front();
      return Split{std::move(sep), std::move(right)};
    }
    auto* in = static_cast<Internal*>(n);
    std::size_t i = upper_idx(in->keys, key);
    auto child_split = insert_rec(in->kids[i].get(), key, std::move(value), assign, inserted);
    if constexpr (Agg::kEnabled) refresh(in, i);  // value may have changed even without a split
    if (!child_split) return std::nullopt;
    in->keys.insert(in->keys.begin() + static_cast<std::ptrdiff_t>(i), std::move(child_split->key));
    in->kids.insert(in->kids.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(child_split->right));
    if constexpr (Agg::kEnabled) {
      in->aggs.insert(in->aggs.begin() + static_cast<std::ptrdiff_t>(i) + 1, Agg::identity());
      refresh(in, i + 1);
    }
    if (in->keys.size() <= Order) return std::nullopt;
    auto right = std::make_unique<Internal>();
    std::size_t mid = in->keys.size() / 2;
    K up = std::move(in->keys[mid]);
    right->keys.assign(std::make_move_iterator(in->keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                       std::make_move_iterator(in->keys.end()));
    right->kids.assign(std::make_move_iterator(in->kids.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                       std::make_move_iterator(in->kids.end()));
    in->keys.resize(mid);
    in->kids.resize(mid + 1);
    if constexpr (Agg::kEnabled) {
      right->aggs.assign(in->aggs.begin() + static_cast<std::ptrdiff_t>(mid) + 1, in->aggs.end());
      in->aggs.resize(mid + 1);
    }
    return Split{std::move(up), std::move(right)};
  }

  // ---- erase ----
  static std::size_t key_count(const Node* n) {
    return n->leaf ? static_cast<const Leaf*>(n)->keys.size() : static_cast<const Internal*>(n)->keys.size();
  }

  bool erase_rec(Node* n, const K& key) {
    if (n->leaf) {
      auto* l = static_cast<Leaf*>(n);
      std::size_t i = lower_idx(l->keys, key);
      if (i >= l->keys.size() || !equal(l->keys[i], key)) return false;
      l->keys.erase(l->keys.begin() + static_cast<std::ptrdiff_t>(i));
      l->vals.erase(l->vals.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
    auto* in = static_cast<Internal*>(n);
    std::size_t i = upper_idx(in->keys, key);
    if (!erase_rec(in->kids[i].get(), key)) return false;
    refresh(in, i);
    if (key_count(in->kids[i].get()) < kMin) rebalance(in, i);
    return true;
  }

  // Restores the minimum occupancy of p->kids[i] by borrowing from a sibling or merging.
  void rebalance(Internal* p, std::size_t i) {
    Node* kid = p->kids[i].get();
    if (i > 0 && key_count(p->kids[i - 1].get()) > kMin) {
      borrow_from_left(p, i);
    } else if (i + 1 < p->kids.size() && key_count(p->kids[i + 1].get()) > kMin) {
      borrow_from_right(p, i);
    } else if (i > 0) {
      merge_with_next(p, i - 1);
    } else {
      merge_with_next(p, i);
    }
    (void)kid;
  }

  void borrow_from_left(Internal* p, std::size_t i) {
    Node* kid = p->kids[i].get();
    Node* left = p->kids[i - 1].get();
    if (kid->leaf) {
      auto* k = static_cast<Leaf*>(kid);
      auto* l = static_cast<Leaf*>(left);
      k->keys.insert(k->keys.begin(), std::move(l->keys.back()));
      k->vals.insert(k->vals.begin(), std::move(l->vals.back()));
      l->keys.pop_back();
      l->vals.pop_back();
      p->keys[i - 1] = k->keys.front();
    } else {
      auto* k = static_cast<Internal*>(kid);
      auto* l = static_cast<Internal*>(left);
      k->keys.insert(k->keys.begin(), std::move(p->keys[i - 1]));
      k->kids.insert(k->kids.begin(), std::move(l->kids.back()));
      l->kids.pop_back();
      p->keys[i - 1] = std::move(l->keys.back());
      l->keys.pop_back();
      if constexpr (Agg::kEnabled) {
        k->aggs.insert(k->aggs.begin(), l->aggs.back());
        l->aggs.pop_back();
      }
    }
    refresh(p, i - 1);
    refresh(p, i);
  }

  void borrow_from_right(Internal* p, std::size_t i) {
    Node* kid = p->kids[i].get();
    Node* right = p->kids[i + 1].get();
    if (kid->leaf) {
      auto* k = static_cast<Leaf*>(kid);
      auto* r = static_cast<Leaf*>(right);
      k->keys.push_back(std::move(r->keys.front()));
      k->vals.push_back(std::move(r->vals.front()));
      r->keys.erase(r->keys.begin());
      r->vals.erase(r->vals.begin());
      p->keys[i] = r->keys.front();
    } else {
      auto* k = static_cast<Internal*>(kid);
      auto* r = static_cast<Internal*>(right);
      k->keys.push_back(std::move(p->keys[i]));
      k->kids.push_back(std::move(r->kids.front()));
      r->kids.erase(r->kids.begin());
      p->keys[i] = std::move(r->keys.front());
      r->keys.erase(r->keys.begin());
      if constexpr (Agg::kEnabled) {
        k->aggs.push_back(r->aggs.front());
        r->aggs.erase(r->aggs.begin());
      }
    }
    refresh(p, i);
    refresh(p, i + 1);
  }

  // Merges p->kids[j+1] into p->kids[j] and drops the separator p->keys[j].
  void merge_with_next(Internal* p, std::size_t j) {
    Node* left = p->kids[j].get();
    Node* right = p->kids[j + 1].get();
    if (left->leaf) {
      auto* l = static_cast<Leaf*>(left);
      auto* r = static_cast<Leaf*>(right);
      l->keys.insert(l->keys.end(), std::make_move_iterator(r->keys.begin()), std::make_move_iterator(r->keys.end()));
      l->vals.insert(l->vals.end(), std::make_move_iterator(r->vals.begin()), std::make_move_iterator(r->vals.end()));
      l->next = r->next;
      if (r->next != nullptr) r->next->prev = l;
    } else {
      auto* l = static_cast<Internal*>(left);
      auto* r = static_cast<Internal*>(right);
      l->keys.push_back(std::move(p->keys[j]));
      l->keys.insert(l->keys.end(), std::make_move_iterator(r->keys.begin()), std::make_move_iterator(r->keys.end()));
      l->kids.insert(l->kids.end(), std::make_move_iterator(r->kids.begin()), std::make_move_iterator(r->kids.end()));
      if constexpr (Agg::kEnabled) l->aggs.insert(l->aggs.end(), r->aggs.begin(), r->aggs.end());
    }
    p->keys.erase(p->keys.begin() + static_cast<std::ptrdiff_t>(j));
    p->kids.erase(p->kids.begin() + static_cast<std::ptrdiff_t>(j) + 1);
    if constexpr (Agg::kEnabled) p->aggs.erase(p->aggs.begin() + static_cast<std::ptrdiff_t>(j) + 1);
    refresh(p, j);
  }

  // ---- validation ----
  void check(const Node* n, const K* lo, const K* hi, std::size_t depth, bool is_root,
             std::optional<std::size_t>& leaf_depth, const Leaf*& prev, std::size_t& count,
             std::ostringstream& err) const {
    if (!err.str().empty()) return;
    if (n->leaf) {
      const auto* l = static_cast<const Leaf*>(n);
      if (!leaf_depth) leaf_depth = depth;
      else if (*leaf_depth != depth) { err << "leaves at different depths"; return; }
      if (!is_root && l->keys.size() < kMin) { err << "leaf underflow"; return; }
      if (l->keys.size() > Order) { err << "leaf overflow"; return; }
      if (l->keys.size() != l->vals.size()) { err << "leaf key/value size mismatch"; return; }
      for (std::size_t i = 0; i < l->keys.size(); ++i) {
        if (i > 0 && !less(l->keys[i - 1], l->keys[i])) { err << "leaf keys not strictly sorted"; return; }
        if (lo != nullptr && less(l->keys[i], *lo)) { err << "leaf key below lower bound"; return; }
        if (hi != nullptr && !less(l->keys[i], *hi)) { err << "leaf key not below upper bound"; return; }
      }
      if (prev != nullptr && prev->next != l) { err << "broken next link"; return; }
      if (l->prev != prev) { err << "broken prev link"; return; }
      if (prev == nullptr && l != first_leaf_) { err << "first_leaf_ is stale"; return; }
      prev = l;
      count += l->keys.size();
      return;
    }
    const auto* in = static_cast<const Internal*>(n);
    if (in->kids.size() != in->keys.size() + 1) { err << "internal fanout mismatch"; return; }
    if (is_root ? in->keys.empty() : in->keys.size() < kMin) { err << "internal underflow"; return; }
    if (in->keys.size() > Order) { err << "internal overflow"; return; }
    for (std::size_t i = 1; i < in->keys.size(); ++i)
      if (!less(in->keys[i - 1], in->keys[i])) { err << "internal keys not sorted"; return; }
    if constexpr (Agg::kEnabled) {
      if (in->aggs.size() != in->kids.size()) { err << "aggs size mismatch"; return; }
    }
    for (std::size_t i = 0; i < in->kids.size(); ++i) {
      const K* clo = i == 0 ? lo : &in->keys[i - 1];
      const K* chi = i == in->keys.size() ? hi : &in->keys[i];
      check(in->kids[i].get(), clo, chi, depth + 1, false, leaf_depth, prev, count, err);
      if constexpr (Agg::kEnabled) {
        if (err.str().empty() && !agg_equal(in->aggs[i], node_agg(in->kids[i].get()))) {
          err << "stale cached aggregate";
          return;
        }
      }
    }
  }
  template <class A>
  static bool agg_equal(const A& a, const A& b) {
    if constexpr (requires { a == b; }) return a == b;
    else return true;
  }

  std::unique_ptr<Node> root_;
  Leaf* first_leaf_ = nullptr;
  std::size_t size_ = 0;
  [[no_unique_address]] Compare cmp_{};
};

}  // namespace aqo
