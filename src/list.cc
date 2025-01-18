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

#include <assert.h>
#include <stdlib.h>
#include <algorithm> // std::sort
#include <ctype.h>
#include <string.h>
#include <vector>

#include "my-math.h"
#include "bf_register.h"
#include "collection.h"
#include "config.h"
#include "execute.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "numbers.h"
#include "options.h"
#include "pattern.h"
#include "server.h"
#include "streams.h"
#include "storage.h"
#include "structures.h"
#include "tasks.h"
#include "unparse.h"
#include "utils.h"
#include "server.h"
#include "background.h"   // Threads
#include "random.h"
#include "db_private.h"
#include "db.h"

#include "dependencies/strnatcmp.c" // natural sorting

static Var emptylist;

static Var
empty_list()
{
    if(emptylist.v.list == nullptr) {
        Var *ptr = (Var *)mymalloc(1 * sizeof(Var), M_LIST);
        if(ptr == nullptr)
            panic_moo("EMPTY_LIST: mymalloc failed");
        
        emptylist.type = TYPE_LIST;
        emptylist.v.list = ptr;
        emptylist.v.list[0] = Var::new_int(0);

        #ifdef ENABLE_GC
            gc_set_color(emptylist.v.list, GC_GREEN);
        #endif
    }
    
    return emptylist;
}

Var
new_list(int size)
{    
    Var list;

    if (size == 0) {
        list = var_ref(empty_list());
        
        #ifdef ENABLE_GC
            assert(gc_get_color(list.v.list) == GC_GREEN);
        #endif

        return list;
    }

    Var *ptr = (Var *)mymalloc((next_power_of_two(size+1) * sizeof(Var)), M_LIST);
    if (ptr == nullptr)
        panic_moo("EMPTY_LIST: mymalloc failed");

    list.type = TYPE_LIST;
    list.v.list = ptr;
    list.v.list[0] = Var::new_int(size);

    if(size > 1) {
        Var z = Var::new_int(0);
        std::fill(std::addressof(list[1]), std::addressof(list[size])+1, z);
    }

    #ifdef MEMO_SIZE
        var_metadata *metadata = ((var_metadata*)list.v.list) - 1;
        metadata->size = sizeof(Var); // + (sizeof(Var) * size);
    #endif

    #ifdef ENABLE_GC
        if (list.length() > 0)   /* only non-empty lists */
            gc_set_color(list.v.list, GC_YELLOW);
    #endif

    return list;
}

/* called from utils.c */
bool
destroy_list(Var list)
{
    if(list.v.list == emptylist.v.list) {
        applog(LOG_WARNING, "WARNING: DESTROY_LIST() CALLED ON EMPTYLIST\n");
        return false;
    }

    int i;
    Var *pv;
    for (i = list.length(), pv = list.v.list + 1; i > 0; i--, pv++)
        free_var(*pv);

    // if((*pv).type != TYPE_ANON && (*pv).type != TYPE_STR) 
    /* Since this list could possibly be the root of a cycle, final
     * destruction is handled in the garbage collector if garbage
     * collection is enabled.
     */
    return true;
}

/* called from utils.c */
Var
list_dup(Var list)
{
    int i, n = list.length();
    Var _new = new_list(n);

    for (i = 1; i <= n; i++)
        _new[i] = var_ref(list[i]);

    #ifdef MEMO_SIZE
        var_metadata *metadata_old = ((var_metadata*)list.v.list) - 1;
        var_metadata *metadata_new = ((var_metadata*)_new.v.list) - 1;

        metadata_new->size = 0; //metadata_old->size;
    #endif

    #ifdef ENABLE_GC
        if (_new.length() > 0)   /* only non-empty lists */
            gc_set_color(_new.v.list, gc_get_color(list.v.list));
    #endif

    return _new;
}

int 
listforeach(Var list, list_callback func, bool reverse) 
{ /* does NOT consume `list' */
    auto len = list.length();

    int ret;

    if(!reverse) {
        for(auto i = 1; i <= len; i++)
            if ((ret = func(list[i], i))) return ret;
    } else {
        for(auto i = len; i >= 1; i--)
            if ((ret = func(list[i], i))) return ret;
    }

    return 0;
}

Var
setadd(Var list, Var value)
{
    if (ismember(value, list, 0)) {
        free_var(value);
        return list;
    }
    return listappend(list, value);
}

Var
setremove(Var list, Var value)
{
    int i;

    if ((i = ismember(value, list, 0)) != 0) {
        return listdelete(list, i);
    } else {
        return list;
    }
}

Var
listset(Var list, Var value, int pos)
{   /* consumes `list', `value' */
    Var _new = list;

    if (var_refcount(list) > 1) {
        _new = var_dup(list);
        free_var(list);
    }

    #ifdef MEMO_SIZE
        var_metadata *metadata = ((var_metadata*)_new.v.list) - 1;
        metadata->size += -(value_bytes(_new[pos])) + value_bytes(value);
    #endif

    free_var(_new[pos]);
    _new[pos] = value;

    #ifdef ENABLE_GC
        if(_new.length() > 0) /* only non-empty list */
            gc_set_color(_new.v.list, GC_YELLOW);
    #endif

    return _new;
}

/*
static Var
listreserve(Var list, int size) {
    list.v.list = (Var *) myrealloc(list.v.list, (next_power_of_two(size)) * sizeof(Var), M_LIST);
    return list;
}
*/

static Var
doinsert(Var list, Var value, int pos)
{
    Var _new;
    int size = list.length() + 1;

    if (var_refcount(list) == 1 && pos == size) {
        if(IS_POWER_OF_TWO(size))
            list.v.list = (Var *) myrealloc(list.v.list, (size << 1) * sizeof(Var), M_LIST);

        list.v.list[0] = Var::new_int(size);
        list[pos] = value;

        #ifdef MEMO_SIZE
            var_metadata *metadata = ((var_metadata*)list.v.list) - 1;
            metadata->size = IS_POWER_OF_TWO(size) ? 0 : (metadata->size + value_bytes(value));
        #endif

        #ifdef ENABLE_GC
            if(_new.length() > 0) /* only non-empty list */
                gc_set_color(list.v.list, GC_YELLOW);
        #endif

        return list;
    }

    _new = new_list(size);
    
    if(size > 1)
        for (int i = 1; i < pos; i++)
            _new[i] = var_ref(list[i]);
    
    _new[pos] = value;
    
    if(size > pos)
        for (int i = pos; i <= list.length(); i++)
            _new[i + 1] = var_ref(list[i]);

    #ifdef MEMO_SIZE
        var_metadata *metadata_old = ((var_metadata*)list.v.list) - 1;
        var_metadata *metadata_new = ((var_metadata*)_new.v.list) - 1;

        int old_size = metadata_old->size;

        metadata_new->size += value_bytes(list) + value_bytes(value);
    #endif

    free_var(list);

    #ifdef ENABLE_GC
        if(_new.length() > 0) /* only non-empty list */
            gc_set_color(_new.v.list, GC_YELLOW);
    #endif

    return _new;
}

Var
listinsert(Var list, Var value, int pos)
{
    if (pos <= 0)
        pos = 1;
    else if (pos > list.length())
        pos = list.length() + 1;
    return doinsert(list, value, pos);
}

Var
listappend(Var list, Var value)
{
    return doinsert(list, value, list.length() + 1);
}

Var
listdelete(Var list, int pos)
{
    Var _new;
    int i;
    int size = list.length() - 1;

    _new = new_list(size);

    #ifdef MEMO_SIZE
        var_metadata *metadata_old = ((var_metadata*)list.v.list) - 1;
        var_metadata *metadata_new = ((var_metadata*)_new.v.list) - 1;

        metadata_new->size = metadata_old->size - value_bytes(list[pos]);
    #endif

    for (i = 1; i < pos; i++) {
        _new[i] = var_ref(list[i]);
    }
    for (i = pos + 1; i <= list.length(); i++)
        _new[i - 1] = var_ref(list[i]);

    free_var(list);

    #ifdef ENABLE_GC
        if (size > 0)       /* only non-empty lists */
            gc_set_color(_new.v.list, GC_YELLOW);
    #endif

    return _new;
}

