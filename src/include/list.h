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

#ifndef EXT_LIST_H
#define EXT_LIST_H 1

#include <functional>
#include <random>

#include "structures.h"
#include "streams.h"

extern Var new_list(int size);
extern bool destroy_list(Var list);
extern Var list_dup(Var list);

extern Var listappend(Var list, Var value);
extern Var listinsert(Var list, Var value, int pos);
extern Var listdelete(Var list, int pos);
extern Var listset(Var list, Var value, int pos);
extern Var listrangeset(Var list, int from, int to, Var value);
extern Var listconcat(Var first, Var second);
extern Var setadd(Var list, Var value);
extern Var setremove(Var list, Var value);
extern Var sublist(Var list, int lower, int upper);
extern int listequal(Var lhs, Var rhs, int case_matters);
extern int list_sizeof(Var *list);

typedef std::function<int(Var, int)> list_callback;
extern int listforeach(Var list, list_callback func, bool reverse=false);

extern Var set_intersection(Var s1, Var s2, bool count_dup);

extern Var strrangeset(Var list, int from, int to, Var value);
extern Var substr(Var str, int lower, int upper);
extern Var strget(Var str, int i);

extern Var corified_as(Var, int);

extern const char *value2str(Var);
extern void unparse_value(Stream *, Var);
extern std::string toliteral(Var);

extern const char* str_escape(const char*, int);
extern const char* str_unescape(const char*, int);

extern Var explode(Var s, char delim, bool mode);
extern Var implode(Var src, Var sep);

extern Var uppercase(Var s);
extern Var lowercase(Var s);

/*
 * Returns the length of the given list `l'.  Does *not* check to
 * ensure `l' is, in fact, a list.
 */
static inline Num
listlength(Var l)
{
    return l.length();
}

/*
 * Wraps `v' in a list if it is not already a list.  Consumes `v', so
 * you may want to var_ref/var_dup `v'.  Currently, this function is
 * called by functions that operate on an object's parents, which can
 * be either an object reference (TYPE_OBJ) or a list of object
 * references (TYPE_LIST).
 */
static inline Var
enlist_var(Var v)
{
    if (TYPE_LIST == v.type) return v;

    Var r = new_list(1);
    r[1] = v;
    return r;
}

/*
 * Iterate over the values in the list `lst'.  Sets `val' to each
 * value, in turn.  `itr' and `cnt' must be int variables.  In the
 * body of the statement, they hold the current index and total count,
 * respectively.  Use the macro as follows (assuming you already have
 * a list in `items'):
 *   Var item;
 *   int i, c;
 *   FOR_EACH(item, items, i, c) {
 *       printf("%d of %d, item = %s\n", i, c, value_to_literal(item));
 *   }
 */
#define FOR_EACH(val, lst, idx, cnt)				\
for (idx = 1, cnt = lst.length();			\
     idx <= cnt && (val = lst[idx], 1);			\
     idx++)

/*
 * Pop the first value off `stck' and put it in `tp'.
 */
#define POP_TOP(tp, stck)					\
tp = var_ref(stck[1]);					\
stck = listdelete(stck, 1);
#endif
