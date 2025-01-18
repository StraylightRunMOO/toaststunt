/******************************************************************************
  Copyright (c) 1995, 1996 Xerox Corporation.  All rights reserved.
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

/*****************************************************************************
 * Routines for manipulating verbs on DB objects
 *****************************************************************************/

#include <assert.h>
#include <ctype.h>

#include <stdlib.h>
#include <string.h>
#include <chrono>

#include "config.h"
#include "db.h"
#include "db_private.h"
#include "db_tune.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "parse_cmd.h"
#include "program.h"
#include "server.h"
#include "storage.h"
#include "utils.h"

static Var verb_cache;

/*********** Prepositions ***********/

#define MAXPPHRASE 3        /* max. number of words in a prepositional phrase */

/* NOTE: New prepositional phrases should only be added to this list at the
 * end, never in the middle, and no entries should every be removed from this
 * list; the list indices are stored raw in the DB file.
 */
static const char *prep_list[] =
{
    "with/using",
    "at/to",
    "in front of",
    "in/inside/into",
    "on top of/on/onto/upon",
    "out of/from inside/from",
    "over",
    "through",
    "under/underneath/beneath",
    "behind",
    "beside",
    "for/about",
    "is",
    "as",
    "off/off of",
};

#define NPREPS Arraysize(prep_list)

typedef struct pt_entry {
    int nwords;
    char *words[MAXPPHRASE];
    struct pt_entry *next;
} pt_entry;

struct pt_entry *prep_table[NPREPS];

void
dbpriv_build_prep_table(void)
{
    int i, j;
    int nwords;
    char **words;
    char cprep[100];
    const char *p;
    char *t;
    pt_entry *current_alias, **prev;

    for (i = 0; i < NPREPS; i++) {
        p = prep_list[i];
        prev = &prep_table[i];
        while (*p) {
            t = cprep;
            if (*p == '/')
                p++;
            while (*p && *p != '/')
                *t++ = *p++;
            *t = '\0';

            /* This call to PARSE_INTO_WORDS() is the reason that this function
             * is called from DB_INITIALIZE() instead of from the first call to
             * DB_FIND_PREP(), below.  You see, PARSE_INTO_WORDS() isn't
             * re-entrant, and DB_FIND_PREP() is called from code that's still
             * using the results of a previous call to PARSE_INTO_WORDS()...
             */
            words = parse_into_words(cprep, &nwords);

            current_alias = (struct pt_entry *)mymalloc(sizeof(struct pt_entry), M_PREP);
            current_alias->nwords = nwords;
            current_alias->next = nullptr;
            for (j = 0; j < nwords; j++)
                current_alias->words[j] = str_dup(words[j]);
            *prev = current_alias;
            prev = &current_alias->next;
        }
    }
}

db_prep_spec
db_find_prep(int argc, char *argv[], int *first, int *last)
{
    pt_entry *alias;
    int i, j, k;
    int exact_match = (first == nullptr || last == nullptr);

    for (i = 0; i < argc; i++) {
        for (j = 0; j < NPREPS; j++) {
            for (alias = prep_table[j]; alias; alias = alias->next) {
                if (i + alias->nwords <= argc) {
                    for (k = 0; k < alias->nwords; k++) {
                        if (strcasecmp(argv[i + k], alias->words[k]))
                            break;
                    }
                    if (k == alias->nwords
                            && (!exact_match || i + k == argc)) {
                        if (!exact_match) {
                            *first = i;
                            *last = i + alias->nwords - 1;
                        }
                        return (db_prep_spec) j;
                    }
                }
            }
        }
        if (exact_match)
            break;
    }
    return PREP_NONE;
}