Var 
listconcat(Var first, Var second) {
    int len1 = first.length();
    int len2 = second.length();

    if((len1 == 0 && len2 == 0) || len2 == 0)
        return first;
    else if(len1 == 0)
        return second;
    
    int len = len1 + len2;

    if(len == 0)
        return new_list(0);

    Var _new = new_list(len);

    #ifdef MEMO_SIZE
        var_metadata *metadata_new    = ((var_metadata*)_new.v.list) - 1;
        var_metadata *metadata_first  = ((var_metadata*)first.v.list) - 1;
        var_metadata *metadata_second = ((var_metadata*)second.v.list) - 1;

        metadata_new->size = metadata_first->size + metadata_second->size;
    #endif

    _new[0] = Var::new_int(len);
    //if(len1 > 0) memcpy(&_new[1], &first[1], len1 * sizeof(Var));
    //if(len2 > 0) memcpy(&_new[len1 + 1], &second[1], len2 * sizeof(Var));

    // Add refs
    //for(auto i=1; i<=len; i++)
    //    _new[i] = var_ref(_new[i]);

    for(auto i=1; i<=len1; i++)
        _new[i] = var_ref(first[i]);

    for(auto i=1; i<=len2; i++)
        _new[len1 + i] = var_ref(second[i]);

    free_var(first);
    free_var(second);

    return _new;
}

Var
listrangeset(Var base, int from, int to, Var value)
{
    /* base and value are free'd */
    int index, offset = 0;
    int val_len = value.length();
    int base_len = base.length();
    int lenleft = (from > 1) ? from - 1 : 0;
    int lenmiddle = val_len;
    int lenright = (base_len > to) ? base_len - to : 0;
    int newsize = lenleft + lenmiddle + lenright;

    Var ans;
    ans = new_list(newsize);

    #ifdef MEMO_SIZE
        var_metadata *metadata_ans  = ((var_metadata*)ans.v.list) - 1;
        var_metadata *metadata_base = ((var_metadata*)base.v.list) - 1;
    #endif

    for (index = 1; index <= lenleft; index++) {
        ans[++offset] = var_ref(base[index]);
        #ifdef MEMO_SIZE
            metadata_ans->size += value_bytes(base[index]);
        #endif
    }

    for (index = 1; index <= lenmiddle; index++) {
        ans[++offset] = var_ref(value[index]);
        #ifdef MEMO_SIZE
            metadata_ans->size += -(value_bytes(ans[offset])) + value_bytes(value[index]);
        #endif
    }

    for (index = 1; index <= lenright; index++) {
        ans[++offset] = var_ref(base[to + index]);
        #ifdef MEMO_SIZE
            metadata_ans->size += value_bytes(base[to + index]);
        #endif
    }

    free_var(base);
    free_var(value);

    #ifdef ENABLE_GC
        if (newsize > 0)    /* only non-empty lists */
            gc_set_color(ans.v.list, GC_YELLOW);
    #endif

    return ans;
}

Var
sublist(Var list, int lower, int upper)
{
    auto len = list.length();
    if(len == 0)
        return list;

    Var r;
    int i;

    if(lower < 0)
        lower = lower + len;

    if(upper < 0)
        upper = upper + len;

    if (lower > upper && lower <= len && upper > 0) {
        r = new_list(lower - upper + 1);
        for (i = lower; i >= upper; i--)
            r.v.list[lower - i + 1] = var_ref(list.v.list[i]);
    } else if(upper > lower && upper <= len && lower > 0) {
        r = new_list(upper - lower + 1);
        for (i = lower; i <= upper; i++)
            r.v.list[i - lower + 1] = var_ref(list.v.list[i]);
    } else if(upper == lower && upper <= len && lower > 0) {
        r = new_list(1);
        r.v.list[1] = var_ref(list.v.list[lower]);
    } else {
        r = new_list(0);
    }

    free_var(list);

    #ifdef ENABLE_GC
        if(r.length() > 0)
            gc_set_color(r.v.list, GC_YELLOW);
    #endif

    return r;
}

int
listequal(Var lhs, Var rhs, int case_matters)
{
    if (lhs.v.list == rhs.v.list)
        return 1;

    if (lhs.length() != rhs.length())
        return 0;

    int i, c = lhs.length();
    for (i = 1; i <= c; i++) {
        if (!equality(lhs[i], rhs[i], case_matters))
            return 0;
    }

    return 1;
}

static inline void
stream_add_float(Stream *s, double v, bool should_round)
{
    char buffer[41];
    bool round = should_round && is_round(v);

    if(round) 
        snprintf(buffer, 40, "%.0f", v);
    else
        snprintf(buffer, 40, "%.*g", DBL_DIG, v);

    if (!round && !strchr(buffer, '.') && !strchr(buffer, 'e'))
        strncat(buffer, ".0", 40);

    stream_add_string(s, buffer);
}

static void
stream_add_complex(Stream * s, Var v)
{
    complex_t c = v.v.complex;
    double real = c.real();
    double imag = c.imag();

    bool has_real = std::fabs(real) > EPSILON32;
    bool has_imag = true; // std::fabs(imag) > EPSILON32;

    if(has_real) {
        if(has_imag) stream_add_char(s, '(');
        stream_add_float(s, c.real(), true);
    }

    if(has_imag) {
        if(has_real) 
            stream_add_string(s, imag >= 0.0 ? " + " : " - ");
        else if(imag < 0.0)
            stream_add_string(s, "-");

        stream_add_float(s, std::fabs(imag) > EPSILON32 ? abs(c.imag()) : 0.0, true);
        stream_add_string(s, (has_real) ? "i)" : "i");
    }
}

static void
stream_add_tostr(Stream * s, Var v)
{
    switch (v.type) {
        case _TYPE_TYPE:
            stream_printf(s, "%d", v.v.num & TYPE_DB_MASK);
            break;
        case TYPE_INT:
            stream_printf(s, "%" PRIdN, v.v.num);
            break;
        case TYPE_OBJ:
        {
            if(server_flag_option_cached(SVO_CORIFY_OBJ_TOSTR) > 0) {
                Var c = corified_as(v, 0);
                stream_printf(s, "%s", c.v.str);
                free_var(c);
            } else {
                stream_printf(s, "#%" PRIdN, v.v.obj);
            }
        }
        break;
        case TYPE_STR:
            stream_add_string(s, v.v.str);
            break;
        case TYPE_ERR:
            stream_add_string(s, unparse_error(v.v.err));
            break;
        case TYPE_FLOAT:
            unparse_value(s, v);
            break;
        case TYPE_MAP:
            stream_add_string(s, "[map]");
            break;
        case TYPE_LIST:
            stream_add_string(s, "{list}");
            break;
        case TYPE_ANON:
            stream_add_string(s, "*anonymous*");
            break;
        case TYPE_WAIF:
            stream_add_string(s, "[[waif]]");
            break;
        case TYPE_BOOL:
            stream_add_string(s, v.v.truth ? "true" : "false");
            break;
        case TYPE_CALL: 
        {
            Var c = corified_as(Var::new_obj(v.v.call->oid), 0);
            stream_printf(s, "%s::%s", c.str(), v.v.call->verbname.str());
            free_var(c);
        }
        break;
        case TYPE_COMPLEX:
            stream_add_complex(s, v);
            break;
        case TYPE_MATRIX:
            stream_add_string(s, "[MAT]");
            break;
        default:
            panic_moo("STREAM_ADD_TOSTR: Unknown Var type");
    }
}

const char *
value2str(Var value)
{
    if (value.type == TYPE_STR) {
        /* do this case separately to avoid two copies
         * and to ensure that the stream never grows */
        return str_ref(value.v.str);
    }
    else {
        static Stream *s = nullptr;
        if (!s)
            s = new_stream(32);
        stream_add_tostr(s, value);
        return str_dup(reset_stream(s));
    }
}

