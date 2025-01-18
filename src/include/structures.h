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

#ifndef Structures_h
#define Structures_h 1

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <cmath>
#include <limits>
#include <vector>
#include <complex>

#include "config.h"
#include "storage.h"

#ifdef ONLY_32_BITS
#define MAXINT	((Num) 2147483647L)
#define MININT	((Num) -2147483648L)
#else
#define MAXINT	((Num) 9223372036854775807LL)
#define MININT	((Num) -9223372036854775807LL)
#endif
#define MAXOBJ	((Objid) MAXINT)
#define MAXOBJ	((Objid) MAXINT)
#define MINOBJ	((Objid) MININT)

#ifdef ONLY_32_BITS
typedef int32_t Num;
typedef uint32_t UNum;
#define PRIdN	PRId32
#define SCNdN	SCNd32
#define INTNUM_MAX INT32_MAX
#define SERVER_BITS 32
#else
typedef int64_t Num;
typedef uint64_t UNum;
#define PRIdN	PRId64
#define SCNdN	SCNd64
#define INTNUM_MAX INT64_MAX
#define SERVER_BITS 64
#endif
typedef Num Objid;
#define EPSILON   std::numeric_limits<double>::epsilon()
#define EPSILON32 std::numeric_limits<float>::epsilon()
typedef std::complex<float> complex_t;
typedef std::reference_wrapper<complex_t> complex_ref;

/*
 * Special Objids
 */
#define SYSTEM_OBJECT  0
#define NOTHING       -1
#define AMBIGUOUS     -2
#define FAILED_MATCH  -3

/* Do not reorder or otherwise modify this list, except to add new elements at
 * the end, since the order here defines the numeric equivalents of the error
 * values, and those equivalents are both DB-accessible knowledge and stored in
 * raw form in the DB.
 */
enum error {
    E_NONE, E_TYPE, E_DIV, E_PERM, E_PROPNF, E_VERBNF, E_VARNF, E_INVIND,
    E_RECMOVE, E_MAXREC, E_RANGE, E_ARGS, E_NACC, E_INVARG, E_QUOTA, E_FLOAT,
    E_FILE, E_EXEC, E_INTRPT
};

/* Types which have external data should be marked with the
 * TYPE_COMPLEX_FLAG so that `free_var()'/`var_ref()'/`var_dup()' can
 * recognize them easily.  This flag is only set in memory.  The
 * original _TYPE_XYZ values are used in the database file and
 * returned to verbs calling typeof().  This allows the inlines to be
 * extremely cheap (both in space and time) for simple types like oids
 * and ints.
 */

#define TYPE_COMPLEX_FLAG 0x8000000
#define TYPE_DB_MASK      0x7ffffff

/* Do not reorder or otherwise modify this list, except to add new
 * elements at the end (see THE END), since the order here defines the
 * numeric equivalents of the type values, and those equivalents are
 * both DB-accessible knowledge and stored in raw form in the DB.  For
 * new complex types add both a _TYPE_XYZ definition and a TYPE_XYZ
 * definition. Don't forget to add an English name for new types to unparse.cc.
 * 
 * Some E_TYPE errors will specifically mention all types are acceptable
 * except a select few. When you add a new type, if applicable, add it to the
 * PUSH_TYPE_MISMATCH call in the following places (terrible, I know):
 * execute.cc: OP_MAP_INSERT, OP_PUSH_REF, OP_RANGE_REF, EOP_RANGESET
 */

typedef enum : uint32_t {
    _TYPE_TYPE    = 0,       /* meta type for holding var_type */
    TYPE_INT      = 1 << 0,  /* user-visible */
    TYPE_OBJ      = 1 << 1,
    _TYPE_STR     = 1 << 2,
    TYPE_ERR      = 1 << 3,
    _TYPE_LIST    = 1 << 4,
    TYPE_CLEAR    = 1 << 5,  /* in clear properties' value slot */
    TYPE_NONE     = 1 << 6,  /* in uninitialized MOO variables */
    TYPE_CATCH    = 1 << 7,  /* on-stack marker for an exception handler */
    TYPE_FINALLY  = 1 << 8,  /* on-stack marker for a TRY-FINALLY clause */
    TYPE_FLOAT    = 1 << 9,  /* floating-point number; user-visible */
    _TYPE_MAP     = 1 << 10, /* map; user-visible */
    _TYPE_CALL    = 1 << 11, /* verb call handle; user-visible */
    _TYPE_ANON    = 1 << 12, /* anonymous object; user-visible */
    _TYPE_WAIF    = 1 << 13, /* lightweight object; user-visible */
    TYPE_BOOL     = 1 << 14, /* boolean type; user-visible */
    TYPE_COMPLEX  = 1 << 15, /* complex number; user-visible */
    _TYPE_MATRIX  = 1 << 16, /* matrix type; user-visible */

    /* THE END - complex aliases come next */
    TYPE_STR     = (_TYPE_STR     | TYPE_COMPLEX_FLAG),
    TYPE_LIST    = (_TYPE_LIST    | TYPE_COMPLEX_FLAG),
    TYPE_MAP     = (_TYPE_MAP     | TYPE_COMPLEX_FLAG),
    TYPE_ANON    = (_TYPE_ANON    | TYPE_COMPLEX_FLAG),
    TYPE_WAIF    = (_TYPE_WAIF    | TYPE_COMPLEX_FLAG),
    TYPE_CALL    = (_TYPE_CALL    | TYPE_COMPLEX_FLAG),
    TYPE_MATRIX  = (_TYPE_MATRIX  | TYPE_COMPLEX_FLAG),
} var_type;