db_prep_spec
db_match_prep(const char *prepname)
{
    db_prep_spec prep;
    int argc;
    char *ptr;
    char **argv;
    char *s, first;

    s = str_dup(prepname);
    first = s[0];
    if (first == '#')
        first = (++s)[0];
    prep = (db_prep_spec)strtol(s, &ptr, 10);
    if (*ptr == '\0') {
        free_str(s);
        if (!isdigit(first) || prep >= NPREPS)
            return PREP_NONE;
        else
            return prep;
    }
    ptr = strchr(s, '/');
    if (ptr != nullptr)
        *ptr = '\0';

    argv = parse_into_words(s, &argc);
    prep = db_find_prep(argc, argv, nullptr, nullptr);
    free_str(s);
    return prep;
}

const char *
db_unparse_prep(db_prep_spec prep)
{
    if (prep == PREP_NONE)
        return "none";
    else if (prep == PREP_ANY)
        return "any";
    else
        return prep_list[prep];
}


/*********** Verbs ***********/

#define DOBJSHIFT  4
#define IOBJSHIFT  6
#define OBJMASK    0x3
#define PERMMASK   0xF

int
db_add_verb(Var obj, const char *vnames, Objid owner, unsigned flags,
            db_arg_spec dobj, db_prep_spec prep, db_arg_spec iobj)
{
    Object *o = dbpriv_dereference(obj);
    Verbdef *v, *newv;
    int count;

    db_priv_affected_callable_verb_lookup();

    newv = (Verbdef *)mymalloc(sizeof(Verbdef), M_VERBDEF);
    newv->name = vnames;
    newv->owner = owner;
    
    Var meta;
    meta.type = TYPE_NONE;
    newv->meta = meta;
    
    newv->perms = flags | (dobj << DOBJSHIFT) | (iobj << IOBJSHIFT);
    newv->prep = prep;
    newv->next = nullptr;
    newv->program = nullptr;
    if (o->verbdefs) {
        for (v = o->verbdefs, count = 2; v->next; v = v->next, ++count);
        v->next = newv;
    } else {
        o->verbdefs = newv;
        count = 1;
    }
    return count;
}

static Verbdef *
find_verbdef_by_name(Object * o, const char *vname, int check_x_bit)
{
    Verbdef *v;

    for (v = o->verbdefs; v; v = v->next)
        if (verbcasecmp(v->name, vname)
                && (!check_x_bit || (v->perms & VF_EXEC)))
            break;

    return v;
}

int
db_count_verbs(Var obj)
{
    int count = 0;
    Object *o = dbpriv_dereference(obj);
    Verbdef *v;

    for (v = o->verbdefs; v; v = v->next)
        count++;

    return count;
}

int
db_for_all_verbs(Var obj,
                 int (*func) (void *data, const char *vname),
                 void *data)
{
    Object *o = dbpriv_dereference(obj);
    Verbdef *v;

    for (v = o->verbdefs; v; v = v->next)
        if ((*func) (data, v->name))
            return 1;

    return 0;
}

void
db_delete_verb(db_verb_handle vh)
{
    verb_handle *h = (verb_handle *)vh.ptr;
    Object *o = dbpriv_find_object(h->definer.obj());
    Verbdef *v = h->verbdef;
    Verbdef *vv;

    db_priv_affected_callable_verb_lookup();

    vv = o->verbdefs;
    if (vv == v)
        o->verbdefs = v->next;
    else {
        while (vv->next != v)
            vv = vv->next;
        vv->next = v->next;
    }

    if (v->program)
        free_program(v->program);
    if (v->name)
        free_str(v->name);
    myfree(v, M_VERBDEF);
}