void
unparse_value(Stream * s, Var v)
{
    switch (v.type) {
        case _TYPE_TYPE:
        {
                const char *str = parse_type_literal(static_cast<var_type>(v.v.num));
                stream_printf(s, "%s", str);
                free_str(str);
        }
        break;
        case TYPE_INT:
            stream_printf(s, "%" PRIdN, v.v.num);
            break;
        case TYPE_OBJ:
        {
            if(server_flag_option_cached(SVO_CORIFY_OBJ_TOLITERAL) > 0) {
                Var c = corified_as(v, 0);
                stream_printf(s, "%s", c.v.str);
                free_var(c);
            } else {
                stream_printf(s, "#%" PRIdN, v.v.obj);
            }
        }
        break;
        case TYPE_ERR:
            stream_add_string(s, error_name(v.v.err));
            break;
        case TYPE_FLOAT:
            stream_add_float(s, v.fnum(), false);
            break;
        case TYPE_STR:
        {
            const char *str = v.v.str;

            stream_add_char(s, '"');
            while (*str) {
                switch (*str) {
                    case '"':
                    case '\\':
                        stream_add_char(s, '\\');
                    /* fall thru */
                    default:
                        stream_add_char(s, *str++);
                }
            }
            stream_add_char(s, '"');
        }
        break;
        case TYPE_LIST:
        {
            const char *sep = "";
            int len, i;

            stream_add_char(s, '{');
            len = v.length();
            for (i = 1; i <= len; i++) {
                stream_add_string(s, sep);
                sep = ", ";
                unparse_value(s, v[i]);
            }
            stream_add_char(s, '}');
        }
        break;
        case TYPE_MAP:
        {
            stream_add_char(s, '[');
            mapforeach(v, [&s](Var key, Var value, int index) -> int {
               if (index > 1) stream_add_string(s, ", ");
               unparse_value(s, key);
               stream_add_string(s, " -> ");
               unparse_value(s, value);

               return 0;
           });
            
           stream_add_char(s, ']');
        }
        break;
        case TYPE_ANON:
            stream_add_string(s, "*anonymous*");
            break;
        case TYPE_WAIF:
            stream_printf(s, "[[class = #%" PRIdN ", owner = #%" PRIdN "]]", v.v.waif->_class, v.v.waif->owner);
            break;
        case TYPE_BOOL:
            stream_printf(s, v.v.truth ? "true" : "false");
            break;
        case TYPE_CALL:
        {
            Var c = corified_as(Var::new_obj(v.v.call->oid), 0);
            stream_printf(s, "%s::%s", c.str(), v.v.call->verbname.str());
            free_var(c);
        }
        break;
        case TYPE_COMPLEX:
            stream_add_complex(s, v);
            break;
        case TYPE_MATRIX:
            stream_add_string(s, "[MAT]");
        case TYPE_NONE:
            stream_add_string(s, "(NONE)");
            break;
        case TYPE_CLEAR:
            stream_add_string(s, "[CLEAR]");
            break;
        default:
            stream_add_string(s, ">>Unknown value<<");
    }
}

std::string toliteral(Var args)
{
    Stream *s = new_stream(100);

    try {
        unparse_value(s, args);
        std::string r = reset_stream(s);
        free_stream(s);
        return r;
    }
    catch (stream_too_big& exception) {
        panic_moo("STREAM TOO BIG!");
    }

    free_stream(s);
    std::string r = ">>UNKNOWN VALUE<<";
    return r;
}

/* called from utils.c */
int
list_sizeof(Var *list)
{
    #ifdef MEMO_SIZE
        var_metadata *metadata = ((var_metadata*)list) - 1;
    #endif
    
    int i, len, size;

    #ifdef MEMO_SIZE
        if ((size = metadata->size) > 0)
            return size;
    #endif

    size = sizeof(Var); /* for the `length' element */
    len = list[0].v.num;
    for (i = 1; i <= len; i++) {
        size += value_bytes(list[i]);
    }

    #ifdef MEMO_SIZE
        metadata->size = size;
    #endif

    return size;
}

Var
strrangeset(Var base, int from, int to, Var value)
{
    /* base and value are free'd */
    int index, offset = 0;
    int val_len   = memo_strlen(value.v.str);
    int base_len  = memo_strlen(base.v.str);
    int lenleft   = (from > 1) ? from - 1 : 0;
    int lenmiddle = val_len;
    int lenright  = (base_len > to) ? base_len - to : 0;
    int newsize   = lenleft + lenmiddle + lenright;

    Var ans;
    char *s;

    ans.type = TYPE_STR;
    s = (char *)mymalloc(sizeof(char) * (newsize + 1), M_STRING);

    for (index = 0; index < lenleft; index++)
        s[offset++] = base.v.str[index];
    for (index = 0; index < lenmiddle; index++)
        s[offset++] = value.v.str[index];
    for (index = 0; index < lenright; index++)
        s[offset++] = base.v.str[index + to];
    s[offset] = '\0';
    ans.v.str = s;
    free_var(base);
    free_var(value);
    return ans;
}

Var
substr(Var str, int lower, int upper)
{
    Var r;
    bool reverse = false;

    r.type = TYPE_STR;
    if ((lower == 0 || upper == 0) && lower > upper)
        r.v.str = str_dup("");
    else {
        if(lower > upper) {
            std::swap(lower, upper);
            reverse = true;
        }

        int loop, index = 0;
        char *s = (char *)mymalloc(upper - lower + 2, M_STRING);

        if(lower == upper)
            s[index++] = str.v.str[lower-1];
        else {
            lower = std::max(lower, 1);
            upper = std::min(upper, static_cast<int>(memo_strlen(str.v.str)));
            if(!reverse) {
                for (loop = lower - 1; loop < upper; loop++)
                    s[index++] = str.v.str[loop];
            } else {
                for(loop = upper - 1; loop >= lower - 1; loop--)
                    s[index++] = str.v.str[loop];
            }
        }

        s[index] = '\0';
        r.v.str = s;
    }

    free_var(str);
    return var_ref(r);
}

Var
strget(Var str, int i)
{
    Var r;
    char *s;

    r.type = TYPE_STR;
    s = str_dup(" ");
    s[0] = str.v.str[i - 1];
    r.v.str = s;
    return r;
}

/**** helpers for catching overly large allocations ****/

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

/**** built in functions ****/

static package
bf_length(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    switch (arglist[1].type) {
        case TYPE_LIST:
            r = Var::new_int(arglist[1].length());
            break;
        case TYPE_MAP:
            r = Var::new_int(maplength(arglist[1]));
            break;
        case TYPE_STR:
            r = Var::new_int(memo_strlen(arglist[1].v.str));
            break;
        default:
            free_var(arglist);
            return make_error_pack(E_TYPE);
            break;
    }

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_setadd(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    Var lst = var_ref(arglist[1]);
    Var elt = var_ref(arglist[2]);

    free_var(arglist);

    r = setadd(lst, elt);

    return make_var_pack(r);
}

static package
bf_setremove(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r = setremove(var_ref(arglist[1]), arglist[2]);

    free_var(arglist);
    return make_var_pack(r);
}

static package
insert_or_append(Var arglist, int append1)
{
    int pos;
    Var r;
    Var lst = var_ref(arglist[1]);
    Var elt = var_ref(arglist[2]);

    if (arglist.length() == 2)
        pos = append1 ? lst.length() + 1 : 1;
    else {
        pos = arglist[3].v.num + append1;
        if (pos <= 0)
            pos = 1;
        else if (pos > lst.length() + 1)
            pos = lst.length() + 1;
    }
    free_var(arglist);

    r = doinsert(lst, elt, pos);

    return make_var_pack(r);
}

static package
bf_listappend(Var arglist, Byte next, void *vdata, Objid progr)
{
    return insert_or_append(arglist, 1);
}

static package
bf_listinsert(Var arglist, Byte next, void *vdata, Objid progr)
{
    return insert_or_append(arglist, 0);
}

static package
bf_listdelete(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    if (arglist[2].v.num <= 0
            || arglist[2].v.num > arglist[1].length()) {
        free_var(arglist);
        return make_error_pack(E_RANGE);
    }

    r = listdelete(var_ref(arglist[1]), arglist[2].v.num);

    free_var(arglist);

    return make_var_pack(r);
}

static package
bf_listset(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;

    Var lst = var_ref(arglist[1]);
    Var elt = var_ref(arglist[2]);
    int pos = arglist[3].v.num;

    free_var(arglist);

    if (pos <= 0 || pos > listlength(lst))
        return make_error_pack(E_RANGE);

    r = listset(lst, elt, pos);

    return make_var_pack(r);
}

static package
bf_equal(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r = Var::new_int(equality(arglist[1], arglist[2], 1));
    free_var(arglist);
    return make_var_pack(r);
}
Var
explode(Var s, char delim, bool mode) 
{
    if(s.length() <= 1 || strchr(s.str(), delim) == NULL) {
        Var ret = enlist_var(var_dup(s));
        free_var(s);
        return ret;
    } 

    char *found, *return_string, *freeme;
    Var ret = new_list(0);

    freeme = return_string = strdup(s.str());

    if (mode) {
        while ((found = strsep(&return_string, &delim)) != nullptr)
            ret = listappend(ret, str_dup_to_var(found));
    } else {
        found = strtok(return_string, &delim);
        while (found != nullptr) {
            ret = listappend(ret, str_dup_to_var(found));
            found = strtok(nullptr, &delim);
        }
    }

    free_var(s);
    free(freeme);

    return ret;
}

/* Return a list of substrings of an argument separated by a delimiter. */
static package
bf_explode(Var arglist, Byte next, void *vdata, Objid progr)
{
    auto nargs = arglist.length();
    char delim = (nargs >= 2) ? *arglist[2].str() : ' ';
    bool mode  = (nargs >= 3 && is_true(arglist[3]));

    Var ret = explode(arglist[1], delim, mode);

    free_var(arglist);
    return make_var_pack(ret);
}

static package
bf_slice(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var ret;
    int nargs = arglist.length();
    Var alist = arglist[1];
    Var index = (nargs < 2 ? Var::new_int(1) : arglist[2]);
    Var default_map_value = (nargs >= 3 ? arglist[3] : nothing);

    // Validate the types here since we used TYPE_ANY to allow lists and ints
    if (nargs > 1 && index.type != TYPE_LIST && index.type != TYPE_INT && index.type != TYPE_STR) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }

    // Check that that index isn't an empty list and doesn't contain negative or zeroes
    if (index.type == TYPE_LIST) {
        if (index.length() == 0) {
            free_var(arglist);
            return make_error_pack(E_RANGE);
        }

        for (int x = 1; x <= index.length(); x++) {
            if (index[x].type != TYPE_INT || index[x].v.num <= 0) {
                free_var(arglist);
                return make_error_pack((index[x].type != TYPE_INT ? E_INVARG : E_RANGE));
            }
        }
    } else if (index.v.num <= 0) {
        free_var(arglist);
        return make_error_pack(E_RANGE);
    }

    /* Ideally, we could allocate the list with the number of elements in our first list.
     * Unfortunately, if we need to return an error in the middle of setting elements in the return list,
     * we can't free_var the entire list because some elements haven't been set yet. So instead we do it the
     * old fashioned way unless/until somebody wants to refactor this to do all the error checking ahead of time. */
    ret = new_list(0);

    for (int x = 1; x <= alist.length(); x++) {
        Var element = alist[x];
        if ((element.type != TYPE_LIST && element.type != TYPE_STR && element.type != TYPE_MAP)
                || ((element.type == TYPE_MAP && index.type != TYPE_STR) || (index.type == TYPE_STR && element.type != TYPE_MAP))) {
            free_var(arglist);
            free_var(ret);
            return make_error_pack(E_INVARG);
        }
        if (index.type == TYPE_STR) {
            if (element.type != TYPE_MAP) {
                free_var(arglist);
                free_var(ret);
                return make_error_pack(E_INVARG);
            } else {
                Var tmp;
                if (maplookup(element, index, &tmp, 0) != nullptr)
                    ret = listappend(ret, var_ref(tmp));
                else if (nargs >= 3)
                    ret = listappend(ret, var_ref(default_map_value));
            }
        } else if (index.type == TYPE_INT) {
            if (index.v.num > (element.type == TYPE_STR ? memo_strlen(element.v.str) : element.length())) {
                free_var(arglist);
                free_var(ret);
                return make_error_pack(E_RANGE);
            } else {
                ret = listappend(ret, (element.type == TYPE_STR ? substr(var_ref(element), index.v.num, index.v.num) : var_ref(element[index.v.num])));
            }
        } else if (index.type == TYPE_LIST) {
            Var tmp = new_list(0);
            for (int y = 1; y <= index.length(); y++) {
                if (index[y].v.num > (element.type == TYPE_STR ? memo_strlen(element.v.str) : element.length())) {
                    free_var(arglist);
                    free_var(ret);
                    free_var(tmp);
                    return make_error_pack(E_RANGE);
                } else {
                    tmp = listappend(tmp, (element.type == TYPE_STR ? substr(var_ref(element), index[y].v.num, index[y].v.num) : var_ref(element[index[y].v.num])));
                }
            }
            ret = listappend(ret, tmp);
        }
    }
    free_var(arglist);
    return make_var_pack(ret);
}

