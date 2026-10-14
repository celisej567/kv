#pragma once
#include <cstdio>

// Detailed logging, off by default (the library only prints errors then). Build with -DKV_VERBOSE
// (CMake: -DKV_VERBOSE=ON) to get what a load and a write do, one "[kv] ..." line each, on stderr.
// With the define off, KV_LOG expands to nothing and its arguments are not evaluated.
#ifdef KV_VERBOSE
#define KV_LOG(...) (std::fprintf(stderr, "[kv] " __VA_ARGS__))
#else
#define KV_LOG(...) ((void)0)
#endif