db_verb_handle
db_find_command_verb(Objid oid, const char *verb,
                     db_arg_spec dobj, unsigned prep, db_arg_spec iobj)
{
    Object *o;
    Verbdef *v;
    static verb_handle h;
    db_verb_handle vh;

    Var ancestors;
    Var ancestor;
    int i, c;

    ancestors = db_ancestors(Var::new_obj(oid), true);

    FOR_EACH(ancestor, ancestors, i, c) {
        o = dbpriv_find_object(ancestor.v.obj);
        for (v = o->verbdefs; v; v = v->next) {
            db_arg_spec vdobj = (db_arg_spec)((v->perms >> DOBJSHIFT) & OBJMASK);
            db_arg_spec viobj = (db_arg_spec)((v->perms >> IOBJSHIFT) & OBJMASK);

            if (verbcasecmp(v->name, verb)
                    && (vdobj == ASPEC_ANY || vdobj == dobj)
                    && (v->prep == PREP_ANY || v->prep == prep)
                    && (viobj == ASPEC_ANY || viobj == iobj)) {
                h.definer = ancestor;
                h.verbdef = v;
                vh.ptr = &h;

                free_var(ancestors);

                return vh;
            }
        }
    }

    free_var(ancestors);

    vh.ptr = nullptr;

    return vh;
}

#ifdef VERB_CACHE

int db_verb_generation = 0;
int verbcache_hit      = 0;
int verbcache_neg_hit  = 0;
int verbcache_miss     = 0;

typedef struct vc_entry vc_entry;

struct vc_entry {
    unsigned int hash;
#ifdef RONG
    int generation;
#endif
    Object *object;
    char *verbname;
    verb_handle h;
    struct vc_entry *next;
};

static vc_entry **vc_table = nullptr;
static int vc_size = 0;

#define DEFAULT_VC_SIZE 7507

void
db_priv_affected_callable_verb_lookup(void)
{
    int i;
    vc_entry *vc, *vc_next;

    if (vc_table == nullptr)
        return;

    db_verb_generation++;

    for (i = 0; i < vc_size; i++) {
        vc = vc_table[i];
        while (vc) {
            vc_next = vc->next;
            free_str(vc->verbname);
            myfree(vc, M_VC_ENTRY);
            vc = vc_next;
        }
        vc_table[i] = nullptr;
    }
}

static void
make_vc_table(int size)
{
    int i;

    vc_size = size;
    vc_table = (vc_entry **)mymalloc(size * sizeof(vc_entry *), M_VC_TABLE);
    for (i = 0; i < size; i++) {
        vc_table[i] = nullptr;
    }
}

#define VC_CACHE_STATS_MAX 16

Var
db_verb_cache_stats(void)
{
    int i, depth, histogram[VC_CACHE_STATS_MAX + 1];
    vc_entry *vc;
    Var v, vv;

    for (i = 0; i < VC_CACHE_STATS_MAX + 1; i++) {
        histogram[i] = 0;
    }

    for (i = 0; i < vc_size; i++) {
        depth = 0;
        for (vc = vc_table[i]; vc; vc = vc->next)
            depth++;
        if (depth > VC_CACHE_STATS_MAX)
            depth = VC_CACHE_STATS_MAX;
        histogram[depth]++;
    }

    v = new_list(5);
    v[1] = Var::new_int(verbcache_hit);
    v[2] = Var::new_int(verbcache_neg_hit);
    v[3] = Var::new_int(verbcache_miss);
    v[4] = Var::new_int(db_verb_generation);

    vv = (v[5] = new_list(VC_CACHE_STATS_MAX + 1));
    for (i = 0; i < VC_CACHE_STATS_MAX + 1; i++) {
        vv[i + 1].type = TYPE_INT;
        vv[i + 1].v.num = histogram[i];
    }
    return v;
}

void
db_log_cache_stats(void)
{
    int i, depth, histogram[VC_CACHE_STATS_MAX + 1];
    vc_entry *vc;

    for (i = 0; i < VC_CACHE_STATS_MAX + 1; i++) {
        histogram[i] = 0;
    }

    for (i = 0; i < vc_size; i++) {
        depth = 0;
        for (vc = vc_table[i]; vc; vc = vc->next)
            depth++;
        if (depth > VC_CACHE_STATS_MAX)
            depth = VC_CACHE_STATS_MAX;
        histogram[depth]++;
    }

    oklog("Verb cache stat summary: %d hits, %d misses, %d generations\n",
          verbcache_hit, verbcache_miss, db_verb_generation);
    oklog("Depth   Count\n");
    for (i = 0; i < VC_CACHE_STATS_MAX + 1; i++)
        oklog("%-5d   %-5d\n", i, histogram[i]);
    oklog("---\n");
}

