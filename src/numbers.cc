/******************************************************************************
  Copyright (c) 1992, 1995, 1996 Xerox Corporation.  All rights reserved.
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

#include <sys/types.h> //for u_int64_t
#include <limits.h>
#include <errno.h>
#include <float.h>
#include <random>
#include <chrono>
#include <algorithm>
#include <functional>
#include <fstream>
#ifdef __MACH__
#include <mach/clock.h>     // Millisecond time for macOS
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include "my-math.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctime>
#include <iomanip>
#include "config.h"
#include "functions.h"
#include "log.h"
#include "numbers.h"
#include "random.h"
#include "server.h"
#include "dependencies/sosemanuk.h"
#include "storage.h"
#include "streams.h"
#include "structures.h"
#include "utils.h"
#include "list.h"

sosemanuk_key_context key_context;
sosemanuk_run_context run_context;

static std::mt19937_64 rng;
static splitmix64 s64;

splitmix64 new_splitmix64(uint64_t seed = 0) {
    return (seed == 0) ? splitmix64() : splitmix64(seed);
}

static void reseed_rng()
{
    /*
    s64 = new_splitmix64();
    std::array<std::uint_fast64_t, std::mt19937_64::state_size> state;
    sysrandom(state.begin(), state.size() * sizeof(std::uint_fast64_t));
    std::seed_seq data(state.begin(), state.end());
    rng.seed(data);
    */
}

int
parse_number(const char *str, Num *result, int try_floating_point)
{
    char *p;

    *result = (Num) strtoimax(str, &p, 10);
    if (try_floating_point &&
            (p == str || *p == '.' || *p == 'e' || *p == 'E'))
        *result = (Num) strtod(str, &p);
    if (p == str)
        return 0;
    while (*p) {
        if (*p != ' ')
            return 0;
        p++;
    }
    return 1;
}

static int
parse_object(const char *str, Objid * result)
{
    Num number;

    while (*str && *str == ' ')
        str++;
    if (*str == '#')
        str++;
    if (parse_number(str, &number, 0)) {
        *result = number;
        return 1;
    } else
        return 0;
}

int
parse_float(const char *str, double *result)
{
    char *p;
    int negative = 0;

    while (*str && *str == ' ')
        str++;
    if (*str == '-') {
        str++;
        negative = 1;
    }
    *result = strtod(str, &p);
    if (p == str)
        return 0;
    while (*p) {
        if (*p != ' ')
            return 0;
        p++;
    }
    if (negative)
        *result = -*result;
    return 1;
}

enum error
become_integer(Var in, Num *ret, int called_from_toint)
{
    switch (in.type) {
        case TYPE_INT:
                    *ret = in.v.num;
            break;
        case TYPE_STR:
            if (!(called_from_toint
                    ? parse_number(in.v.str, ret, 1)
                    : parse_object(in.v.str, ret)))
                *ret = 0;
            break;
        case TYPE_OBJ:
            *ret = in.v.obj;
            break;
        case TYPE_ERR:
            *ret = in.v.err;
            break;
        case TYPE_FLOAT:
            if (!IS_REAL(in.v.fnum))
                return E_FLOAT;
            *ret = (Num) in.v.fnum;
            break;
        case TYPE_COMPLEX:
            *ret = static_cast<Num>(in.v.complex.real());
            break;
        case TYPE_BOOL:
            *ret = in.v.truth;
            break;
        case TYPE_MAP:
        case TYPE_LIST:
        case TYPE_ANON:
        case TYPE_WAIF:
            return E_TYPE;
        default:
            errlog("BECOME_INTEGER: Impossible var type: %d\n", (int) in.type);
    }
    return E_NONE;
}

static enum error
become_float(Var in, double *ret)
{
    switch (in.type) {
        case TYPE_INT:
                    *ret = (double) in.v.num;
            break;
        case TYPE_STR:
            if (!parse_float(in.v.str, ret) || !IS_REAL(*ret))
                return E_INVARG;
            break;
        case TYPE_OBJ:
            *ret = (double) in.v.obj;
            break;
        case TYPE_ERR:
            *ret = (double) in.v.err;
            break;
        case TYPE_FLOAT:
            *ret = in.v.fnum;
            break;
        case TYPE_COMPLEX:
            *ret = static_cast<double>(in.v.complex.real());
            break;
        case TYPE_MAP:
        case TYPE_LIST:
        case TYPE_ANON:
        case TYPE_WAIF:
        case TYPE_BOOL:
            return E_TYPE;
        default:
            errlog("BECOME_FLOAT: Impossible var type: %d\n", (int) in.type);
    }
    return E_NONE;
}

/**** opcode implementations ****/

/*
 * All of the following implementations are strict, not performing any
 * coercions between integer and floating-point operands.
 */

