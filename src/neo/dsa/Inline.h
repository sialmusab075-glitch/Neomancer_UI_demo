#pragma once

// NEO_NOINLINE keeps a hot loop in a function of its own. Inlining a loop into a large caller can leave it
// without registers: the loop counter and a spilled temporary then live on the stack, and every iteration
// waits on a store-to-load forward. Measured on dsa::topK with GCC 13: the same source took 1.2 ms inlined
// into the benchmark and 0.48 ms as its own function (docs/DSA_NOTES.md, "Why the heap looked slow"). The
// call costs nanoseconds; the loop it protects runs once per candidate.
#if defined(_MSC_VER)
#define NEO_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define NEO_NOINLINE __attribute__((noinline))
#else
#define NEO_NOINLINE
#endif