#endif

/*
 * Used by `db_find_callable_verb' once a suitable starting point
 * is found.  The function iterates through all ancestors looking
 * for a matching verbdef.
 */
struct verbdef_definer_data {
    Object *o;
    Verbdef *v;
};

static struct verbdef_definer_data
find_callable_verbdef(Object *start, const char *verb)
{
    Object *o = nullptr;
    Verbdef *v = nullptr;

    if ((v = find_verbdef_by_name(start, verb, 1)) != nullptr) {
        struct verbdef_definer_data data;
        data.o = start;
        data.v = v;
        return data;
    }

    Var stack = enlist_var(var_ref(start->parents));

    while (listlength(stack) > 0) {
        Var top;

        POP_TOP(top, stack);

        o = dbpriv_find_object(top.v.obj);
        free_var(top);

        if (!o) /* if it's invalid, AKA $nothing */
            continue;

        if ((v = find_verbdef_by_name(o, verb, 1)) != nullptr)
            break;

        if (TYPE_OBJ == o->parents.type)
            stack = listinsert(stack, var_ref(o->parents), 1);
        else
            stack = listconcat(var_ref(o->parents), stack);
    }

    free_var(stack);

    struct verbdef_definer_data data;
    data.o = o;
    data.v = v;
    return data;
}

/* does NOT consume `recv' and `verb' */
db_verb_handle
db_find_callable_verb2(Var recv, const char *verb)
{
    if (!recv.is_object())
        panic_moo("DB_FIND_CALLABLE_VERB: Not an object!");

    Object *o;
#ifdef VERB_CACHE
    vc_entry *new_vc;
#else
    static verb_handle h;
#endif
    db_verb_handle vh;

#ifdef VERB_CACHE
    /*
     * First, find the `first_parent_with_verbs'.  This is the first
     * ancestor of a parent that actually defines a verb.  If I define
     * verbs, then `first_parent_with_verbs' is me.  Otherwise,
     * iterate through each parent in turn, find the first ancestor
     * with verbs, and then try to find the verb starting at that
     * point.
     */
    Var stack = new_list(1);
    stack[1] = var_ref(recv);

try_again:
    while (listlength(stack) > 0) {
        Var top;

        POP_TOP(top, stack);

        if (top.is_object() && is_valid(top)) {
            o = dbpriv_dereference(top);
            if (o->verbdefs == nullptr) {
                /* keep looking */
                stack = (TYPE_OBJ == o->parents.type)
                        ? listinsert(stack, var_ref(o->parents), 1)
                        : listconcat(var_ref(o->parents), stack);
                free_var(top);
                continue;
            }
        }
        else {
            /* just consume it */
            free_var(top);
            continue;
        }

        free_var(top);

        assert(o != nullptr);

        unsigned long first_parent_with_verbs = (unsigned long)o;

        /* found something with verbdefs, now check the cache */
        unsigned int hash, bucket;
        vc_entry *vc;

        if (vc_table == nullptr)
            make_vc_table(DEFAULT_VC_SIZE);

        hash = str_hash(verb) ^ (~first_parent_with_verbs); /* ewww, but who cares */
        bucket = hash % vc_size;

        for (vc = vc_table[bucket]; vc; vc = vc->next) {
            if (hash == vc->hash
                    && o == vc->object && !strcasecmp(verb, vc->verbname)) {
                /* we haaave a winnaaah */
                if (vc->h.verbdef) {
                    verbcache_hit++;
                    vh.ptr = &vc->h;
                } else {
                    verbcache_neg_hit++;
                    vh.ptr = nullptr;
                }
                if (vh.ptr) {
                    free_var(stack);
                    return vh;
                }
                goto try_again;
            }
        }

        /* a swing and a miss */
        verbcache_miss++;

#else
    if (recv.is_object() && is_valid(recv))
        o = dbpriv_dereference(recv);
    else
        o = NULL;
#endif

#ifdef VERB_CACHE
        /*
         * Add the entry to the verbcache whether we find it or not.  This
         * means we do "negative caching", keeping track of failed lookups
         * so that repeated failures hit the cache instead of going
         * through a lookup.
         */
        new_vc = (vc_entry *)mymalloc(sizeof(vc_entry), M_VC_ENTRY);

        new_vc->hash = hash;
        new_vc->object = o;
        new_vc->verbname = str_dup(verb);
        new_vc->h.verbdef = nullptr;
        new_vc->next = vc_table[bucket];
        vc_table[bucket] = new_vc;
#endif

        struct verbdef_definer_data data = find_callable_verbdef(o, verb);
        if (data.o != nullptr && data.v != nullptr) {

#ifdef VERB_CACHE
            free_var(stack);

            new_vc->h.definer = Var::new_obj(data.o->id);
            new_vc->h.verbdef = data.v;
            vh.ptr = &new_vc->h;
#else
            h.definer = Var::new_obj(data.o->id);
            h.verbdef = data.v;
            vh.ptr = &h;
#endif
            return vh;
        }

#ifdef VERB_CACHE
    }

    free_var(stack);
#endif

    /*
     * note that the verbcache has cleared h.verbdef, so it defaults to a
     * "miss" cache if the for loop doesn't win
     */
    vh.ptr = nullptr;

    return vh;
}