#define TYPE_ANY     0xffffffff                             /* wildcard for use in declaring built-ins */
#define TYPE_NUMERIC (TYPE_INT | TYPE_FLOAT | TYPE_COMPLEX) /* wildcard for (integer, float, or complex) */

typedef struct Var Var;

/* see map.cc */
typedef struct hashmap hashmap;

/* defined in db_private.h */
typedef struct Object Object;

struct WaifPropdefs;

/* Try to make struct Waif fit into 32 bytes with this mapsz.  These bytes
 * are probably "free" (from a powers-of-two allocator) and we can use them
 * to save lots of space.  With 64bit addresses I think the right value is 8.
 * If checkpoints are unforked, save space for an index used while saving.
 * Otherwise we can alias propdefs and clobber it in the child.
 */
#ifdef UNFORKED_CHECKPOINTS
#define WAIF_MAPSZ	2
#else
#define WAIF_MAPSZ	3
#endif

    Var str_dup_to_var(const char *s);
    Var str_ref_to_var(const char *s);


typedef struct Waif {
    Objid               _class;
    Objid               owner;
    struct WaifPropdefs *propdefs;
    Var			            *propvals;
    unsigned long		    map[WAIF_MAPSZ];
#ifdef UNFORKED_CHECKPOINTS
    unsigned long		    waif_save_index;
#else
#define waif_save_index	map[0]
#endif
} Waif;

typedef struct verb_handle verb_handle;
typedef struct db_verb_handle db_verb_handle;

Var get_call(Var who, Var verb);

struct Var {
  union {
    Num num;              /* NUM, CATCH, FINALLY */
    const char *str;      /* CSTR    */
    char *str_;           /* STR     */
    UNum unum;            /* UNUM    */
    Objid obj;            /* OBJ     */
    enum error err;       /* ERR     */
    Var *list;            /* LIST    */
    hashmap *map;         /* MAP     */
    double fnum;          /* FLOAT   */
    Object *anon;         /* ANON    */
    Waif *waif;           /* WAIF    */
    bool truth;           /* BOOL    */
    db_verb_handle *call; /* CALL    */
    complex_t complex;    /* COMPLEX */
    void *mat;            /* MATRIX  */
  } v;

  var_type type;

  // Default constructor
  Var();

  // Copy constructors
  Var(const Var& other);
  Var(const std::vector<Var>& other);

  // Bracket operators
  Var& operator[](int i);
  Var& operator[](Var k);
  Var& operator[](const char* key);

  // Copy assignment
  Var& operator=(const Var& other);
  Var& operator=(Var& other);

  // Move assignment
  Var& operator=(const Var&& other);
  Var& operator=(Var&& other);

  inline bool is_type() {
    return _TYPE_TYPE == type;
  }

  inline bool is_pointer() {
   return TYPE_COMPLEX_FLAG & type;
  }

  inline bool is_none() {
      return TYPE_NONE == type;
  }

  inline bool is_collection() {
      return TYPE_LIST == type || TYPE_MAP == type;
  }

  inline bool is_object() {
      return TYPE_OBJ == type || TYPE_ANON == type || TYPE_WAIF == type;
  }

  inline bool is_int() {
      return TYPE_INT == type || is_type();
  }

  inline bool is_str() const {
    return TYPE_STR == type;
  }

  inline bool is_obj() const {
    return TYPE_OBJ == type;
  }

  inline bool is_float() const {
    return TYPE_FLOAT == type;
  }

  inline bool is_complex() const {
    return TYPE_COMPLEX == type;
  }

  inline bool is_num() const {
    return is_int() || is_float() || is_complex() || type == _TYPE_TYPE;
  }
  
  inline bool is_int() const {
    return TYPE_INT == type || type == _TYPE_TYPE;
  }

  inline bool is_err() const {
    return TYPE_ERR == type;
  }

  inline const char* str() const {
    return is_str() ? v.str : nullptr;
  }

  inline const char* str(const char* s) {
    type = TYPE_STR;
    return v.str = s;
  }

  inline Objid obj() const {
    return TYPE_OBJ == type ? v.obj : -1;
  }

  inline Num num() const {
    if(is_num())
      if(type & TYPE_COMPLEX) return static_cast<Num>(v.complex.real());
      return is_int() ? v.num : static_cast<Num>(v.fnum);
    return 0;
  }