/* Sorts various MOO types using std::sort.
 * Args: LIST <values to sort>, [LIST <values to sort by>], [INT <natural sort ordering?>], [INT <reverse?>] */
void
sort_callback(Var arglist, Var *ret, void *extra_data)
{
    const int nargs        = arglist.length();
    const int list_to_sort = (nargs >= 2 && arglist[2].length() > 0 ? 2 : 1);
    const bool natural     = (nargs >= 3 && is_true(arglist[3]));
    const bool reverse     = (nargs >= 4 && is_true(arglist[4]));

    if (arglist[list_to_sort].length() == 0) {
        *ret = new_list(0);
        return;
    } else if (list_to_sort == 2 && arglist[1].length() != arglist[2].length()) {
        ret->type = TYPE_ERR;
        ret->v.err = E_INVARG;
        return;
    }

    // Create and sort a vector of indices rather than values. This makes it easier to sort a list by another list.
    std::vector<size_t> s(arglist[list_to_sort].length());
    const var_type type_to_sort = arglist[list_to_sort][1].type;

    const Num list_length = arglist[list_to_sort].length();
    for (int count = 1; count <= list_length; count++)
    {
        var_type type = arglist[list_to_sort][count].type;
        if (type != type_to_sort || type == TYPE_LIST || type == TYPE_MAP || type == TYPE_ANON || type == TYPE_WAIF)
        {
            ret->type = TYPE_ERR;
            ret->v.err = E_TYPE;
            return;
        }
        s[count - 1] = count;
    }

    struct VarCompare {
        VarCompare(const Var *Arglist, const bool Natural) : m_Arglist(Arglist), m_Natural(Natural) {}

        bool operator()(const size_t a, const size_t b) const
        {
            const Var lhs = m_Arglist[a];
            const Var rhs = m_Arglist[b];

            switch (rhs.type) {
                case TYPE_INT:
                    return lhs.v.num < rhs.v.num;
                case TYPE_FLOAT:
                    return lhs.v.fnum < rhs.v.fnum;
                case TYPE_OBJ:
                    return lhs.v.obj < rhs.v.obj;
                case TYPE_ERR:
                    return ((int) lhs.v.err) < ((int) rhs.v.err);
                case TYPE_STR:
                    return (m_Natural ? strnatcasecmp(lhs.v.str, rhs.v.str) : strcasecmp(lhs.v.str, rhs.v.str)) < 0;
                default:
                    errlog("Unknown type in sort compare: %d\n", rhs.type);
                    return 0;
            }
        }
        const Var *m_Arglist;
        const bool m_Natural;
    };

    std::sort(s.begin(), s.end(), VarCompare(arglist[list_to_sort].v.list, natural));

    *ret = new_list(s.size());

    if (reverse)
        std::reverse(std::begin(s), std::end(s));

    int moo_list_pos = 0;
    for (const auto &it : s) {
        (*ret)[++moo_list_pos] = var_ref(arglist[1][it]);
    }
}

static package
bf_sort(Var arglist, Byte next, void *vdata, Objid progr)
{
    return background_thread(sort_callback, &arglist);
}

void 
all_members_thread_callback(Var arglist, Var *ret, void *extra_data)
{
    *ret = new_list(0);
    Var data = arglist[1];
    Var list = arglist[2];

    for (int x = 1, list_size = arglist[2].length(); x <= list_size; x++)
        if (equality(data, list[x], 0))
            *ret = listappend(*ret, Var::new_int(x));
}

/* Return the indices of all elements of a value in a list. */
static package
bf_all_members(Var arglist, Byte next, void *vdata, Objid progr)
{
    return background_thread(all_members_thread_callback, &arglist);
}

void range_callback(Var arglist, Var *ret, void *extra_data)
{
    int from = arglist[1].num();
    int step = arglist[2].num();
    int len  = arglist[3].num();

    Var r = new_list(len);

    for(auto i=0; i<r.length(); i++)
        r[i+1] = Var::new_int(from + (i * step));

    *ret = r;
}


static package 
bf_make(Var arglist, Byte next, void *vdata, Objid progr) 
{
    Var r = new_list(arglist[2].num());
    for(auto i=1; i<=arglist[2].num(); i++)
        r[i] = var_ref(arglist[1]);
    free_var(arglist);
    return make_var_pack(r);
}

static package 
bf_range(Var arglist, Byte next, void *vdata, Objid progr) 
{
    auto nargs = arglist.length();

    int from    = arglist[1].num();
    int to      = arglist[2].num();
    int step    = nargs >= 3 ? arglist[3].num() : 1;
    bool thread = nargs >= 4 ? arglist[4].v.truth : abs(from - to) >= (1 << 14);

    if(step == 0) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    } else if(from == to) {
        free_var(arglist);
        return make_var_pack(enlist_var(Var::new_int(from)));
    } else if((from < to && step < 0) || (from > to && step > 0)) {
        step = -step;
    }

    if(!thread) {
        Var r = new_list(abs((to - from) / step) + 1);

        for(auto i=0; i<r.length(); i++)
            r[i+1] = Var::new_int(from + (i * step));
        
        free_var(arglist);
        return make_var_pack(r);
    }

    Var args = new_list(3);

    args[1] = Var::new_int(from);
    args[2] = Var::new_int(step);
    args[3] = Var::new_int(abs((to - from) / step) + 1);

    free_var(arglist);
    return background_thread(range_callback, &args);
}

static package 
bf_shuffle(Var arglist, Byte next, void *vdata, Objid progr) {
    auto nargs = arglist.length();
    Var list = var_ref(arglist[1]);
    splitmix64 rng = nargs >= 2 ? new_splitmix64(arglist[2].unum()) : new_splitmix64(0);

    std::shuffle(std::addressof(list[1]), std::addressof(list[list.length()])+1, rng);
 
    free_var(arglist);
    return make_var_pack(list);
}