static inline Var alloc_call(Var obj, Var verbname, bool found = true) {
    db_verb_handle *vh = (db_verb_handle*)mymalloc(sizeof(db_verb_handle), M_CALL);
    vh->ptr = nullptr;
    vh->oid = found ? obj.obj() : FAILED_MATCH;
    vh->verbname = var_dup(verbname);

    Var c;
    c.type = TYPE_CALL;
    c.v.call = vh;

    return var_ref(c);
}

static inline Var cache_timestamp() {
    using clock     = std::chrono::steady_clock;
    using duration  = std::chrono::duration<double, std::ratio<1>>;
    using ms        = std::chrono::time_point<clock, duration>;

    duration t = clock::now().time_since_epoch();

    return Var::new_float(t.count());
}

static inline void prune_verb_cache() {
    Var entries = new_list(0);
    mapforeach(verb_cache, [&entries](Var obj, Var cache, int i) -> int {
        mapforeach(cache, [&entries, &obj](Var verb, Var entry, int j) -> int {
            Var item = new_list(3);
            item[1] = obj;
            item[2] = var_ref(verb);
            item[3] = Var::new_float((cache_timestamp().fnum() - entry[3].fnum()) / entry[2].fnum());
            entries = listappend(entries, item);
            return 0;
        });
        return 0;
    });

    if(entries.length() > 1) {
        std::qsort(
            (void*)&entries.v.list[1],
            entries.length(),
            sizeof(Var),
            [](const void* x, const void* y)
            {
                const Var lhs = *static_cast<const Var*>(x);
                const Var rhs = *static_cast<const Var*>(y);
                return lhs.v.list[3].fnum() < rhs.v.list[3].fnum() ? -1 : 1;
            }
        );
    }

    int dc = 0;
    Var current, cache;
    for(auto i=entries.length(); i > 100; i--) {
        current = entries[i];
        const map_entry *m = maplookup(verb_cache, current[1], nullptr, 0);
        if(m != nullptr && mapdelete(m->value, var_ref(current[2]))) 
            dc++;
    }

    entries = sublist(entries, 1, entries.length() - dc);

    free_var(entries);
}