  inline UNum unum() const {
    if(is_num())
      if(type & TYPE_COMPLEX) return static_cast<UNum>(v.complex.real());
      return is_int() ? v.unum : static_cast<UNum>(v.fnum);
    return 0;
  }

  inline double fnum() const {
    if(is_num())
      if(type & TYPE_COMPLEX) return static_cast<double>(v.complex.real());
      return is_float() ? v.fnum : static_cast<double>(v.num);
    return 0.0;
  }

  inline complex_t complex() const {
    if(is_num())
      return is_complex() ? v.complex : complex_t(fnum(), 0.0);
    return complex_t(0.0, 0.0);
  }

  inline enum error err() const {
    return TYPE_ERR == type ? v.err : E_TYPE;
  }

  inline bool truth() const {
    return v.truth;
  }

  static inline Var new_int(const Num num) {
    Var v;
    v.type = TYPE_INT;
    v.v.num = num;
    return v;
  }

  static inline Var new_obj(const Objid obj) {
    Var v;
    v.type = TYPE_OBJ;
    v.v.obj = obj;
    return v;
  }

  static inline Var new_waif(Waif *waif) {
    Var v;
    v.type = TYPE_WAIF;
    v.v.waif = waif;
    return v;
  }

  static inline Var new_bool(bool truth) {
    Var v;
    v.type = TYPE_BOOL;
    v.v.truth = truth;
    return v;
  }

  static inline Var new_float(const double d) {
    Var v;
    v.type = TYPE_FLOAT;
    v.v.fnum = d;
    return v;
  }

  static inline Var new_complex(const double r, const double i) {
    Var v;
    v.type = TYPE_COMPLEX;
    v.v.complex = complex_t(static_cast<float>(r), static_cast<float>(i));
    return v;
  }

  static inline Var new_complex(complex_t c) {
    Var v;
    v.type = TYPE_COMPLEX;
    v.v.complex = c;
    return v;
  }

  static inline Var new_str(size_t len) {
    Var v;
    v.type = TYPE_STR;
    v.str((const char*)mymalloc(len, M_STRING));
    return v;
  }

  static inline Var new_str(const char* s, bool copy = false) {
    return copy ? str_dup_to_var(s) : str_ref_to_var(s);
  }


/*
  static inline Var str_concat(Var a, Var b)
  {
    a.v.str = (const char*)myrealloc((void*)a.v.str, memo_strlen(a.v.str) + memo_strlen(b.v.str), M_STRING);
    a.v.str = strcat((char*)a.v.str, b.v.str);
    free_str(b.v.str);
    *b.v.str_ = *a.v.str_;
    return a;
  }
*/

  static inline Var new_err(enum error e) {
    Var v;
    v.type = TYPE_ERR;
    v.v.err = e;
    return v;
  }

  static inline Var new_call(Var dude, Var verb) {
    Var c = get_call(dude, verb);
    return c;
  }
  
  static inline Var new_list(size_t sz);
  static inline Var new_map(size_t sz);

  inline hashmap* map() const;
  inline Var& map(Var k) const;
  inline Var* list() const;
  inline Var* list(Num i) const;

  // Helper functions
  inline Var& __index(Num i);
  inline bool contains(Var key) const;
  Num length() const;

  std::size_t hash() const;

  Var& operator+=(const Var& rhs);
  Var& operator+(const Var& rhs);

  operator std::vector<Var>() const;
  operator std::string() const;
  operator int() const;
};

template <>
struct std::hash<Var> {
  std::size_t operator()(const Var& v) const {
    return v.hash();
  }
};

struct db_verb_handle {
  verb_handle *ptr;
  Objid oid;
  Var verbname;

  verb_handle *handle();
};

inline Var
str_dup_to_var(const char *s)
{
    Var r;
    r.type = TYPE_STR;
    r.v.str = str_dup(s);
    return r;
}

inline Var
str_ref_to_var(const char *s)
{
    Var r;
    r.type = TYPE_STR;
    r.v.str = str_ref(s);
    return r;
}

/* generic tuples */
typedef struct var_pair {
    Var a;
    Var b;
} var_pair;

extern Var zero;    /* see numbers.c */
extern Var nothing; /* see objects.c */
extern Var clear;   /* see objects.c */
extern Var none;    /* see objects.c */

/* Hard limits on string, list and map sizes are imposed mainly to
 * keep malloc calculations from rolling over, and thus preventing the
 * ensuing buffer overruns.  Sizes allow extra space for reference
 * counts and cached length values.  Actual limits imposed on
 * user-constructed maps, lists and strings should generally be
 * smaller (see options.h)
 */
#define MAX_STRING                  (INT32_MAX - MIN_STRING_CONCAT_LIMIT)
#define MAX_MAP_VALUE_BYTES_LIMIT   (INT32_MAX - MIN_MAP_VALUE_BYTES_LIMIT)
#define MAX_LIST_VALUE_BYTES_LIMIT  (INT32_MAX - MIN_LIST_VALUE_BYTES_LIMIT)

#endif				/* !Structures_h */
