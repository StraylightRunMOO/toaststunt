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
#define EPSILON std::numeric_limits<double>::epsilon()

/*
 * Special Objid's
 */
#define SYSTEM_OBJECT	0
#define NOTHING		-1
#define AMBIGUOUS	-2
#define FAILED_MATCH	-3

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
#define TYPE_COMPLEX_FLAG	0x80
#define TYPE_DB_MASK		0x7f

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
typedef enum : uint8_t {
    TYPE_INT, TYPE_OBJ, _TYPE_STR, TYPE_ERR, _TYPE_LIST, /* user-visible */
    TYPE_CLEAR,			/* in clear properties' value slot */
    TYPE_NONE,			/* in uninitialized MOO variables */
    TYPE_CATCH,			/* on-stack marker for an exception handler */
    TYPE_FINALLY,		/* on-stack marker for a TRY-FINALLY clause */
    _TYPE_FLOAT,		/* floating-point number; user-visible */
    _TYPE_MAP,			/* map; user-visible */
    _TYPE_OBSOLETE,		/* formerly iterator - we may be able to re-use this */
    _TYPE_ANON,			/* anonymous object; user-visible */
    _TYPE_WAIF,         /* lightweight object; user-visible */
    TYPE_BOOL,			/* Experimental boolean type */
    /* THE END - complex aliases come next */
    TYPE_STR = (_TYPE_STR | TYPE_COMPLEX_FLAG),
    TYPE_FLOAT = (_TYPE_FLOAT),
    TYPE_LIST = (_TYPE_LIST | TYPE_COMPLEX_FLAG),
    TYPE_MAP = (_TYPE_MAP | TYPE_COMPLEX_FLAG),
    TYPE_ANON = (_TYPE_ANON | TYPE_COMPLEX_FLAG),
    TYPE_WAIF = (_TYPE_WAIF | TYPE_COMPLEX_FLAG),
} var_type;

static inline std::string get_type_name(var_type t) {
  switch(t) {
  case TYPE_INT:
    return "INT";
  case TYPE_OBJ:
    return "OBJ";
  case _TYPE_STR:
    return "_STR";
  case TYPE_ERR:
    return "ERR";
  case _TYPE_LIST:
    return "_LIST";
  case TYPE_CLEAR:
    return "CLEAR";
  case TYPE_NONE:
    return "NONE";
  case TYPE_CATCH:
    return "CATCH";
  case TYPE_FINALLY:
    return "FINALLY";
  case _TYPE_MAP:
    return "_MAP";
  case _TYPE_OBSOLETE:
    return "_OBSOLETE";
  case _TYPE_ANON:
    return "_ANON";
  case _TYPE_WAIF:
    return "_WAIF";
  case TYPE_BOOL:
    return "BOOL";
  case TYPE_STR:
    return "STR";
  case TYPE_FLOAT:
    return "FLOAT";
  case TYPE_LIST:
    return "LIST";
  case TYPE_MAP:
    return "MAP";
  case TYPE_ANON:
    return "ANON";
  case TYPE_WAIF:
    return "WAIF";
  default:
    break;
  }

  return ">>UNKNOWN TYPE " + std::to_string(t) + "<<";
}

#define TYPE_ANY ((var_type) -1)	/* wildcard for use in declaring built-ins */
#define TYPE_NUMERIC ((var_type) -2)	/* wildcard for (integer or float) */

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

struct Var {
  union {
  	const char *str; /* STR */
  	Num num;		     /* NUM, CATCH, FINALLY */
    UNum unum;       /* UNUM */
  	Objid obj;		   /* OBJ */
  	enum error err;  /* ERR */
  	Var *list;		   /* LIST */
  	hashmap *map;		 /* MAP */
  	double fnum;		 /* FLOAT */
  	Object *anon;		 /* ANON */
      Waif *waif;    /* WAIF */
  	bool truth	;		 /* BOOL */
  } v;