static inline Var ancestors_with_verbs(Var obj) {
    Var ancestors = db_ancestors(obj, true);
    Var r = new_list(0);
    listforeach(ancestors, [&r](Var value, int index) -> int {
        Object *o = dbpriv_find_object(value.obj());
        if(o != nullptr && o->verbdefs != nullptr)
            r = listappend(r, var_ref(value));
        return 0;
    });
    return var_ref(r);
}

Var get_entry(Var obj, Var verbname) {
    Var cache, entry;
    if(maplookup(verb_cache, obj, &cache, 0) != nullptr && maplookup(cache, verbname, &entry, 0) != nullptr)
        return entry;
    else    
        return Var::new_err(E_VERBNF);
}

void set_entry(Var obj, Var verbname, Var entry) {
    Var cache;
    if(maplookup(verb_cache, obj, &cache, 0) == nullptr) {
        cache = new_map(0);
    }

    cache = mapinsert(cache, var_ref(verbname), var_ref(entry));
    verb_cache = mapinsert(verb_cache, obj, var_ref(cache));
}

static inline db_verb_handle
db_verb_cache_find(Var obj, Var verbname)
{
    if(verb_cache.v.map == nullptr)
        verb_cache = new_map(0);

    db_verb_handle vh;

    Var entry = get_entry(obj, verbname);
    if(entry.type == TYPE_LIST) {
        vh = *entry[1].v.call;
        entry[2].v.num++;
    } else {
        vh.ptr = nullptr;
        vh.oid = obj;
        vh.verbname = var_ref(verbname);
    }

    return vh;
}

static inline db_verb_handle
db_verb_cache_add(Var obj, Var verbname)
{
    Var call;

    struct verbdef_definer_data data = find_callable_verbdef(dbpriv_dereference(obj), var_dup(verbname).str());

    if (data.o != nullptr && data.v != nullptr) {
        obj = Var::new_obj(data.o->id);
        call = alloc_call(obj, var_dup(verbname), true);
        verb_handle *h = (verb_handle*)mymalloc(sizeof(verb_handle), M_STRUCT);
        h->definer = obj;
        h->verbdef = data.v;
        call.v.call->ptr = h;
    } else {
        call = alloc_call(obj, var_dup(verbname), false);
    }

    Var entry = new_list(3);
    entry[1] = var_ref(call);
    entry[2] = Var::new_int(1);
    entry[3] = cache_timestamp();

    set_entry(obj, var_dup(verbname), var_dup(entry));

    return *call.v.call;
}

/* does NOT consume `recv' and `verb' */
db_verb_handle
db_find_callable_verb(Var recv, const char *verb)
{
    if (!recv.is_object())
        panic_moo("DB_FIND_CALLABLE_VERB: Not an object!");

    db_verb_handle vh;

    #ifdef VERB_CACHE
        Var verbname = str_dup_to_var(verb);
        listforeach(ancestors_with_verbs(recv), [&verbname, &vh](Var value, int index) -> int {
            vh = db_verb_cache_find(var_dup(value), var_dup(verbname));
            return vh.ptr || vh.oid == FAILED_MATCH ? 1 : 0;
        });

        if(vh.oid == FAILED_MATCH) {
            return vh;
        } else if(!vh.ptr) {
            vh = db_verb_cache_add(recv, var_dup(verbname));
        }
    #else
        static verb_handle h;
        Object *o = (recv.is_object() && is_valid(recv)) ? dbpriv_dereference(recv) : nullptr;

        struct verbdef_definer_data data = find_callable_verbdef(o, verb);
        if (data.o != nullptr && data.v != nullptr) {
            h.definer = Var::new_obj(data.o->id);
            h.verbdef = data.v;
            vh.ptr = &h;
            return vh;
        }

        vh.ptr = nullptr;
    #endif
    
    return vh;
}

