# DSA notes — NEO-DX / ASTRODSA

Study notes for the viva. One section per structure: what it does, why it was
chosen over the alternatives, and a worked example small enough to reproduce on
paper.

Code: `src/neo/dsa/`. Tests: `tests/neo_dsa_tests.cpp` (133 checks, including
~200,000 random operations per structure against the `std::` equivalent).

Sizes quoted are from the real dataset: **42,666 objects, 42,819 close
approaches**.

---

## 0. The one idea behind all of it

The master data lives once, in `std::vector<AsteroidRecord>`, and the approaches
live once, in a flat `std::vector<CloseApproach>`. **Every structure here stores
`std::uint32_t` positions into those two vectors — never a copy of a record.**

Why it matters: an `AsteroidRecord` is 400 bytes. Six sorted views over 42,666
records cost 6 × 42,666 × 4 B ≈ 1 MB of indices. Six views holding copies would
cost ≈ 102 MB, and every edit would have to be applied six times.

---

## 1. HashMap — exact lookup

`src/neo/dsa/HashMap.h`. Open addressing, Robin Hood probing, backward-shift
deletion.

**Job:** `pdes -> record index` ("433" → 17) and `spkid -> record index`.

| Operation | Time | Space |
|---|---|---|
| `find` / `insert` / `erase` | O(1) expected, O(n) worst | — |
| rehash on growth | O(n), amortised O(1) per insert | new table |
| whole table | — | capacity × sizeof(Slot), capacity a power of two |

**Real numbers:** 42,666 entries in a 65,536-slot table, load factor 0.65,
**mean probe distance 0.90, worst 12**.

### Why not separate chaining (what `std::unordered_map` does)

Chaining allocates a node per entry and follows a pointer per probe. For 42,666
designations that is 42,666 small allocations scattered in memory. Open
addressing is one block, and probing walks neighbouring slots, which the cache
prefetches. The cost is that deletion needs care — hence backward shifting.

### Why Robin Hood

