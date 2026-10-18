# ADR 0001: C++20, CMake, GoogleTest, legacy code quarantined

Status: accepted

## Context
The original repository was a set of single-file experiments (`using namespace std`, raw owning pointers,
CSV persistence, no tests, built with a hand-typed `g++` line). Upgrading it in place would mix prototype code
with engine code.

## Decision
* New engine lives in `include/aqo` (+ `src/` once non-template code appears); the old code is moved
  unchanged to `legacy/` with `git mv` so history is preserved and nothing is silently dropped.
* C++20 (concepts-lite features, `[[no_unique_address]]`, defaulted `operator==`), CMake >= 3.20, GoogleTest.
* Warnings on (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`); sanitizers via `-DAQO_SANITIZE=ON`.
* The B+ tree is a header-only template because key/value/aggregate types vary (ADR 0002, 0003).

## Consequences
* Needs GCC >= 11 or Clang >= 14. The MinGW 6.3 in the original author's PATH cannot build it; MSYS2 UCRT64 can.
* ASan is not available on MinGW-w64, so the sanitizer job runs on Linux CI only.