int
do_equals(Var lhs, Var rhs)
{   /* LHS == RHS */
    /* At least one of LHS and RHS is TYPE_FLOAT */

    if (lhs.type != rhs.type)
        return 0;
    else
        return lhs.v.fnum == rhs.v.fnum;
}

int
compare_integers(Num a, Num b)
{
    if (a < b)
        return -1;
    else if (a > b)
        return 1;
    else
        return 0;
}

Var
compare_numbers(Var a, Var b)
{
    Var ans;

    if (a.type != b.type) {
        ans.type = TYPE_ERR;
        ans.v.err = E_TYPE;
    } else if (a.type == TYPE_INT) {
        ans.type = TYPE_INT;
        if (a.v.num < b.v.num)
            ans.v.num = -1;
        else if (a.v.num > b.v.num)
            ans.v.num = 1;
        else
            ans.v.num = 0;
    } else {
        ans.type = TYPE_INT;
        if (a.v.fnum < b.v.fnum)
            ans.v.num = -1;
        else if (a.v.fnum > b.v.fnum)
            ans.v.num = 1;
        else
            ans.v.num = 0;
    }

    return ans;
}

#define SIMPLE_BINARY(name, op)                 \
    Var                                         \
    do_ ## name(Var a, Var b)                   \
    {                                           \
        Var ans;                                \
                                                \
        if (a.type != b.type) {                 \
            if(b.type == TYPE_COMPLEX)          \
                ans = Var::new_complex(complex_t(a.fnum(), 0.0) op b.complex()); \
            else if(a.type == TYPE_COMPLEX)     \
                ans = Var::new_complex(a.complex() op complex_t(b.fnum(), 0.0)); \
            else                                \
                ans = (a.is_float() || b.is_float()) ? Var::new_float(a.fnum() op b.fnum()) : Var::new_err(E_TYPE); \
        } else if(a.is_complex()) {             \
            ans = Var::new_complex(a.complex() op b.complex()); \
        } else if (a.is_int()) {                \
            ans = Var::new_int(a.num() op b.num()); \
        } else {                                \
            double d = a.fnum() op b.fnum();    \
            \
            if (!IS_REAL(d)) {                  \
                ans.type = TYPE_ERR;            \
                ans.v.err = E_FLOAT;            \
            } else {                            \
                ans.type = TYPE_FLOAT;          \
                ans.v.fnum = d;                 \
            }                                   \
        }                                       \
                                                \
        return ans;                             \
    }

SIMPLE_BINARY(add, +)
SIMPLE_BINARY(subtract, -)
SIMPLE_BINARY(multiply, *)

Var
do_modulus(Var a, Var b)
{
    Var ans;

    if(!a.is_complex() && !b.is_complex() && b.fnum() == 0.0) {
        ans.type = TYPE_ERR;
        ans.v.err = E_DIV;
    } else if (a.type != b.type) {
        if(b.type == TYPE_COMPLEX) {
            complex_t lhs = complex_t(a.fnum(), 0.0);
            ans = Var::new_float(std::sqrt(std::pow(lhs.real() - b.complex().real(), 2.0) + std::pow(lhs.imag() - b.complex().imag(), 2.0)));
        } else if(a.type == TYPE_COMPLEX) {
            complex_t rhs = complex_t(b.fnum(), 0.0);
            ans = Var::new_float(std::sqrt(std::pow(a.complex().real() - rhs.real(), 2.0) + std::pow(a.complex().imag() - rhs.imag(), 2.0)));
        } else
            ans = (a.is_float() || b.is_float()) ? Var::new_float(a.fnum() / b.fnum()) : Var::new_err(E_TYPE);
    } else if(a.is_complex()) {
        ans = Var::new_float(std::sqrt(std::pow(a.complex().real() - b.complex().real(), 2.0) + std::pow(a.complex().imag() - b.complex().imag(), 2.0)));
    } else {
        if (a.type == TYPE_INT)
        {
            const auto n = a.v.num;
            const auto d = b.v.num;
            const auto result = (n % d + d) % d;
            ans.type = TYPE_INT;
            ans.v.num = result;
        }
        else
        {
            const auto n = a.v.fnum;
            const auto d = b.v.fnum;
            const auto result = fmod((fmod(n, d) + d), d);
            ans.type = TYPE_FLOAT;
            ans.v.fnum = result;
        }
    }
    return ans;
}

