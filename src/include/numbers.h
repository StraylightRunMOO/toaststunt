/******************************************************************************
  Copyright (c) 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/


#ifndef EXT_NUMBERS_H
#define EXT_NUMBERS_H 1

#include <random>
#include <fstream>

#include "sosemanuk.h"
#include "structures.h"
#include "log.h"
#include "../dependencies/xxhash.h"

extern int parse_number(const char *str, Num *result, int try_floating_point);
extern enum error become_integer(Var, Num *, int);

extern int do_equals(Var, Var);
extern int compare_integers(Num, Num);
extern Var compare_numbers(Var, Var);

extern Var do_add(Var, Var);
extern Var do_subtract(Var, Var);
extern Var do_multiply(Var, Var);
extern Var do_divide(Var, Var);
extern Var do_modulus(Var, Var);
extern Var do_power(Var, Var);

extern sosemanuk_key_context key_context;
extern sosemanuk_run_context run_context;


static size_t sysrandom(void* dst, size_t dstlen)
{
    char* buffer = reinterpret_cast<char*>(dst);
    std::ifstream stream("/dev/urandom", std::ios_base::binary | std::ios_base::in);
    stream.read(buffer, dstlen);
    return dstlen;
}

struct splitmix64 {
  using result_type = uint64_t;

  splitmix64() { sysrandom(&state, sizeof(result_type)); }
  splitmix64(result_type s) : state(static_cast<result_type>(XXH64(&s, sizeof(result_type), UINT64_C(0x1ffffffddad23000)))) {}

  static constexpr result_type min() { return 0; }
  static constexpr result_type max() { return 0xFFFFFFFFFFFFFFFF; }
  result_type operator()() {
    result_type z = (state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
  }

  result_type state;
};

extern splitmix64 new_splitmix64(uint64_t);

inline bool 
is_round(double num) 
{
    double integral;
    double fractional = std::modf(num, &integral);
    return std::fabs(fractional) < EPSILON;
}

#endif