  var_type type;

  // Default constructor
  Var();

  // Copy constructors
  Var(const Var& other);
  Var(const std::vector<Var>& other);

  // Bracket operators
  Var& operator[](Num i);
  Var& operator[](Var k);
  Var& operator[](const char* key);

  // Copy assignment
  Var& operator=(const Var& other);
  Var& operator=(Var& other);

  // Move assignment
  Var& operator=(const Var&& other);
  Var& operator=(Var&& other);

  inline bool is_complex() {
   return TYPE_COMPLEX_FLAG & type;
  }

  inline bool is_none() {
      return TYPE_NONE == type;
  }

  inline bool is_collection() {
      return TYPE_LIST == type || TYPE_MAP == type || TYPE_ANON == type;
  }

  inline bool is_object() {
      return TYPE_OBJ == type || TYPE_ANON == type || TYPE_WAIF == type;
  }

  inline bool is_int() {
      return TYPE_INT == type;
  }

  inline bool is_str() const {
    return TYPE_STR == type;
  }

  inline bool is_obj() const {
    return TYPE_OBJ == type;
  }

  inline bool is_num() const {
    return TYPE_INT == type || TYPE_FLOAT == type;
  }

  inline bool is_int() const {
    return TYPE_INT == type;
  }

  inline bool is_float() const {
    return TYPE_FLOAT == type;
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
      return TYPE_INT == type ? v.num : static_cast<Num>(fnum());
    return 0;
  }

  inline UNum unum() const {
    if(is_num())
      return TYPE_INT == type ? v.unum : static_cast<UNum>(fnum());
    return 0;
  }

  inline double fnum() const {
    if(is_num())
      return TYPE_FLOAT == type ? v.fnum : static_cast<double>(num());
    return 0.0;
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

  static inline Var new_obj(const Objid &obj) {
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

  static inline Var new_bool(int value) {
    Var v;
    v.type = TYPE_BOOL;
    v.v.truth = value ? true : false;
    return v;
  }

  static inline Var new_float(const double& d) {
    Var v;
    v.type = TYPE_FLOAT;
    v.v.fnum = d;
    return v;
  }

  static inline Var new_str(size_t len) {
    Var v;
    v.type = TYPE_STR;
    v.str((const char*)mymalloc(len, M_STRING));
    return v;
  }

  static Var new_string(const char* string) noexcept
  {
    Var v;
    v.type = TYPE_STR;
    v.v.str = str_dup(string);
    return v;
  }

  static inline Var new_err(enum error e) {
    Var v;
    v.type = TYPE_ERR;
    v.v.err = e;
    return v;
  }

  static inline Var new_str(const char* s) {
    return str_ref_to_var(s);
  }

  static inline Var new_str(const char* s, bool copy) {
    return copy ? str_dup_to_var(s) : str_ref_to_var(s);
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

  operator std::vector<Var>() const;
  operator std::string() const;
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

extern Var zero;		/* see numbers.c */
extern Var nothing;		/* see objects.c */
extern Var clear;		/* see objects.c */
extern Var none;		/* see objects.c */

/* Hard limits on string, list and map sizes are imposed mainly to
 * keep malloc calculations from rolling over, and thus preventing the
 * ensuing buffer overruns.  Sizes allow extra space for reference
 * counts and cached length values.  Actual limits imposed on
 * user-constructed maps, lists and strings should generally be
 * smaller (see options.h)
 */
#define MAX_STRING	(INT32_MAX - MIN_STRING_CONCAT_LIMIT)
#define MAX_LIST_VALUE_BYTES_LIMIT	(INT32_MAX - MIN_LIST_VALUE_BYTES_LIMIT)
#define MAX_MAP_VALUE_BYTES_LIMIT	(INT32_MAX - MIN_MAP_VALUE_BYTES_LIMIT)

#endif				/* !Structures_h */