Var
do_divide(Var a, Var b)
{
    using namespace std::complex_literals;

    Var ans;

    if(!b.is_complex() && b.fnum() == 0.0) {
        ans.type = TYPE_ERR;
        ans.v.err = E_DIV;
    } else if (a.type != b.type) {
        if(b.type == TYPE_COMPLEX)
            ans = Var::new_complex(complex_t(a.fnum(), 0.0) / b.complex());
        else if(a.type == TYPE_COMPLEX)
            ans = Var::new_complex(a.complex() / complex_t(b.fnum(), 0.0));
        else
            ans = (a.is_float() || b.is_float()) ? Var::new_float(a.fnum() / b.fnum()) : Var::new_err(E_TYPE);
    } else if(a.is_complex()) {
        ans = (b.complex() == static_cast<complex_t>(0.0 + 0i)) ? Var::new_err(E_DIV) : Var::new_complex(a.complex() / b.complex());
    } else if (a.type == TYPE_INT) {
        ans.type = TYPE_INT;
        if (a.v.num == MININT && b.v.num == -1)
            ans.v.num = MININT;
        else
            ans.v.num = a.v.num / b.v.num;
    } else { // must be float
        double d = a.v.fnum / b.v.fnum;
        if (!IS_REAL(d)) {
            ans.type = TYPE_ERR;
            ans.v.err = E_FLOAT;
        } else {
            ans.type = TYPE_FLOAT;
            ans.v.fnum = d;
        }
    }

    return ans;
}

static inline float maybe_round(float n) {
    return trunc(abs(n) + 0.000001) > abs(n) ? std::roundf(n) : n;
}

Var
do_power(Var a, Var b)
{
    Var ans;

    if(a.type == b.type) {
        switch(a.type) {
        case TYPE_INT:
            ans = Var::new_int(std::pow(a.num(), b.num()));
            break;
        case TYPE_FLOAT:
            ans = Var::new_float(std::pow(a.fnum(), b.fnum()));
            break;
        case TYPE_COMPLEX:
            ans = Var::new_complex(std::pow(a.complex(), b.complex()));
            break;
        default:
            ans = Var::new_err(E_TYPE);
        }
    } else {
        if(b.type == TYPE_COMPLEX) {
            complex_t lhs = complex_t(static_cast<float>(a.fnum()), 0.0);
            complex_t res = std::pow(lhs, b.complex());
            ans = Var::new_complex(complex_t(maybe_round(res.real()), maybe_round(res.imag())));
        } else if(a.type == TYPE_COMPLEX) {
            complex_t rhs = complex_t(static_cast<float>(b.fnum()), 0.0);
            complex_t res = std::pow(a.complex(), rhs);
            ans = Var::new_complex(complex_t(maybe_round(res.real()), maybe_round(res.imag())));
        } else
            ans = (a.is_float() || b.is_float()) ? Var::new_float(std::pow(a.fnum(), b.fnum())) : Var::new_err(E_TYPE);
    }

    return ans;
}

/**** built in functions ****/

static package
bf_toint(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    enum error e;

    r.type = TYPE_INT;
    e = become_integer(arglist[1], &(r.v.num), 1);

    free_var(arglist);
    if (e != E_NONE)
        return make_error_pack(e);

    return make_var_pack(r);
}

static package
bf_tofloat(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    enum error e;

    r.type = TYPE_FLOAT;
    e = become_float(arglist[1], &r.v.fnum);

    free_var(arglist);
    if (e != E_NONE)
        return make_error_pack(e);

    return make_var_pack(r);
}

static package
bf_tocomplex(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    enum error e;

    r.type = TYPE_FLOAT;
    e = become_float(arglist[1], &r.v.fnum);

    free_var(arglist);

    if (e != E_NONE)
        return make_error_pack(e);

    return make_var_pack(Var::new_complex(complex_t(r.fnum(), 0.0)));
}

static package
bf_min(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    int i, nargs = arglist.length();
    int bad_types = 0;

    r = arglist[1];
    if (r.type == TYPE_INT) {   /* integers */
        for (i = 2; i <= nargs; i++)
            if (arglist[i].type != TYPE_INT)
                bad_types = 1;
            else if (arglist[i].v.num < r.v.num)
                r = arglist[i];
    } else {            /* floats */
        for (i = 2; i <= nargs; i++)
            if (arglist[i].type != TYPE_FLOAT)
                bad_types = 1;
            else if (arglist[i].v.fnum < r.v.fnum)
                r = arglist[i];
    }

    r = var_ref(r);
    free_var(arglist);
    if (bad_types)
        return make_error_pack(E_TYPE);
    else
        return make_var_pack(r);
}

static package
bf_max(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    int i, nargs = arglist.length();
    int bad_types = 0;

    r = arglist[1];
    if (r.type == TYPE_INT) {   /* integers */
        for (i = 2; i <= nargs; i++)
            if (arglist[i].type != TYPE_INT)
                bad_types = 1;
            else if (arglist[i].v.num > r.v.num)
                r = arglist[i];
    } else {            /* floats */
        for (i = 2; i <= nargs; i++)
            if (arglist[i].type != TYPE_FLOAT)
                bad_types = 1;
            else if (arglist[i].v.fnum > r.v.fnum)
                r = arglist[i];
    }

    r = var_ref(r);
    free_var(arglist);
    if (bad_types)
        return make_error_pack(E_TYPE);
    else
        return make_var_pack(r);
}