Var db_verb_cache() {
    Var cache = new_list(2);
    cache[1] = var_ref(verb_cache);
    cache[2] = cache_timestamp();
    return cache;
}

Var make_call(Objid o, const char *vname) {
    db_verb_handle h = db_find_callable_verb(Var::new_obj(o), vname);
    db_verb_handle *vh = (db_verb_handle*)mymalloc(sizeof(db_verb_handle), M_CALL);

    vh->ptr = h.ptr;
    vh->oid = o;
    vh->verbname = str_dup_to_var(vname);
    
    Var c;
    c.type = TYPE_CALL;
    c.v.call = vh;
    return c;
}

bool destroy_call(Var c) {
    db_verb_handle *h = c.v.call;
    free_var(h->verbname);
    return true;
}

db_verb_handle
db_find_defined_verb(Var obj, const char *vname, int allow_numbers)
{
    Object *o = dbpriv_dereference(obj);
    Verbdef *v;
    char *p;
    int num, i;
    static verb_handle h;
    db_verb_handle vh;

    if (!allow_numbers ||
            (num = strtol(vname, &p, 10),
             (isspace(*vname) || *p != '\0')))
        num = -1;

    for (i = 0, v = o->verbdefs; v; v = v->next, i++)
        if (i == num || verbcasecmp(v->name, vname))
            break;

    if (v) {
        h.definer = obj;
        h.verbdef = v;
        vh.ptr = &h;

        return vh;
    }
    vh.ptr = nullptr;

    return vh;
}

db_verb_handle
db_find_indexed_verb(Var obj, unsigned index)
{
    Object *o = dbpriv_dereference(obj);
    Verbdef *v;
    unsigned i;
    static verb_handle h;
    db_verb_handle vh;

    for (v = o->verbdefs, i = 0; v; v = v->next)
        if (++i == index) {
            h.definer = obj;
            h.verbdef = v;
            vh.ptr = &h;

            return vh;
        }
    vh.ptr = nullptr;

    return vh;
}

Var
db_verb_definer(db_verb_handle vh)
{
    verb_handle *h = (verb_handle *) vh.ptr;
    Var r = h ? h->definer : Var::new_obj(-1);
    return r;
}

const char *
db_verb_names(db_verb_handle vh)
{
    verb_handle *h = (verb_handle*)vh.ptr;
    return h ? h->verbdef->name : "";
}

void
db_set_verb_names(db_verb_handle vh, const char *names)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    db_priv_affected_callable_verb_lookup();

    if (h) {
        if (h->verbdef->name)
            free_str(h->verbdef->name);
        h->verbdef->name = names;
    } else
        panic_moo("DB_SET_VERB_NAMES: Null verb_handle!");
}

Objid
db_verb_owner(db_verb_handle vh)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    if (h)
        return h->verbdef->owner;

    panic_moo("DB_VERB_OWNER: Null verb_handle!");
    return 0;
}

void
db_set_verb_owner(db_verb_handle vh, Objid owner)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    if (h)
        h->verbdef->owner = owner;
    else
        panic_moo("DB_SET_VERB_OWNER: Null verb_handle!");
}

Var
db_verb_meta(db_verb_handle vh)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    if (h) {
        if(h->verbdef->meta.type == TYPE_MAP) {
            return h->verbdef->meta;
        } else if(h->verbdef->meta.type == TYPE_LIST && h->verbdef->meta.length() > 0) {
            Var m = new_map(h->verbdef->meta.length());
            listforeach(h->verbdef->meta, [&m](Var value, int index) -> int {
                if(value.type != TYPE_LIST || value.length() <= 1)
                    m = mapinsert(m, Var::new_int(index), var_ref(value));
                else if(value.length() == 2)
                    m = mapinsert(m, var_ref(value[1]), var_ref(value[2]));
                else
                    m = mapinsert(m, var_ref(value[1]), sublist(var_ref(value), 2, value.length()));
                return 0;
            });

            return m;
        } else {
            return new_map(0);
        }
    }

    panic_moo("DB_VERB_META: Null verb_handle!");
    return Var::new_int(0);
}

