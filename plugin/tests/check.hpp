// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <cstdio>

// The tests' one assertion. A failed CHECK prints where and what, counts, and carries on, so
// one run reports every failure; main() returns non-zero when `failures` is.
inline int failures = 0;

#define CHECK(condition)                                                                              \
  do {                                                                                                \
    if (!(condition)) {                                                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                                \
      ++failures;                                                                                     \
    }                                                                                                 \
  } while (0)