static package
bf_abs(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;

    r = var_dup(arglist[1]);
    if (r.type == TYPE_INT) {
        if (r.v.num < 0)
            r.v.num = -r.v.num;
    } else
        r.v.fnum = fabs(r.v.fnum);

    free_var(arglist);
    return make_var_pack(r);
}

namespace std {
    complex_t trunc(complex_t n) {
        return complex_t(trunc(n.real()), trunc(n.imag()));
    }

    complex_t round(complex_t n) {
        return complex_t(round(n.real()), round(n.imag()));
    }

    complex_t ceil(complex_t n) {
        return complex_t(ceil(n.real()), ceil(n.imag()));
    }

    complex_t floor(complex_t n) {
        return complex_t(floor(n.real()), floor(n.imag()));
    }
}

#define MATH_FUNC(name)                                          \
  static package                                                  \
  bf_ ## name(Var arglist, Byte next, void *vdata, Objid progr)   \
  {                                                               \
    Var v = arglist[1];                                           \
    errno = 0;                                                    \
    switch(v.type) {                                              \
    case TYPE_INT:                                                \
        v = Var::new_int(std::name(v.num()));                     \
        break;                                                    \
    case TYPE_FLOAT:                                              \
        v = Var::new_float(std::name(v.fnum()));                  \
        break;                                                    \
    case TYPE_COMPLEX:                                            \
        v = Var::new_complex(std::name(v.complex()));             \
        break;                                                    \
    default:                                                      \
        v = Var::new_err(E_INVARG);                               \
    }                                                             \
                                                                  \
    free_var(arglist);                                            \
                                                                  \
    if (errno == EDOM)                                            \
        return make_error_pack(E_INVARG);                         \
    else if (errno != 0)                                          \
        return make_error_pack(E_FLOAT);                          \
                                                                  \
    return make_var_pack(v);                                      \
}


static package
bf_sqrt(Var arglist, Byte next, void *vdata, Objid progr) {
    Var r, v = arglist[1];

    switch(v.type) {
    case TYPE_INT:
        r = (v.num() > 0) ? Var::new_int(std::sqrt(v.num())) : Var::new_complex(std::sqrt(v.complex()));
        break;
    case TYPE_FLOAT:
        r = (v.num() > 0) ? Var::new_float(std::sqrt(v.fnum())) : Var::new_complex(std::sqrt(v.complex()));
        break;
    case TYPE_COMPLEX:
        r = Var::new_complex(std::sqrt(v.complex()));
        break;
    default:
        r = Var::new_err(E_INVARG);
    }

    free_var(arglist);
    return (r.type != TYPE_ERR) ? make_var_pack(r) : make_error_pack(r.v.err);
}

static package 
bf_cbrt(Var arglist, Byte next, void *vdata, Objid progr) {
    Var v = arglist[1];

    switch(v.type) {
    case TYPE_INT:
        v = Var::new_int(std::cbrt(v.num()));
        break;
    case TYPE_FLOAT:
        v = Var::new_float(std::cbrt(v.fnum()));
        break;
    case TYPE_COMPLEX:
        v = Var::new_complex(std::pow(v.complex(), complex_t(1.0 / 3.0, 0.0)));
        break;
    default:
        v = Var::new_err(E_INVARG);
    }

    free_var(arglist);
    return (v.type != TYPE_ERR) ? make_var_pack(v) : make_error_pack(v.v.err);
}

#define M_LOG2 0.693147180559945