void
db_set_verb_meta(db_verb_handle vh, Var meta)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    if (!h) return panic_moo("DB_SET_VERB_META: Null verb_handle!");

    free_var(h->verbdef->meta);

    switch(meta.type) {
        case TYPE_INT:
        case TYPE_OBJ:
        case TYPE_ERR:
        case TYPE_FLOAT:
        case TYPE_COMPLEX:
        case TYPE_MATRIX:
        case TYPE_CALL:
        case TYPE_STR:
        case TYPE_BOOL:
        case TYPE_ANON:
        case TYPE_WAIF:
        {
            h->verbdef->meta = enlist_var(var_ref(meta));
            break;
        }
        case TYPE_LIST:
        case TYPE_MAP:
        {
            Var m;
            if(meta.length() > 0) {
                m = var_ref(meta);
            } else {                
                m.type = TYPE_NONE;
            }
            h->verbdef->meta = m;
            break;
        }
        default:
        {
            Var m;
            m.type = TYPE_NONE;
            h->verbdef->meta = m;
            break;
        }
    }

    free_var(meta);
}

unsigned
db_verb_flags(db_verb_handle vh)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    if (h)
        return h->verbdef->perms & PERMMASK;

    panic_moo("DB_VERB_FLAGS: Null verb_handle!");
    return 0;
}

void
db_set_verb_flags(db_verb_handle vh, unsigned flags)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    db_priv_affected_callable_verb_lookup();

    if (h) {
        h->verbdef->perms &= ~PERMMASK;
        h->verbdef->perms |= flags;
    } else
        panic_moo("DB_SET_VERB_FLAGS: Null verb_handle!");
}

Program *
db_verb_program(db_verb_handle vh)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    if (h) {
        Program *p = h->verbdef->program;

        return p ? p : null_program();
    }
    panic_moo("DB_VERB_PROGRAM: Null verb_handle!");
    return nullptr;
}

void
db_set_verb_program(db_verb_handle vh, Program * program)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    if (h) {
        if (h->verbdef->program)
            free_program(h->verbdef->program);
        h->verbdef->program = program;
    } else
        panic_moo("DB_SET_VERB_PROGRAM: Null verb_handle!");
}

void
db_verb_arg_specs(db_verb_handle vh,
                  db_arg_spec * dobj, db_prep_spec * prep, db_arg_spec * iobj)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    if (h) {
        *dobj = (db_arg_spec)((h->verbdef->perms >> DOBJSHIFT) & OBJMASK);
        *prep = (db_prep_spec)h->verbdef->prep;
        *iobj = (db_arg_spec)((h->verbdef->perms >> IOBJSHIFT) & OBJMASK);
    } else
        panic_moo("DB_VERB_ARG_SPECS: Null verb_handle!");
}

void
db_set_verb_arg_specs(db_verb_handle vh,
                      db_arg_spec dobj, db_prep_spec prep, db_arg_spec iobj)
{
    verb_handle *h = (verb_handle *) vh.ptr;

    db_priv_affected_callable_verb_lookup();

    if (h) {
        h->verbdef->perms = ((h->verbdef->perms & PERMMASK)
                             | (dobj << DOBJSHIFT)
                             | (iobj << IOBJSHIFT));
        h->verbdef->prep = prep;
    } else
        panic_moo("DB_SET_VERB_ARG_SPECS: Null verb_handle!");
}

int
db_verb_allows(db_verb_handle h, Objid progr, db_verb_flag flag)
{
    return ((db_verb_flags(h) & flag)
            || progr == db_verb_owner(h)
            || is_wizard(progr));
}