Plain linear probing produces long "rich" runs: some keys sit in their ideal
slot, others 30 slots away. Robin Hood evens this out. On insert, if the entry
already in a slot is *closer to home* than the one being placed, they swap: the
key that has suffered more takes the slot ("steal from the rich, give to the
poor"). The mean probe length is unchanged — it is a property of the load factor
— but the **variance** collapses, so the worst lookup stays near the average.

### Worked example: insert with displacement

Table of 8 slots, `·` = empty. Each entry is written `key(d)` where `d` is how
far it sits from its ideal slot.

```
Start, after inserting A (ideal 3) and B (ideal 3):

 slot   0    1    2    3    4    5    6    7
       ·    ·    ·   A(0) B(1)  ·    ·    ·
```

Now insert C, whose ideal slot is also 3.

```
step 1: slot 3 holds A with d=0; C has d=0. Not poorer -> move on, C.d = 1
step 2: slot 4 holds B with d=1; C has d=1. Not poorer -> move on, C.d = 2
step 3: slot 5 is empty -> C lands there with d=2

 slot   0    1    2    3    4    5    6    7
       ·    ·    ·   A(0) B(1) C(2)  ·    ·
```

Now insert D with ideal slot **4**:

```
step 1: slot 4 holds B with d=1; D has d=0.
        B is poorer (1 > 0) -> keep going, D.d = 1
step 2: slot 5 holds C with d=2; D has d=1.
        C is poorer -> keep going, D.d = 2
step 3: slot 6 empty -> D lands with d=2
```

And the displacing case — insert E with ideal slot **6**, after the table has
E'(d=3) sitting at slot 6:

```
E arrives at slot 6 with d=0 and finds an entry with d=3.
The resident is RICHER than E? No: 3 > 0, the resident is poorer, so E moves on.

The swap happens the other way round: if E had d=4 and met a resident with d=1,
E would take the slot and the resident (now carrying d=1) would continue
searching from the next slot with d=2, 3, ...
```

The rule in one line: **the entry with the larger distance keeps the slot.**

### Worked example: backward-shift deletion

Tombstones (marking a slot "deleted") make every later probe walk past dead
slots forever. Instead, deleting shifts the run back:

```
Before, deleting B at slot 4:

 slot   3    4    5    6    7
      A(0) B(1) C(2) D(2)  ·

1. hole = 4. Look at slot 5: C has d=2 > 0, so C could have probed past the
   hole -> move C into 4 and decrement its distance to 1. hole = 5.

 slot   3    4    5    6    7
      A(0) C(1)  ?  D(2)  ·

2. hole = 5. Look at slot 6: D has d=2 > 0 -> move D into 5, d becomes 1.
   hole = 6.

 slot   3    4    5    6    7
      A(0) C(1) D(1)  ?    ·

3. hole = 6. Look at slot 7: empty -> stop. Clear slot 6.

 slot   3    4    5    6    7
      A(0) C(1) D(1)  ·     ·
```

The loop stops at the first entry with `d == 0` (it is already home, so it never
probed through the hole) or at an empty slot. The table is left exactly as if B
had never been inserted.

**Test that proves it:** 64 keys hashed to a single slot, then erasing from the
middle of the run — everything after it must still be findable.

### Why grow at 0.75

For open addressing the expected probe count grows roughly like 1/(1−α)² — at
α = 0.9 it is ~100× worse than at α = 0.5. 0.75 is the usual compromise between
that curve and wasted memory. Capacity is a power of two so `hash % capacity`
becomes `hash & (capacity − 1)`.

---

## 2. Merge sort + binary search — ordered views

`src/neo/dsa/Sort.h`. Stable merge sort over index arrays, plus my own
`lowerBound` / `upperBound`.

| Operation | Time | Space |
|---|---|---|
| `mergeSort` | O(n log n) always | one n-element scratch buffer, reused |
| `insertionSort` (≤ 16 elements) | O(k²) | in place |
| `lowerBound` / `upperBound` | O(log n) | O(1) |
| range query on a view | O(log n) + O(m) to read m results | — |

### Why merge sort and not quicksort/introsort

1. **Stability.** Ties are everywhere in this data: equal diameters, equal
   dates, whole-degree inclinations. A stable sort keeps equal keys in record
   order, so a view is *deterministic* — two runs give byte-identical results
   and the oracle tests can compare exactly. `std::sort` is introsort, which is
   not stable.
2. **No bad case.** O(n log n) regardless of input order. Quicksort's O(n²) case
   needs a pivot strategy to avoid; merge sort has none.
3. The merge is a sequential read/write, which suits index arrays.

The price is the scratch buffer (n × 4 bytes), allocated **once** by the caller
and reused by every level of the recursion — not per level, which would be the
classic waste.

### Why the insertion-sort cutoff at 16

Below ~16 elements the recursion overhead and the copy in and out of the buffer
cost more than the O(k²) comparisons on a run that already sits in cache.
Insertion sort is itself stable (it only shifts elements past *strictly* greater
ones), so the whole sort stays stable. For n = 42,666 the cutoff replaces about
5,300 leaf recursions with ~2,700 straight-line loops.

There is a second optimisation in the same place: after sorting both halves, if
`!less(first[middle], first[middle-1])` the halves are already in order and the
merge is skipped entirely. An already-sorted view therefore costs comparisons
but no data movement.

### Worked example: merge sort trace

Sorting `[5, 2, 9, 2*, 7]` (`2*` marks the second 2, to watch stability):

```
split:      [5, 2]   [9, 2*, 7]
split:      [5] [2]  [9] [2*, 7]
                          split: [2*] [7]

merge [5] [2]:        compare 2 < 5 -> take 2, then 5     -> [2, 5]
merge [2*] [7]:       compare 7 < 2*? no -> take 2*, then 7 -> [2*, 7]
merge [9] [2*, 7]:    2* < 9 -> 2* ; 7 < 9 -> 7 ; drain 9  -> [2*, 7, 9]
merge [2,5] [2*,7,9]:
      compare 2* < 2 ?  NO  (they are equal, and the test is strictly less)
                        -> take 2 from the LEFT half   [2]
      compare 2* < 5 ?  yes -> take 2*                 [2, 2*]
      compare 7  < 5 ?  no  -> take 5                  [2, 2*, 5]
      drain right: 7, 9                                [2, 2*, 5, 7, 9]
```

The stability hinges on that one line: **take from the right half only when it
is strictly smaller.** On a tie the left element goes first, and the left half
holds the earlier records.

### Sorted views: unknown is not zero

A `SortedView` holds `order` (record indices) and `keys` (their key values,
parallel). Records whose key is **unknown are not in the view at all**, and the
count is kept (`excludedUnknown`). This makes the project rule structural: a
"diameter > 100 m" query cannot accidentally match an object with no measured
diameter, because such an object is not in the diameter view. NaN is refused the
same way — it would break the ordering, since NaN compares false against
everything.

Real numbers: the measured-diameter view holds **1,264** of 42,666 objects;
**41,402 are excluded as unknown**. That number is reportable, which is the
point: the UI can say "41,402 objects have no measured diameter" instead of
quietly treating them as 0 m.

Keys are stored beside the indices so a binary search touches one contiguous
array of doubles instead of jumping into 42,666 scattered 400-byte records.

---

## 3. BinaryHeap — top-K

`src/neo/dsa/BinaryHeap.h`. Array-backed binary heap plus a `topK` helper.

| Operation | Time |
|---|---|
| `push` / `pop` | O(log n) |
| `top` | O(1) |
| `heapify` (build from an array) | **O(n)**, not O(n log n) |
| `topK` over n candidates | O(n log k) time, O(k) space |

### Why heapify is O(n)

Sifting down from every internal node looks like O(n log n), but almost all
nodes are near the bottom and barely move. Summing the real work over levels:

```
level h from the bottom has about n / 2^(h+1) nodes, each sifting at most h
total = sum over h of (n / 2^(h+1)) * h = n * sum h/2^(h+1) -> n * 1 = O(n)
```

### Why a heap for top-K instead of sorting

Sorting the candidates is O(n log n) and needs room for all of them. A heap of
exactly k elements keeps the **worst of the current best k** on top, so each new
candidate costs one comparison against that worst element, and only occasionally
an O(log k) replacement.

For the real dataset, "10 closest approaches" over 42,819 rows:

- full sort ≈ 42,819 × log₂(42,819) ≈ **650,000** comparisons
- heap of 10 ≈ 42,819 + (replacements × log₂10) ≈ **~43,000** comparisons

with a working set of 10 elements instead of a 42,819-element copy.

### The comparator, and the bug worth remembering

The heap is a **max-heap under its comparator**: `top()` is the greatest element
under `Compare`. `topK` passes the ranking predicate `better` straight in,
because *the greatest element under "ranks above" is the one that ranks last* —
the worst of the k kept, which is exactly the one a new candidate must beat.

My first version inverted the predicate, reasoning "I want the worst on top, so
invert". That inverted it twice and put the **best** on top, so every new
candidate was compared against the wrong end and `topK` returned the k *worst*
mixed with the best. The differential test against `std::partial_sort` caught it
immediately. Lesson: with a max-heap, "worst on top" is the *un*-inverted
ranking.

### Worked example: heapify

Build a max-heap from `[3, 1, 6, 5, 2, 4]` (array form, children of i are 2i+1
and 2i+2):

```
                3
             /     .
           1         6
         /   .     /
        5     2   4

Start at the last internal node, index 2 (value 6):
  children: 4. 6 > 4, nothing to do.

Index 1 (value 1): children 5 and 2, larger is 5. 1 < 5 -> swap.
                3
             /     .
           5         6
         /   .     /
        1     2   4

Index 0 (value 3): children 5 and 6, larger is 6. 3 < 6 -> swap, then
  sift 3 down into the subtree at index 2: its child is 4, 3 < 4 -> swap.
                6
             /     .
           5         4
         /   .     /
        1     2   3

Final array: [6, 5, 4, 1, 2, 3]
```

### Worked example: top-3 closest

Candidates with distances `[0.9, 0.2, 0.7, 0.4, 0.1, 0.8]`, k = 3, smaller is
better. The heap keeps the worst of the best three on top:

```
push 0.9, 0.2, 0.7      heap (worst on top): 0.9 | 0.2, 0.7
0.4 < top 0.9 ?  yes -> replaceTop(0.4)      0.7 | 0.2, 0.4
0.1 < top 0.7 ?  yes -> replaceTop(0.1)      0.4 | 0.2, 0.1
0.8 < top 0.4 ?  no  -> rejected in ONE comparison
result, sorted best-first: 0.1, 0.2, 0.4
```

---

## 4. AVL tree — ordered index and range queries

`src/neo/dsa/AvlTree.h`. Height-balanced BST over `(key, payload)` pairs, stored
in a vector node pool with `std::int32_t` links.

| Operation | Time |
|---|---|
| `find` / `insert` / `erase` | O(log n) guaranteed |
| `range(lo, hi)` | O(log n + m) for m results |
| `inOrder` | O(n) |

**Real numbers:** 42,819 approach dates, tree **height 18** (a perfectly
balanced tree would be 16; an unbalanced insert-in-date-order list would be
42,819).

### Why AVL and not an interval tree

**A close approach is an instant, not an interval.** It has a date; it has no
start and end. A date window is therefore two boundary searches plus an in-order
walk, which any balanced BST does. An interval tree exists to answer "which
stored intervals overlap this one", and pays for it with a subtree-maximum in
every node that must be maintained through every rotation. Here that machinery
would answer a question nobody asks.

If the data ever changed — an approach with a duration, or a visibility window
with a start and end — an interval tree would become the right structure.

### Why AVL and not red-black

AVL keeps height ≈ 1.44 log₂ n; red-black allows ≈ 2 log₂ n. AVL rebalances more
eagerly, which costs a little more on insert and pays back on every lookup. This
index is built once per dataset and queried repeatedly, so the read-heavy side
wins. The AVL invariant is also exactly checkable (see below), which red-black's
colour rules make fiddlier.

### Why a node pool

Nodes live in one `std::vector<Node>` and reference each other by index, not
pointer:

- building 42,819 nodes is a handful of reallocations, not 42,819 `new` calls
- the nodes are contiguous, so a traversal is cache-friendly
- an erased node's slot goes on a free list (threaded through its `left` link)
  and is reused
- indices survive copying and could be written to a file; pointers could not

### Duplicate keys

Many approaches share a date. The tree therefore orders by the composite
**(key, payload)**, where the payload is the approach index. Every entry is
unique, the primary ordering is still by key, and `range(lo, hi)` returns every
entry whose key falls in the window.

### The four rotation cases

Balance factor = height(left) − height(right). AVL allows −1, 0, +1. After an
insert or erase, exactly one of four shapes can appear on the path back up.

**Case 1 — LL (left-left): balance +2, left child leans left.** One right
rotation.

```
        z (+2)                 y
       / .                    . .
      y   D        ->        x   z
     / .                    / .  / .
    x   C                  A  B C   D
   / .
  A   B
```

**Case 2 — RR (right-right): balance −2, right child leans right.** One left
rotation. Mirror image of LL.

```
    z (-2)                     y
   / .                        . .
  A   y          ->          z   x
     / .                    / .  / .
    B   x                  A  B C   D
       / .
      C   D
```

**Case 3 — LR (left-right): balance +2, left child leans right.** Rotate the
left child left, which turns it into case LL, then rotate right.

```
      z (+2)              z                    x
     / .                 / .                  . .
    y   D               x   D                y   z
   / .        ->       / .        ->        / . / .
  A   x               y   C                A  B C  D
     / .             / .
    B   C           A   B
```

**Case 4 — RL (right-left): balance −2, right child leans left.** Rotate the
right child right, then rotate left. Mirror of LR.

Worked example, inserting 3, 1, 2 (the LR case):

```
insert 3:      3            balance 0
insert 1:      3            balance +1  (left subtree height 1, right 0)
              /
             1
insert 2:      3            balance +2 at node 3, and its left child 1
              /             leans RIGHT (balance -1) -> case LR
             1
              .
               2

step 1, rotate left about 1:      3        step 2, rotate right about 3:
                                 /                      2
                                2                      / .
                               /                      1   3
                              1
```

Height 2, balance 0 everywhere. The test builds exactly these four shapes and
asserts the resulting height and that a rotation was counted.

### The invariant checker

`checkInvariants()` verifies, for the whole tree:

1. in-order traversal is strictly increasing in (key, payload)
2. every node's stored height equals 1 + max(child heights)
3. every balance factor is in [−1, +1]
4. the node count matches `size()`

It runs every 5,000 operations during a 200,000-operation fuzz against
`std::map`, so a structural break is caught where it happens.

---

## 5. Bucket index — categorical filters

`src/neo/dsa/BucketIndex.{h,cpp}`. Counting-sorted CSR layout.

| Operation | Time |
|---|---|
| build | O(n + b): count, prefix-sum, place. No comparisons at all |
| bucket lookup | O(1) to a contiguous slice |
| year range | O(1) per year |

**Layout (compressed sparse row):** one flat `uint32_t` array with every index
grouped by bucket, plus an offsets array where bucket *i* owns
`[offset[i], offset[i+1])`. Two allocations for the whole index, and iterating a
bucket is a straight memory walk. A `vector<vector<uint32_t>>` would be one
allocation per bucket and a pointer chase per access.

**Size classes** (application-defined, *not* NASA classifications):

| Bucket | Why that boundary | Real count (measured + H-estimate) |
|---|---|---|
| < 10 m | burns up in the atmosphere | 2,707 |
| 10–50 m | Chelyabinsk (~20 m) to Tunguska (~50 m) | 17,489 |
| 50–140 m | city-scale damage | 10,455 |
| 140 m – 1 km | the PHA survey-completeness threshold | 10,872 |
| > 1 km | global effects | 954 |
| unknown | no diameter under this policy | 189 |

**Unknown is a bucket, not a gap.** Dropping those 189 objects would silently
under-report every size query; keeping them lets the UI say how much it cannot
classify.

**Diameter policy is part of the index.** A measured-only index and a
measured-plus-estimate index put different objects in different buckets, so an
index stores which policy built it. Mixing them would make "the 140 m+ bucket"
mean two different things.

**Year buckets:** approaches grouped by calendar year (TDB). Years are dense
between the first and last present (1950–2149 → 200 buckets), so a year maps to
a slot by subtraction — no hashing, no search.

---

## 6. Instrumentation

`src/neo/dsa/Instrumentation.h`. Each structure takes a `Counters` policy as its
last template parameter and **inherits from it privately**:

- `NullCounters` (the default): every call is an inline empty function, and as
  an empty base class it adds no bytes. Production code is unchanged.
- `LiveCounters`: counts comparisons, probes, swaps, rotations and moves. The
  tests instantiate the same structures with it, so the counting path is
  exercised without a second build, and stage 7 reports the numbers.

The counter methods are `const` with `mutable` fields, so a `find()` — logically
const but really costing probes — can still count.

---

## 7. What each structure replaces, and what stage 7 will compare

| Mine | `std::` equivalent | The experiment |
|---|---|---|
| `HashMap` | `std::unordered_map` | exact lookup: linear scan vs mine vs std |
| `mergeSort` + bounds | `std::stable_sort` + `std::lower_bound` | range query: linear vs sorted view vs AVL |
| `BinaryHeap` / `topK` | `std::priority_queue` / `std::partial_sort` | top-K: full sort vs heap |
| `AvlTree` | `std::map` / `std::multimap` | ordered range queries |
| `SizeBucketIndex` | (no direct equivalent) | categorical filter vs scan |

All five are already driven side by side with those `std::` containers in the
differential tests, which is what makes the stage 7 comparison meaningful: the
structures are known to be *correct* before they are measured.