static package 
bf_log2(Var arglist, Byte next, void *vdata, Objid progr) {
    Var v = arglist[1];
    Var b = arglist[2];

    switch(v.type) {
    case TYPE_INT:
        v = Var::new_int(static_cast<Num>(std::log(v.fnum()) / M_LOG2));
        break;
    case TYPE_FLOAT:
        v = Var::new_float(std::log(v.fnum()) / M_LOG2);
        break;
    case TYPE_COMPLEX:
        v = Var::new_complex(std::log(v.complex()) / complex_t(M_LOG2, 0.0));
        break;
    default:
        {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
    }

    free_var(arglist);
    return make_var_pack(v);
}

static package 
bf_logn(Var arglist, Byte next, void *vdata, Objid progr) {
    Var v = arglist[1];
    Var b = arglist[2];

    switch(v.type) {
    case TYPE_INT:
        v = Var::new_int(static_cast<Num>(std::log(v.fnum()) / std::log(b.fnum())));
        break;
    case TYPE_FLOAT:
        v = Var::new_float(std::log(v.fnum()) / std::log(b.fnum()));
        break;
    case TYPE_COMPLEX:
        v = Var::new_complex(std::log(v.complex()) / std::log(b.complex()));
        break;
    default:
        {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
    }

    free_var(arglist);
    return make_var_pack(v);
}

MATH_FUNC(sin)
MATH_FUNC(cos)
MATH_FUNC(tan)
MATH_FUNC(asin)
MATH_FUNC(acos)
MATH_FUNC(sinh)
MATH_FUNC(cosh)
MATH_FUNC(tanh)
MATH_FUNC(acosh)
MATH_FUNC(asinh)
MATH_FUNC(atanh)
MATH_FUNC(exp)
MATH_FUNC(log)
MATH_FUNC(log10)
MATH_FUNC(ceil)
MATH_FUNC(floor)
MATH_FUNC(round)
MATH_FUNC(trunc)

static package
bf_atan(Var arglist, Byte next, void *vdata, Objid progr)
{
    auto nargs = arglist.length();

    Var r;

    errno = 0;
    if(nargs >= 2) {
        if(arglist[1].type == TYPE_COMPLEX) {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        } else if(arglist[1].type == TYPE_FLOAT || arglist[2].type == TYPE_FLOAT) {
            r = Var::new_float(std::atan2(arglist[1].fnum(), arglist[2].fnum()));
        } else {
            r = Var::new_int(static_cast<Num>(std::atan2(arglist[1].fnum(), arglist[2].fnum())));
        }
    } else {
        switch(arglist[1].type) {
        case TYPE_INT:
            r = Var::new_int(static_cast<Num>(std::atan(arglist[1].fnum())));
            break;
        case TYPE_FLOAT:
            r = Var::new_float(std::atan(arglist[1].fnum()));
            break;
        case TYPE_COMPLEX:
            r = Var::new_complex(std::atan(arglist[1].complex()));
            break;
        default:
            r = Var::new_err(E_INVARG);        
        }
    }

    free_var(arglist);
    if (errno == EDOM)
        return make_error_pack(E_INVARG);
    else if (errno != 0)
        return make_error_pack(E_FLOAT);
    else
        return make_var_pack(r);
}

static package bf_time_fmt(Var arglist, Byte next, void *vdata, Objid progr) {
  /* tm time structs have a max year equal to integer, 
     which a 64 bit number of seconds will surpass  */
  const long int year_seconds = 31536000;
  static const Num max_year   = std::numeric_limits<int>::max() * year_seconds;
  static const Num min_year   = -max_year;

  auto nargs      = arglist.length();
  const char *fmt = nargs >= 1 ? arglist[1].str() : "%c %Z";
  std::time_t ts  = nargs >= 2 ? std::clamp(arglist[2].num(), min_year, max_year) : time(nullptr);
  struct tm *t    = localtime(&ts);
  
  free_var(arglist);
  if (t == nullptr)
    return make_error_pack(E_INVARG);

  std::ostringstream ss;
  ss << std::put_time(t, fmt);  

  return make_var_pack(str_dup_to_var(ss.str().c_str()));
}

static package bf_time_parse(Var arglist, Byte next, void *vdata, Objid progr) {
  auto nargs      = arglist.length();
  const char *str = arglist[1].str();
  const char *fmt = nargs >= 2 ? arglist[2].str() : "%c %Z";
  int is_dst      = nargs >= 3 ? arglist[3].num() : -1; // default attempts to figure out DST

  std::tm t{.tm_isdst = is_dst};

  std::istringstream ss(str);
  ss >> std::get_time(&t, fmt);

  free_var(arglist);
  return make_var_pack(Var::new_int(mktime(&t)));
}

static package
bf_time(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    r.type = TYPE_INT;
    r.v.num = time(nullptr);
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_ctime(Var arglist, Byte next, void *vdata, Objid progr)
{
    /* tm time structs have a max year equal to integer, which a 64 bit number of seconds will surpass */
    const long int year_seconds = 31536000;
    const Num max_year = std::numeric_limits<int>::max() * year_seconds;

    Var r;
    time_t c;
    char buffer[128];
    struct tm *t;

    if (arglist.length() == 1) {
        /* Make sure the year doesn't overflow */
        c = std::min(arglist[1].v.num, max_year);
    } else {
        c = time(nullptr);
    }

    free_var(arglist);

    t = localtime(&c);
    if (t == nullptr)
        return make_error_pack(E_INVARG);

    {   /* Format the time, including a timezone name */
#if HAVE_STRFTIME
        if (strftime(buffer, sizeof buffer, "%a %b %d %H:%M:%S %Y %Z", t) == 0)
            return make_error_pack(E_INVARG);
#else
#  if HAVE_TM_ZONE
        struct tm *t = localtime(&c);
        char *tzname = t->tm_zone;
#  else
#    if !HAVE_TZNAME
        const char *tzname = "XXX";
#    endif
#  endif

        strcpy(buffer, ctime(&c));
        buffer[24] = ' ';
        strncpy(buffer + 25, tzname, 3);
        buffer[28] = '\0';
#endif
    }

    if (buffer[8] == '0')
        buffer[8] = ' ';
    r.type = TYPE_STR;
    r.v.str = str_dup(buffer);

    return make_var_pack(r);
}

#ifdef __FreeBSD__
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

enum ftime_mode {
    NANO   = 4,
    MICRO  = 5,
    MILLI  = 6,
    SECOND = 7,
    MIN    = 8,
    HOUR   = 9
};

// Monotonic Clock
template<class T>
static inline Var hr_time() {
    using clock = std::chrono::steady_clock;
    T ff = clock::now().time_since_epoch();
    return Var::new_float(ff.count());
}

/* Returns a float representing seconds and nanoseconds since the Epoch.
   Optional arguments specify monotonic time; 1: Monotonic. 2. Monotonic raw.
   (seconds since an arbitrary period of time. More useful for timing
   since its not affected by NTP or other time changes.) */
static package
bf_ftime(Var arglist, Byte next, void *vdata, Objid progr)
{
    using NS = std::chrono::duration<double, std::nano>;
    using US = std::chrono::duration<double, std::micro>;
    using MS = std::chrono::duration<double, std::milli>;
    using SS = std::chrono::duration<double, std::ratio<1>>;
    using MM = std::chrono::duration<double, std::ratio<60>>;
    using HH = std::chrono::duration<double, std::ratio<3600>>;

    auto nargs = arglist.length();
    if(nargs >= 1 && arglist[1].num() >= 4) {

        Var tt;
        enum ftime_mode fmode = static_cast<enum ftime_mode>(arglist[1].num());

        switch(fmode) {
        case NANO:
            tt = hr_time<NS>();
            break;
        case MICRO:
            tt = hr_time<US>();
            break;
        case MILLI:
            tt = hr_time<MS>();
            break;
        case SECOND:
            tt = hr_time<SS>();
            break;
        case MIN:
            tt = hr_time<MM>();
            break;
        case HOUR:
            tt = hr_time<HH>();
            break;
        }

        free_var(arglist);
        return make_var_pack(tt);
    }

#ifdef __MACH__
    // macOS only provides SYSTEM_CLOCK for monotonic time, so our arguments don't matter.
    clock_id_t clock_type = (arglist.length() == 0 ? CALENDAR_CLOCK : SYSTEM_CLOCK);
#else
    // Other OSes provide MONOTONIC_RAW and MONOTONIC, so we'll check args for 2(raw) or 1.
    clockid_t clock_type = 0;
    if (arglist.length() == 0)
        clock_type = CLOCK_REALTIME;
    else
        clock_type = arglist[1].v.num == 2 ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC;
#endif

    struct timespec ts;

#ifdef __MACH__
    // macOS lacks clock_gettime, use clock_get_time instead
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), clock_type, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    ts.tv_sec = mts.tv_sec;
    ts.tv_nsec = mts.tv_nsec;
#else
    clock_gettime(clock_type, &ts);
#endif

    free_var(arglist);
    return make_var_pack(Var::new_float((double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0));
}

static package
bf_random(Var arglist, Byte next, void *vdata, Objid progr)
{
    int nargs = arglist.length();

    Num minnum = (nargs == 2 ? arglist[1].v.num : 1);
    Num maxnum = (nargs >= 1 ? arglist[nargs].v.num : INTNUM_MAX);

    free_var(arglist);

    if (maxnum < minnum || minnum > maxnum)
        return make_error_pack(E_INVARG);

    std::uniform_int_distribution<Num> distribution(minnum, maxnum);
    Var r = Var::new_int(distribution(rng));
    return make_var_pack(r);
}

static package
bf_reseed_random(Var arglist, Byte next, void *vdata, Objid progr)
{
    free_var(arglist);

    if (!is_wizard(progr))
        return make_error_pack(E_PERM);

    reseed_rng();
    return no_var_pack();
}

/* Return a random floating point value between 0.0..args[1] or args[1]..args[2] */
static package
bf_frandom(Var arglist, Byte next, void *vdata, Objid progr)
{
    double fmin = (arglist.length() > 1 ? arglist[1].v.fnum : 0.0);
    double fmax = (arglist.length() > 1 ? arglist[2].v.fnum : arglist[1].v.fnum);

    free_var(arglist);

    double f = (double)rand() / RAND_MAX;
    f = fmin + f * (fmax - fmin);

    Var ret;
    ret.type = TYPE_FLOAT;
    ret.v.fnum = f;

    return make_var_pack(ret);
}

static package 
bf_rand_splitmix64(Var arglist, Byte next, void *vdata, Objid progr) {
    auto nargs = arglist.length();
    auto min   = nargs >= 1 ? arglist[1].num() : 1;
    auto max   = nargs >= 2 ? arglist[2].num() : INTNUM_MAX;
    auto count = nargs >= 3 ? arglist[3].num() : 1;

    std::uniform_int_distribution<Num> distrib(min, max);

    Var r;
    if(nargs >= 4) {
        splitmix64 rng = splitmix64(arglist[4].unum());
        if(count <= 1) {
            r = Var::new_int(distrib(rng));
        } else {
            r = new_list(count);
            for(auto i=1; i<=r.length(); i++)
                r[i] = Var::new_int(distrib(rng));
        }
    } else {
        if(count <= 1) {
            r = Var::new_int(distrib(s64));
        } else {
            r = new_list(count);
            for(auto i=1; i<=r.length(); i++)
                r[i] = Var::new_int(distrib(s64));
        }
    }

    free_var(arglist);
    return make_var_pack(r);
}

#define TRY_STREAM enable_stream_exceptions()
#define ENDTRY_STREAM disable_stream_exceptions()

static package
make_space_pack()
{
    if (server_flag_option_cached(SVO_MAX_CONCAT_CATCHABLE))
        return make_error_pack(E_QUOTA);
    else
        return make_abort_pack(ABORT_SECONDS);
}

static package
bf_random_bytes(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (count) */
    Var r;
    package p;

    int len = arglist[1].v.num;

    if (len < 0 || len > 10000) {
        p = make_raise_pack(E_INVARG, "Invalid count", var_ref(arglist[1]));
        free_var(arglist);
        return p;
    }

    unsigned char out[len];

    sosemanuk_prng(&run_context, out, len);

    Stream *s = new_stream(32 * 3);

    TRY_STREAM;
    try {
        stream_add_raw_bytes_to_binary(s, (char *)out, len);

        r.type = TYPE_STR;
        r.v.str = str_dup(stream_contents(s));
        p = make_var_pack(r);
    }
    catch (stream_too_big& exception) {
        p = make_space_pack();
    }
    ENDTRY_STREAM;

    free_stream(s);
    free_var(arglist);

    return p;
}

#undef TRY_STREAM
#undef ENDTRY_STREAM

static package
bf_floatstr(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (float, precision [, sci-notation]) */
    double d = arglist[1].v.fnum;
    int prec = arglist[2].v.num;
    int use_sci = (arglist.length() >= 3
                   && is_true(arglist[3]));
    char fmt[10], output[500];  /* enough for IEEE double */
    Var r;

    free_var(arglist);
    if (prec > __DECIMAL_DIG__)
        prec = __DECIMAL_DIG__;
    else if (prec < 0)
        return make_error_pack(E_INVARG);
    sprintf(fmt, "%%.%d%c", prec, use_sci ? 'e' : 'f');
    sprintf(output, fmt, d);

    r.type = TYPE_STR;
    r.v.str = str_dup(output);

    return make_var_pack(r);
}

/* Calculates the distance between two n-dimensional sets of coordinates. */
static package
bf_distance(Var arglist, Byte next, void *vdata, Objid progr)
{
    double ret = 0.0, tmp = 0.0;
    int count;

    const Num list_length = arglist[1].length();
    for (count = 1; count <= list_length; count++)
    {
        if ((arglist[1][count].type != TYPE_INT && arglist[1][count].type != TYPE_FLOAT) || (arglist[2][count].type != TYPE_INT && arglist[2][count].type != TYPE_FLOAT))
        {
            free_var(arglist);
            return make_error_pack(E_TYPE);
        }
        else
        {
            tmp = (arglist[2][count].type == TYPE_INT ? (double)arglist[2][count].v.num : arglist[2][count].v.fnum) - (arglist[1][count].type == TYPE_INT ? (double)arglist[1][count].v.num : arglist[1][count].v.fnum);
            ret = ret + (tmp * tmp);
        }
    }

    free_var(arglist);

    return make_var_pack(Var::new_float(sqrt(ret)));
}

/* Calculates the bearing between two sets of three dimensional floating point coordinates. */
static package
bf_relative_heading(Var arglist, Byte next, void *vdata, Objid progr)
{
    if (arglist[1][1].type != TYPE_FLOAT || arglist[1][2].type != TYPE_FLOAT || arglist[1][3].type != TYPE_FLOAT || arglist[2][1].type != TYPE_FLOAT || arglist[2][2].type != TYPE_FLOAT || arglist[2][3].type != TYPE_FLOAT) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }

    double dx = arglist[2][1].fnum() - arglist[1][1].fnum();
    double dy = arglist[2][2].fnum() - arglist[1][2].fnum();
    double dz = arglist[2][3].fnum() - arglist[1][3].fnum();

    double xy = 0.0;
    double z = 0.0;

    xy = atan2(dy, dx) * 57.2957795130823;

    if (xy < 0.0)
        xy = xy + 360.0;

    z = atan2(dz, sqrt((dx * dx) + (dy * dy))) * 57.2957795130823;

    Var s = new_list(2);
    s[1] = Var::new_int(xy);
    s[2] = Var::new_int(z);

    free_var(arglist);

    return make_var_pack(s);
}

Var zero;           /* useful constant */

void
register_numbers(void)
{
    zero.type = TYPE_INT;
    zero.v.num = 0;

    reseed_rng();

    register_function("toint",           1,  1, bf_toint, TYPE_ANY);
    register_function("tofloat",         1,  1, bf_tofloat, TYPE_ANY);
    register_function("tocomplex",       1,  1, bf_tocomplex, TYPE_ANY);
    register_function("min",             1, -1, bf_min, TYPE_NUMERIC);
    register_function("max",             1, -1, bf_max, TYPE_NUMERIC);
    register_function("abs",             1,  1, bf_abs, TYPE_NUMERIC);
    register_function("random",          0,  2, bf_random, TYPE_INT, TYPE_INT);
    register_function("reseed_random",   0,  0, bf_reseed_random);
    register_function("frandom",         1,  2, bf_frandom, TYPE_FLOAT, TYPE_FLOAT);
    register_function("rand_splitmix64", 0,  4, bf_rand_splitmix64, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT);
    register_function("round",           1,  1, bf_round, TYPE_NUMERIC);
    register_function("random_bytes",    1,  1, bf_random_bytes, TYPE_INT);
    register_function("time",            0,  0, bf_time);
    register_function("ctime",           0,  1, bf_ctime, TYPE_INT);
    register_function("ftime",           0,  1, bf_ftime, TYPE_INT);
    register_function("time_fmt",        0,  2, bf_time_fmt, TYPE_STR, TYPE_INT);
    register_function("time_parse",      1,  3, bf_time_parse, TYPE_STR, TYPE_STR, TYPE_INT);
    register_function("floatstr",        2,  3, bf_floatstr, TYPE_FLOAT, TYPE_INT, TYPE_ANY);

    register_function("sqrt",  1, 1, bf_sqrt,  TYPE_NUMERIC);
    register_function("cbrt",  1, 1, bf_cbrt,  TYPE_NUMERIC);
    register_function("sin",   1, 1, bf_sin,   TYPE_NUMERIC);
    register_function("cos",   1, 1, bf_cos,   TYPE_NUMERIC);
    register_function("tan",   1, 1, bf_tan,   TYPE_NUMERIC);
    register_function("asin",  1, 1, bf_asin,  TYPE_NUMERIC);
    register_function("acos",  1, 1, bf_acos,  TYPE_NUMERIC);
    register_function("atan",  1, 2, bf_atan,  TYPE_NUMERIC, TYPE_INT | TYPE_FLOAT);
    register_function("sinh",  1, 1, bf_sinh,  TYPE_NUMERIC);
    register_function("cosh",  1, 1, bf_cosh,  TYPE_NUMERIC);
    register_function("tanh",  1, 1, bf_tanh,  TYPE_NUMERIC);
    register_function("acosh", 1, 1, bf_acosh, TYPE_NUMERIC);
    register_function("atanh", 1, 1, bf_atanh, TYPE_NUMERIC);
    register_function("asinh", 1, 1, bf_asinh, TYPE_NUMERIC);
    register_function("exp",   1, 1, bf_exp,   TYPE_NUMERIC);
    register_function("log",   1, 1, bf_log,   TYPE_NUMERIC);
    register_function("log2",  1, 1, bf_log2,  TYPE_NUMERIC);
    register_function("log10", 1, 1, bf_log10, TYPE_NUMERIC);
    register_function("logn",  2, 2, bf_logn,  TYPE_NUMERIC, TYPE_NUMERIC);
    register_function("ceil",  1, 1, bf_ceil,  TYPE_NUMERIC);
    register_function("floor", 1, 1, bf_floor, TYPE_NUMERIC);
    register_function("trunc", 1, 1, bf_trunc, TYPE_NUMERIC);

    /* Possibly misplaced functions... */
    register_function("distance",         2, 2, bf_distance,         TYPE_LIST, TYPE_LIST);
    register_function("relative_heading", 2, 2, bf_relative_heading, TYPE_LIST, TYPE_LIST);
}