static package
bf_strsub(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (source, what, with [, case-matters]) */
    int case_matters = 0;
    Stream *s;
    package p;

    if (arglist.length() == 4)
        case_matters = is_true(arglist[4]);
    if (arglist[2].v.str[0] == '\0') {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }
    s = new_stream(100);
    TRY_STREAM;
    try {
        Var r;
        stream_add_strsub(s, arglist[1].v.str, arglist[2].v.str,
                          arglist[3].v.str, case_matters);
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

static int
signum(int x)
{
    return x < 0 ? -1 : (x > 0 ? 1 : 0);
}

static package
bf_strcmp(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (string1, string2) */
    Var r = Var::new_int(signum(strcmp(arglist[1].v.str, arglist[2].v.str)));
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_strtr(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (subject, from, to [, case_matters]) */
    Var r;
    int case_matters = 0;

    if (arglist.length() > 3)
        case_matters = is_true(arglist[4]);
    r.type = TYPE_STR;
    r.v.str = str_dup(strtr(arglist[1].v.str, memo_strlen(arglist[1].v.str),
                            arglist[2].v.str, memo_strlen(arglist[2].v.str),
                            arglist[3].v.str, memo_strlen(arglist[3].v.str),
                            case_matters));
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_index(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (source, what [, case-matters [, offset]]) */
    Var r;
    int case_matters = 0;
    int offset = 0;

    if (arglist.length() > 2)
        case_matters = is_true(arglist[3]);
    if (arglist.length() > 3)
        offset = arglist[4].v.num;
    if (offset < 0) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }

    r = Var::new_int(strindex(arglist[1].v.str + offset, memo_strlen(arglist[1].v.str) - offset,
                       arglist[2].v.str, memo_strlen(arglist[2].v.str),
                       case_matters));

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_rindex(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (source, what [, case-matters [, offset]]) */
    Var r;

    int case_matters = 0;
    int offset = 0;

    if (arglist.length() > 2)
        case_matters = is_true(arglist[3]);
    if (arglist.length() > 3)
        offset = arglist[4].v.num;
    if (offset > 0) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }

    r = Var::new_int(strrindex(arglist[1].v.str, memo_strlen(arglist[1].v.str) + offset,
                        arglist[2].v.str, memo_strlen(arglist[2].v.str),
                        case_matters));

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_tostr(Var arglist, Byte next, void *vdata, Objid progr)
{
    package p;
    Stream *s = new_stream(100);

    TRY_STREAM;
    try {
        Var r;
        int i;

        for (i = 1; i <= arglist.length(); i++) {
            stream_add_tostr(s, arglist[i]);
        }
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

static package
bf_toliteral(Var arglist, Byte next, void *vdata, Objid progr)
{
    package p;
    Stream *s = new_stream(100);

    TRY_STREAM;
    try {
        Var r;

        unparse_value(s, arglist[1]);
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

struct pat_cache_entry {
    char *string;
    int case_matters;
    Pattern pattern;
    struct pat_cache_entry *next;
};

static struct pat_cache_entry *pat_cache;
static struct pat_cache_entry pat_cache_entries[PATTERN_CACHE_SIZE];

static void
setup_pattern_cache()
{
    int i;

    for (i = 0; i < PATTERN_CACHE_SIZE; i++) {
        pat_cache_entries[i].string = nullptr;
        pat_cache_entries[i].pattern.ptr = nullptr;
    }

    for (i = 0; i < PATTERN_CACHE_SIZE - 1; i++)
        pat_cache_entries[i].next = &(pat_cache_entries[i + 1]);
    pat_cache_entries[PATTERN_CACHE_SIZE - 1].next = nullptr;

    pat_cache = &(pat_cache_entries[0]);
}

static Pattern
get_pattern(const char *string, int case_matters)
{
    struct pat_cache_entry *entry, **entry_ptr;

    entry = pat_cache;
    entry_ptr = &pat_cache;

    while (1) {
        if (entry->string && !strcmp(string, entry->string)
                && case_matters == entry->case_matters) {
            /* A cache hit; move this entry to the front of the cache. */
            break;
        } else if (!entry->next) {
            /* A cache miss; this is the last entry in the cache, so reuse that
             * one for this pattern, moving it to the front of the cache iff
             * the compilation succeeds.
             */
            if (entry->string) {
                free_str(entry->string);
                free_pattern(entry->pattern);
            }
            entry->pattern = new_pattern(string, case_matters);
            entry->case_matters = case_matters;
            if (!entry->pattern.ptr)
                entry->string = nullptr;
            else
                entry->string = str_dup(string);
            break;
        } else {
            /* not done searching the cache... */
            entry_ptr = &(entry->next);
            entry = entry->next;
        }
    }

    *entry_ptr = entry->next;
    entry->next = pat_cache;
    pat_cache = entry;
    return entry->pattern;
}

Var
do_match(Var arglist, int reverse)
{
    const char *subject, *pattern;
    int i;
    Pattern pat;
    Var ans;
    Match_Indices regs[10];

    subject = arglist[1].v.str;
    pattern = arglist[2].v.str;
    pat = get_pattern(pattern, (arglist.length() == 3
                                && is_true(arglist[3])));

    if (!pat.ptr) {
        ans.type = TYPE_ERR;
        ans.v.err = E_INVARG;
    } else
        switch (match_pattern(pat, subject, regs, reverse)) {
            default:
                panic_moo("do_match:  match_pattern returned unfortunate value.\n");
            /*notreached*/
            case MATCH_SUCCEEDED:
                ans = new_list(4);

                ans[1] = Var::new_int(regs[0].start);
                ans[2] = Var::new_int(regs[0].end);
                ans[3] = new_list(9);
                ans[4] = str_ref_to_var(subject);

                for (i = 1; i <= 9; i++) {
                    ans[3][i] = new_list(2);
                    ans[3][i][1] = Var::new_int(regs[i].start);
                    ans[3][i][2] = Var::new_int(regs[i].end);
                }

                break;
            case MATCH_FAILED:
                ans = new_list(0);
                break;
            case MATCH_ABORTED:
                ans.type = TYPE_ERR;
                ans.v.err = E_QUOTA;
                break;
        }

    return ans;
}

static package
bf_match(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var ans;

    ans = do_match(arglist, 0);
    free_var(arglist);
    if (ans.type == TYPE_ERR)
        return make_error_pack(ans.v.err);
    else
        return make_var_pack(ans);
}

static package
bf_rmatch(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var ans;

    ans = do_match(arglist, 1);
    free_var(arglist);
    if (ans.type == TYPE_ERR)
        return make_error_pack(ans.v.err);
    else
        return make_var_pack(ans);
}

int
invalid_pair(int num1, int num2, int max)
{
    if ((num1 == 0 && num2 == -1)
            || (num1 > 0 && num2 >= num1 - 1 && num2 <= max))
        return 0;
    else
        return 1;
}

int
check_subs_list(Var subs)
{
    const char *subj;
    int subj_length, loop;

    if (subs.type != TYPE_LIST || subs.length() != 4
            || subs[1].type != TYPE_INT
            || subs[2].type != TYPE_INT
            || subs[3].type != TYPE_LIST
            || subs[3].length() != 9
            || subs[4].type != TYPE_STR)
        return 1;
    subj = subs[4].v.str;
    subj_length = memo_strlen(subj);
    if (invalid_pair(subs[1].v.num, subs[2].v.num,
                     subj_length))
        return 1;

    for (loop = 1; loop <= 9; loop++) {
        Var pair;
        pair = subs[3][loop];
        if (pair.type != TYPE_LIST
                || pair.length() != 2
                || pair[1].type != TYPE_INT
                || pair[2].type != TYPE_INT
                || invalid_pair(pair[1].v.num, pair[2].v.num,
                                subj_length))
            return 1;
    }
    return 0;
}

static package
bf_substitute(Var arglist, Byte next, void *vdata, Objid progr)
{
    int template_length;
    const char *_template, *subject;
    Var subs, ans;
    package p;
    Stream *s;
    char c = '\0';

    _template = arglist[1].v.str;
    template_length = memo_strlen(_template);
    subs = arglist[2];

    if (check_subs_list(subs)) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }
    subject = subs[4].v.str;

    s = new_stream(template_length);
    TRY_STREAM;
    try {
        while ((c = *(_template++)) != '\0') {
            if (c != '%')
                stream_add_char(s, c);
            else if ((c = *(_template++)) == '%')
                stream_add_char(s, '%');
            else {
                int start = 0, end = 0;
                if (c >= '1' && c <= '9') {
                    Var pair = subs[3][c - '0'];
                    start = pair[1].v.num - 1;
                    end = pair[2].v.num - 1;
                } else if (c == '0') {
                    start = subs[1].v.num - 1;
                    end = subs[2].v.num - 1;
                } else {
                    p = make_error_pack(E_INVARG);
                    goto oops;
                }
                while (start <= end)
                    stream_add_char(s, subject[start++]);
            }
        }
        ans.type = TYPE_STR;
        ans.v.str = str_dup(stream_contents(s));
        p = make_var_pack(ans);
oops: ;
    }
    catch (stream_too_big& exception) {
        p = make_space_pack();
    }
    ENDTRY_STREAM;
    free_var(arglist);
    free_stream(s);
    return p;
}

static package
bf_value_bytes(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var value = arglist[1];

    #ifdef MEMO_SIZE
        if(value.is_pointer()) {
            var_metadata *metadata = ((var_metadata*)value.v.list) - 1;
            metadata->size = 0;
        }
    #endif

    Var r = Var::new_int(value_bytes(value));
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_decode_binary(Var arglist, Byte next, void *vdata, Objid progr)
{
    int length;
    const char *bytes = binary_to_raw_bytes(arglist[1].v.str, &length);
    int nargs = arglist.length();
    int fully = (nargs >= 2 && is_true(arglist[2]));
    Var r;
    int i;

    free_var(arglist);
    if (!bytes)
        return make_error_pack(E_INVARG);

    if (fully) {
        r = new_list(length);
        for (i = 1; i <= length; i++) {
            r[i] = Var::new_int((unsigned char) bytes[i - 1]);
        }
    } else {
        int count, in_string;
        Stream *s = new_stream(50);

        for (count = in_string = 0, i = 0; i < length; i++) {
            unsigned char c = bytes[i];

            if (isgraph(c) || c == ' ' || c == '\t') {
                if (!in_string)
                    count++;
                in_string = 1;
            } else {
                count++;
                in_string = 0;
            }
        }

        r = new_list(count);
        for (count = 1, in_string = 0, i = 0; i < length; i++) {
            unsigned char c = bytes[i];

            if (isgraph(c) || c == ' ' || c == '\t') {
                stream_add_char(s, c);
                in_string = 1;
            } else {
                if (in_string) {
                    r[count] = str_dup_to_var(reset_stream(s));
                    count++;
                }
                r[count] = Var::new_int(c);
                count++;
                in_string = 0;
            }
        }

        if (in_string)
            r[count] = str_dup_to_var(reset_stream(s));

        free_stream(s);
    }

    return make_var_pack(r);
}

static int
encode_binary(Stream * s, Var v, int minimum, int maximum)
{
    int i;

    switch (v.type) {
        case TYPE_INT:
            if (v.v.num < minimum || v.v.num > maximum)
                return 0;
            stream_add_char(s, (char) v.v.num);
            break;
        case TYPE_STR:
            stream_add_string(s, v.v.str);
            break;
        case TYPE_LIST:
            for (i = 1; i <= v.length(); i++)
                if (!encode_binary(s, v[i], minimum, maximum))
                    return 0;
            break;
        default:
            return 0;
    }

    return 1;
}

static package
bf_encode_binary(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    package p;
    Stream *s = new_stream(100);
    Stream *s2 = new_stream(100);

    TRY_STREAM;
    try {
        if (encode_binary(s, arglist, 0, 255)) {
            stream_add_raw_bytes_to_binary(
                s2, stream_contents(s), stream_length(s));
            r.type = TYPE_STR;
            r.v.str = str_dup(stream_contents(s2));
            p = make_var_pack(r);
        }
        else
            p = make_error_pack(E_INVARG);
    }
    catch (stream_too_big& exception) {
        p = make_space_pack();
    }
    ENDTRY_STREAM;
    free_stream(s2);
    free_stream(s);
    free_var(arglist);
    return p;
}

static package
bf_chr(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    package p;
    Stream *s = new_stream(100);

    TRY_STREAM;
    try {
        int encoded = (!is_wizard(progr) ? encode_binary(s, arglist, 32, 254) : encode_binary(s, arglist, 0, 255));
        if (encoded) {
            r.type = TYPE_STR;
            r.v.str = str_dup(stream_contents(s));
            p = make_var_pack(r);
        }
        else
            p = make_error_pack(E_INVARG);
    }
    catch (stream_too_big& exception) {
        p = make_space_pack();
    }
    ENDTRY_STREAM;
    free_stream(s);
    free_var(arglist);
    return p;
}

static package
bf_parse_ansi(Var arglist, Byte next, void *vdata, Objid progr)
{
#define ANSI_TAG_TO_CODE(tag, code, case_matters)           \
    {                                   \
        stream_add_strsub(str, reset_stream(tmp), tag, code, case_matters); \
        stream_add_string(tmp, reset_stream(str));              \
    }

    Var r;
    r.type = TYPE_STR;

    Stream *str = new_stream(50);
    Stream *tmp = new_stream(50);
    const char *random_codes[] = {"\e[31m", "\e[32m", "\e[33m", "\e[34m", "\e[35m", "\e[35m", "\e[36m"};

    stream_add_string(tmp, arglist[1].v.str);
    free_var(arglist);

    ANSI_TAG_TO_CODE("[red]",        "\e[31m",   0);
    ANSI_TAG_TO_CODE("[green]",      "\e[32m",   0);
    ANSI_TAG_TO_CODE("[yellow]",     "\e[33m",   0);
    ANSI_TAG_TO_CODE("[blue]",       "\e[34m",   0);
    ANSI_TAG_TO_CODE("[purple]",     "\e[35m",   0);
    ANSI_TAG_TO_CODE("[cyan]",       "\e[36m",   0);
    ANSI_TAG_TO_CODE("[normal]",     "\e[0m",    0);
    ANSI_TAG_TO_CODE("[inverse]",    "\e[7m",    0);
    ANSI_TAG_TO_CODE("[underline]",  "\e[4m",    0);
    ANSI_TAG_TO_CODE("[bold]",       "\e[1m",    0);
    ANSI_TAG_TO_CODE("[bright]",     "\e[1m",    0);
    ANSI_TAG_TO_CODE("[unbold]",     "\e[22m",   0);
    ANSI_TAG_TO_CODE("[blink]",      "\e[5m",    0);
    ANSI_TAG_TO_CODE("[unblink]",    "\e[25m",   0);
    ANSI_TAG_TO_CODE("[magenta]",    "\e[35m",   0);
    ANSI_TAG_TO_CODE("[unbright]",   "\e[22m",   0);
    ANSI_TAG_TO_CODE("[white]",      "\e[37m",   0);
    ANSI_TAG_TO_CODE("[gray]",       "\e[1;30m", 0);
    ANSI_TAG_TO_CODE("[grey]",       "\e[1;30m", 0);
    ANSI_TAG_TO_CODE("[beep]",       "\a",       0);
    ANSI_TAG_TO_CODE("[black]",      "\e[30m",   0);
    ANSI_TAG_TO_CODE("[b:black]",   "\e[40m",   0);
    ANSI_TAG_TO_CODE("[b:red]",     "\e[41m",   0);
    ANSI_TAG_TO_CODE("[b:green]",   "\e[42m",   0);
    ANSI_TAG_TO_CODE("[b:yellow]",  "\e[43m",   0);
    ANSI_TAG_TO_CODE("[b:blue]",    "\e[44m",   0);
    ANSI_TAG_TO_CODE("[b:magenta]", "\e[45m",   0);
    ANSI_TAG_TO_CODE("[b:purple]",  "\e[45m",   0);
    ANSI_TAG_TO_CODE("[b:cyan]",    "\e[46m",   0);
    ANSI_TAG_TO_CODE("[b:white]",   "\e[47m",   0);

    char *t = reset_stream(tmp);
    while (*t) {
        if (!strncasecmp(t, "[random]", 8)) {
            stream_add_string(str, random_codes[RANDOM() % 6]);
            t += 8;
        } else
            stream_add_char(str, *t++);
    }

    stream_add_strsub(tmp, reset_stream(str), "[null]", "", 0);

    ANSI_TAG_TO_CODE("[null]", "", 0);

    r.v.str = str_dup(reset_stream(tmp));

    free_stream(tmp);
    free_stream(str);
    return make_var_pack(r);

#undef ANSI_TAG_TO_CODE
}

static package
bf_remove_ansi(Var arglist, Byte next, void *vdata, Objid progr)
{

#define MARK_FOR_REMOVAL(tag)                   \
    {                               \
        stream_add_strsub(tmp, reset_stream(tmp), tag, "", 0);  \
    }
    Var r;
    Stream *tmp;

    tmp = new_stream(50);
    stream_add_string(tmp, arglist[1].v.str);
    free_var(arglist);

    MARK_FOR_REMOVAL("[red]");
    MARK_FOR_REMOVAL("[green]");
    MARK_FOR_REMOVAL("[yellow]");
    MARK_FOR_REMOVAL("[blue]");
    MARK_FOR_REMOVAL("[purple]");
    MARK_FOR_REMOVAL("[cyan]");
    MARK_FOR_REMOVAL("[normal]");
    MARK_FOR_REMOVAL("[inverse]");
    MARK_FOR_REMOVAL("[underline]");
    MARK_FOR_REMOVAL("[bold]");
    MARK_FOR_REMOVAL("[bright]");
    MARK_FOR_REMOVAL("[unbold]");
    MARK_FOR_REMOVAL("[blink]");
    MARK_FOR_REMOVAL("[unblink]");
    MARK_FOR_REMOVAL("[magenta]");
    MARK_FOR_REMOVAL("[unbright]");
    MARK_FOR_REMOVAL("[white]");
    MARK_FOR_REMOVAL("[gray]");
    MARK_FOR_REMOVAL("[grey]");
    MARK_FOR_REMOVAL("[beep]");
    MARK_FOR_REMOVAL("[black]");
    MARK_FOR_REMOVAL("[b:black]");
    MARK_FOR_REMOVAL("[b:red]");
    MARK_FOR_REMOVAL("[b:green]");
    MARK_FOR_REMOVAL("[b:yellow]");
    MARK_FOR_REMOVAL("[b:blue]");
    MARK_FOR_REMOVAL("[b:magenta]");
    MARK_FOR_REMOVAL("[b:purple]");
    MARK_FOR_REMOVAL("[b:cyan]");
    MARK_FOR_REMOVAL("[b:white]");
    MARK_FOR_REMOVAL("[random]");
    MARK_FOR_REMOVAL("[null]");

    r.type = TYPE_STR;
    r.v.str = str_dup(reset_stream(tmp));

    free_stream(tmp);
    return make_var_pack(r);

#undef MARK_FOR_REMOVAL
}

Var 
uppercase(Var s) {
    auto len = memo_strlen(s.str());
    char *str = (char*)s.str();

    for(auto i=0; i<len; i++)
        if(str[i] >= 'a' && str[i] <= 'z')
            str[i] += 'A'-'a';

    return s;
}

static package
bf_uppercase(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var ret = uppercase(var_dup(arglist[1]));
    free_var(arglist);
    return make_var_pack(ret);
}

Var
lowercase(Var s)
{
    auto len = memo_strlen(s.str());
    char *str = (char*)s.str();

    for(auto i=0; i<len; i++)
        if(str[i] >= 'A' && str[i] <= 'Z')
            str[i] += 'a'-'A';

    return s;
}

static package
bf_lowercase(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var ret = lowercase(var_dup(arglist[1]));
    free_var(arglist);
    return make_var_pack(ret);
}

typedef enum : Byte {
    STR_OP_LEFT = 1,
    STR_OP_RIGHT,
    STR_OP_BOTH
} string_op_mode;

static inline const char*
do_trim(char *str, char c, string_op_mode mode) {
    int len = memo_strlen(str);
    int i, j;

    if(mode & STR_OP_LEFT) {
        for(i = 0; i < len && str[i] == c; i++);

        for(j = 0; i < len; i++, j++)
            str[j] = str[i];

        str[j] = '\0';
    }

    if(mode & STR_OP_RIGHT)
        for(i = (mode & STR_OP_LEFT) ? j - 1 : len - 1; i >= 0 && str[i] == c; i--)
            str[i]='\0';

    return str;
}

static package
bf_str_trim(Var arglist, Byte next, void *vdata, Objid progr)
{
    char c = ' ';    
    if(arglist.length() >= 2) {
        if(memo_strlen(arglist[2].str()) != 1) {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
        c = arglist[2].str()[0];
    }

    Var ret = str_dup_to_var(do_trim((char*)arglist[1].str(), c, STR_OP_BOTH));

    free_var(arglist);
    return make_var_pack(ret);
}

static package
bf_str_triml(Var arglist, Byte next, void *vdata, Objid progr)
{
    char c = ' ';    
    if(arglist.length() >= 2) {
        if(memo_strlen(arglist[2].str()) != 1) {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
        c = arglist[2].str()[0];
    }

    Var ret = str_dup_to_var(do_trim((char*)arglist[1].str(), c, STR_OP_LEFT));

    free_var(arglist);
    return make_var_pack(ret);
}

static package
bf_str_trimr(Var arglist, Byte next, void *vdata, Objid progr)
{
    char c = ' ';    
    if(arglist.length() >= 2) {
        if(memo_strlen(arglist[2].str()) != 1) {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
        c = arglist[2].str()[0];
    }

    Var ret = str_dup_to_var(do_trim((char*)arglist[1].str(), c, STR_OP_RIGHT));

    free_var(arglist);
    return make_var_pack(ret);
}

static inline const char*
do_pad(const char *str, char *buf, int size, int len, char c, int mode) {
    memset((void*)buf, c, len);

    if(mode == STR_OP_LEFT)
        strncpy((char*)(buf + (len - size)), str, size);
    else if(mode == STR_OP_RIGHT)
        strncpy((char*)buf, str, size);
    else if(mode == STR_OP_BOTH)
        strncpy((char*)(buf + (len - size) / 2), str, size);

    buf[len]='\0';
    return buf;
}

static package
bf_str_pad(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var src = arglist[1];
    int len = arglist[2].num();
    char c = ' ';

    if(arglist.length() >= 3) {
        if(memo_strlen(arglist[3].str()) == 1)
            c = arglist[3].str()[0];
        else {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
    }

    Var r = Var::new_str(len + 1);
    r.v.str = do_pad(src.str(), r.v.str_, memo_strlen(src.v.str), len, c, STR_OP_BOTH);

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_str_padr(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var src = arglist[1];
    int len = arglist[2].num();
    char c = ' ';

    if(arglist.length() >= 3) {
        if(memo_strlen(arglist[3].str()) == 1)
            c = arglist[3].str()[0];
        else {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
    }

    Var r = Var::new_str(len + 1);
    r.v.str = do_pad(src.str(), r.v.str_, memo_strlen(src.v.str), len, c, STR_OP_RIGHT);

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_str_padl(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var src = arglist[1];
    int len = arglist[2].num();
    char c = ' ';

    if(arglist.length() >= 3) {
        if(memo_strlen(arglist[3].str()) == 1)
            c = arglist[3].str()[0];
        else {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
    }

    Var r = Var::new_str(len + 1);
    r.v.str = do_pad(src.str(), r.v.str_, memo_strlen(src.v.str), len, c, STR_OP_LEFT);

    free_var(arglist);
    return make_var_pack(r);
}

Var 
implode(Var src, Var sep)
{
    Var r;

    int src_size = 0;    
    int sep_size = memo_strlen(sep.str());

    for(auto i=1; i<=src.length(); i++) {
        if(src[i].type != TYPE_STR) {
            Var s = str_dup_to_var(toliteral(src[i]).c_str());
            std::swap(src[i], s);
            free_var(s);
        }
        src_size += memo_strlen(src[i].str());
    }

    r = Var::new_str(src_size + ((src.length() - 1) * sep_size) + 1);
    char *buf = (char*)r.str();

    int len = 0, pos = 0, src_len = src.length();
    for(auto i=1; i<=src_len; i++) {
        len = memo_strlen(src[i].str());
        strncpy((char*)(buf) + pos, src[i].str(), len);
        pos += len;
        strncpy((char*)(buf) + pos, sep.str(), sep_size);
        pos += sep_size;
    }

    buf[memo_strlen(buf)] = '\0';

    free_var(src);
    free_var(sep);

    return r;
}

static package
bf_implode(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r = implode(var_ref(arglist[1]), (arglist.length() >= 2) ? var_ref(arglist[2]) : str_dup_to_var(" "));
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_xxhash(Var arglist, Byte next, void *vdata, Objid progr)
{
    auto hash = arglist[1].hash();
    free_var(arglist);
    return make_var_pack(Var::new_int(hash));
}

#define ESC '\033'

const char* str_escape(const char *ptr, int len) {
    const char *str;
    Stream *s = new_stream(len * 4);
    TRY_STREAM;
    try {
        for (int i = 0; i < len; i++, ptr++) {
            switch (*ptr) {
                case '\0': stream_printf(s, "\\0");  break;
                case '\a': stream_printf(s, "\\a");  break;
                case '\b': stream_printf(s, "\\b");  break;
                case  ESC: stream_printf(s, "\\e");  break;
                case '\f': stream_printf(s, "\\f");  break;
                case '\n': stream_printf(s, "\\n");  break;
                case '\r': stream_printf(s, "\\r");  break;
                case '\t': stream_printf(s, "\\t");  break;
                case '\v': stream_printf(s, "\\v");  break;
                case '\\': stream_printf(s, "\\\\"); break;
                case '\?': stream_printf(s, "\\\?"); break;
                case '\'': stream_printf(s, "\\\'"); break;
                case '\"': stream_printf(s, "\\\""); break;
                default:   stream_printf(s, isprint(*ptr) ? "%c" : "\\%03o", *ptr);
            }
        }

        str = str_dup(stream_contents(s));
    } catch (stream_too_big& exception) {
        panic_moo("str_escape(): Stream too large");
    }
    ENDTRY_STREAM;

    free_stream(s);
    return str;
}

const char* str_unescape(const char *ptr, int len) {
    const char *str;
    Stream *s = new_stream(len * 4);
    TRY_STREAM;
    try {
        for (auto i = 0; i < len; i++, ptr++) {
            if(*ptr == '\\') {
                char c;
                if((c = (char)strtol(++ptr, (char**)&ptr, 0)))
                    stream_add_char(s, c);
                else {
                    switch (*ptr) {
                        case '0':  stream_add_char(s, '\0'); break;
                        case 'a':  stream_add_char(s, '\a'); break;
                        case 'b':  stream_add_char(s, '\b'); break;
                        case 'e':  stream_add_char(s,  ESC); break;
                        case 'f':  stream_add_char(s, '\f'); break;
                        case 'n':  stream_add_char(s, '\n'); break;
                        case 'r':  stream_add_char(s, '\r'); break;
                        case 't':  stream_add_char(s, '\t'); break;
                        case 'v':  stream_add_char(s, '\v'); break;
                        case '\\': stream_add_char(s, '\\'); break;
                        case '?':  stream_add_char(s, '\?'); break;
                        case '\'': stream_add_char(s, '\''); break;
                        case '"':  stream_add_char(s, '\"'); break;
                        default:   stream_add_char(s, *ptr);
                    }
                }
            } else stream_add_char(s, *ptr);
        }

        str = str_dup(stream_contents(s));
    } catch (stream_too_big& exception) {
        panic_moo("str_unescape(): Stream too large");
    }
    ENDTRY_STREAM;

    free_stream(s);
    return str;
}

#undef ESC

static package
bf_str_escape(Var arglist, Byte next, void *vdata, Objid progr) 
{
    const char *str = str_escape(arglist[1].str(), memo_strlen(arglist[1].str()));
    Var r = str_ref_to_var(str);
    free_str(str);
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_str_unescape(Var arglist, Byte next, void *vdata, Objid progr) 
{
    const char *str = str_unescape(arglist[1].str(), memo_strlen(arglist[1].str()));
    Var r = str_ref_to_var(str);
    free_str(str);
    free_var(arglist);
    return make_var_pack(r);
}

struct map_args_data {
    Var func;
    Var list;
    Var args;
};

static package
bf_map_args(Var arglist, Byte next, void *vdata, Objid progr) 
{
    auto nargs = arglist.length();
    enum error e = E_NONE;
    struct map_args_data *data;

    if(next == 1) {    
        data = (struct map_args_data*)alloc_data(sizeof(struct map_args_data));
        data->func = var_ref(arglist[1]);
        data->list = var_ref(arglist[2]);
        data->args = nargs >= 3 ? sublist(var_ref(arglist), 3, arglist.length()) : new_list(0);
    } else {
        data = (struct map_args_data*)vdata;
        data->list[next - 1] = arglist;

        if(next > data->list.length()) {
            return make_var_pack(data->list);
        }
    }

    Var call_args = new_list(1 + data->args.length());
    call_args[1] = data->list[next];
    for(auto i=1; i<=data->args.length(); i++) {
        call_args[i+1] = data->args[i];
    }

    db_verb_handle h = *(db_verb_handle*)data->func.v.call;
    Var definer = db_verb_definer(h);
    Var this_ = Var::new_obj(h.oid);

    e = call_verb2(definer.obj(), h.verbname.str(), is_valid(this_) ? this_ : definer, call_args, 0, DEFAULT_THREAD_MODE);

    return make_call_pack(++next, data);
}

void
register_list(void)
{
    register_function("chr",           0, -1, bf_chr);
    register_function("decode_binary", 1,  2, bf_decode_binary, TYPE_STR, TYPE_ANY);
    register_function("encode_binary", 0, -1, bf_encode_binary);
    register_function("value_bytes",   1,  1, bf_value_bytes, TYPE_ANY);
    register_function("xxhash",        1,  1, bf_xxhash, TYPE_ANY);

    /* list */
    register_function("length",      1,  1, bf_length, TYPE_ANY);
    register_function("setadd",      2,  2, bf_setadd, TYPE_LIST, TYPE_ANY);
    register_function("setremove",   2,  2, bf_setremove, TYPE_LIST, TYPE_ANY);
    register_function("listappend",  2,  3, bf_listappend, TYPE_LIST, TYPE_ANY, TYPE_INT);
    register_function("listinsert",  2,  3, bf_listinsert, TYPE_LIST, TYPE_ANY, TYPE_INT);
    register_function("listdelete",  2,  2, bf_listdelete, TYPE_LIST, TYPE_INT);
    register_function("listset",     3,  3, bf_listset, TYPE_LIST, TYPE_ANY, TYPE_INT);
    register_function("equal",       2,  2, bf_equal, TYPE_ANY, TYPE_ANY);
    register_function("explode",     1,  3, bf_explode, TYPE_STR, TYPE_STR, TYPE_INT);
    register_function("implode",     2,  2, bf_implode, TYPE_LIST, TYPE_STR);
    register_function("slice",       1,  3, bf_slice, TYPE_LIST, TYPE_ANY, TYPE_ANY);
    register_function("sort",        1,  4, bf_sort, TYPE_LIST, TYPE_LIST, TYPE_INT, TYPE_INT);
    register_function("all_members", 2,  2, bf_all_members, TYPE_ANY, TYPE_LIST);
    register_function("make",        2,  2, bf_make, TYPE_ANY, TYPE_INT);
    register_function("range",       2,  4, bf_range, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_BOOL);
    register_function("shuffle",     1,  2, bf_shuffle, TYPE_LIST, TYPE_INT);
    register_function("map_args",    2, -1, bf_map_args, TYPE_CALL, TYPE_LIST, TYPE_ANY);

    setup_pattern_cache();

    /* string */
    register_function("tostr",         0, -1, bf_tostr);
    register_function("toliteral",     1,  1, bf_toliteral, TYPE_ANY);
    register_function("match",         2,  3, bf_match, TYPE_STR, TYPE_STR, TYPE_ANY);
    register_function("rmatch",        2,  3, bf_rmatch, TYPE_STR, TYPE_STR, TYPE_ANY);
    register_function("substitute",    2,  2, bf_substitute, TYPE_STR, TYPE_LIST);
    register_function("index",         2,  4, bf_index, TYPE_STR, TYPE_STR, TYPE_ANY, TYPE_INT);
    register_function("rindex",        2,  4, bf_rindex, TYPE_STR, TYPE_STR, TYPE_ANY, TYPE_INT);
    register_function("strcmp",        2,  2, bf_strcmp, TYPE_STR, TYPE_STR);
    register_function("strsub",        3,  4, bf_strsub, TYPE_STR, TYPE_STR, TYPE_STR, TYPE_ANY);
    register_function("strtr",         3,  4, bf_strtr, TYPE_STR, TYPE_STR, TYPE_STR, TYPE_ANY);
    register_function("str_trim",      1,  2, bf_str_trim, TYPE_STR, TYPE_STR);
    register_function("str_triml",     1,  2, bf_str_triml, TYPE_STR, TYPE_STR);
    register_function("str_trimr",     1,  2, bf_str_trimr, TYPE_STR, TYPE_STR);
    register_function("str_pad",       2,  3, bf_str_pad, TYPE_STR, TYPE_INT, TYPE_STR);
    register_function("str_padr",      2,  3, bf_str_padr, TYPE_STR, TYPE_INT, TYPE_STR);
    register_function("str_padl",      2,  3, bf_str_padl, TYPE_STR, TYPE_INT, TYPE_STR);
    register_function("str_uppercase", 1,  1, bf_uppercase, TYPE_STR);
    register_function("str_lowercase", 1,  1, bf_lowercase, TYPE_STR);
    register_function("str_escape",    1,  1, bf_str_escape, TYPE_STR);
    register_function("str_unescape",  1,  1, bf_str_unescape, TYPE_STR);
    register_function("parse_ansi",    1,  1, bf_parse_ansi, TYPE_STR);
    register_function("remove_ansi",   1,  1, bf_remove_ansi, TYPE_STR);
}
