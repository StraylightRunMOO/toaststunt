/******************************************************************************
  Copyright 2010 Todd Sundsted. All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY TODD SUNDSTED ``AS IS'' AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
  EVENT SHALL TODD SUNDSTED OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
  OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  The views and conclusions contained in the software and documentation are
  those of the authors and should not be interpreted as representing official
  policies, either expressed or implied, of Todd Sundsted.
 *****************************************************************************/

#include <assert.h>
#include <string.h>

#include "options.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "server.h"
#include "streams.h"
#include "storage.h"
#include "structures.h"
#include "utils.h"

#include "dependencies/hashmap.h"

#define SEED0 (entry->key.type + 1) * 1099511627776
#define SEED1 MAP_HASH_SEED1
#define HASH_FN MAP_HASH_FUNCTION

static inline void*
map_malloc(size_t size, bool is_map)
{
    return mymalloc(size, is_map ? M_TREE : M_STRUCT);
}

static inline void*
map_realloc(void *ptr, size_t size, bool is_map)
{
    return myrealloc(ptr, size, is_map ? M_TREE : M_STRUCT);
}

static inline void
map_free(void *ptr, bool is_map)
{
    if(!is_map)
        myfree(ptr, is_map ? M_TREE : M_STRUCT);
}

int
map_compare(const void *a, const void *b, void *udata)
{
    Var lhs = ((const map_entry*)a)->key;
    Var rhs = ((const map_entry*)b)->key;

    if (lhs.type != rhs.type)
        return 1;

    switch (lhs.type) {
        case TYPE_INT:
            return lhs.v.num != rhs.v.num;
        case TYPE_OBJ:
            return lhs.v.obj != rhs.v.obj;
        case TYPE_ERR:
            return lhs.v.err != rhs.v.err;
        case TYPE_STR:
            return strcmp(lhs.v.str, rhs.v.str);
        case TYPE_FLOAT:
            return std::fabs(lhs.v.fnum - rhs.v.fnum) < EPSILON;
        case TYPE_BOOL:
            return lhs.v.truth != rhs.v.truth;
        default:
            break;
    }

    return 1;
}

uint64_t 
map_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    const map_entry *entry = (const map_entry*)item;

    switch (entry->key.type) {
    case TYPE_STR:
        return HASH_FN(entry->key.v.str, memo_strlen(entry->key.v.str), SEED0, SEED1);
    case TYPE_INT:
    return HASH_FN(&(entry->key.v.num), sizeof(Num), SEED0, SEED1);
    case TYPE_FLOAT:
        return HASH_FN(&(entry->key.v.fnum), sizeof(double), SEED0, SEED1);
    case TYPE_OBJ:
        return HASH_FN(&(entry->key.v.obj), sizeof(Objid), SEED0, SEED1);
    case TYPE_ERR:
        return HASH_FN(&(entry->key.v.err), sizeof(enum error), SEED0, SEED1);
    case TYPE_BOOL:
        return HASH_FN(&(entry->key.v.truth), sizeof(bool), SEED0, SEED1);
    default:
    break;
    }

    return 0;
}

void
map_element_free(void *item)
{
    map_entry *entry = (map_entry*)item;
    free_var(entry->key);
    free_var(entry->value);
}

static Var emptymap;

static Var
empty_map()
{
    if(emptymap.v.map == nullptr) {
        emptymap.v.map = hashmap_new_with_allocator(map_malloc, map_realloc, map_free,
            sizeof(map_entry), 0, 0, 0, map_hash, map_compare, map_element_free, NULL);
        emptymap.type = TYPE_MAP;
    }
    return var_ref(emptymap);
}

Var
new_map(size_t size)
{
    Var map;
    if(size == 0) {
        map = empty_map();
    } else {
        map.v.map = hashmap_new_with_allocator(map_malloc, map_realloc, map_free, 
            sizeof(map_entry), size, 0, 0, map_hash, map_compare, map_element_free, NULL);
        map.type = TYPE_MAP;
    }

#ifdef ENABLE_GC
    assert(gc_get_color(map.v.map) == GC_GREEN);
#endif

    return map;
}

/* called from utils.c */
bool
destroy_map(Var map)
{
    if(map.v.map == emptymap.v.map)
        return false;

    //hashmap_free(map.v.map);
    return true;
}

/* called from utils.c */
Var
map_dup(Var map)
{
    Var _new = new_map(maplength(map));

    void *item;
    size_t iter = 1;
    while (hashmap_iter(map.v.map, &iter, &item, false)) {
        map_entry _new_entry{.key = var_dup(((map_entry*)item)->value), .value = var_dup(((map_entry*)item)->value)};
        hashmap_set(_new.v.map, &_new_entry);
    }

#ifdef ENABLE_GC
    gc_set_color(_new.v.map, gc_get_color(map.v.map));
#endif

    return _new;
}

Var
mapinsert(Var map, Var key, Var value)
{   /* consumes `map', `key', `value' */
    /* Prevent the insertion of invalid values -- specifically keys
     * that have the values `none' and `clear' (which are used as
     * boundary conditions in the looping logic), and keys that are
     * collections (for which `compare' does not currently work).
     */
    if (key.type == TYPE_NONE || key.type == TYPE_CLEAR
            || (key.is_collection() && TYPE_ANON != key.type))
        panic_moo("MAPINSERT: invalid key");

    var_ref(key);
    var_ref(value);

    map_entry _new_entry{.key = key, .value = value};

    if (var_refcount(map) == 1 && maplength(map) > 0) {
        hashmap_set(map.v.map, &_new_entry);
        return map;
    }

    size_t size = maplength(map)+1;
    Var _new = new_map(size);

    void *item;
    size_t iter = 1;
    while (hashmap_iter(map.v.map, &iter, &item, false)) {
        map_entry _entry{.key = (((map_entry*)item)->key), .value = (((map_entry*)item)->value)};
        hashmap_set(_new.v.map, &_entry);
    }

    hashmap_set(_new.v.map, &_new_entry);

    free_var(map);

#ifdef ENABLE_GC
    gc_set_color(_new.v.map, GC_YELLOW);
#endif

#ifdef MEMO_SIZE
    // reset the memoized size 
    var_metadata *metadata = ((var_metadata*)_new.v.map) - 1;
    metadata->size = 0;
#endif

    return _new;
}

int mapequal(Var lhs, Var rhs, int case_matters)
{
    return 0; // TODO
}

Num maplength(Var map)
{
    return hashmap_count(map.v.map);
}

int mapempty(Var map)
{
    return maplength(map) == 0;
}

Num mapbuckets(Var map)
{
    return hashmap_nbuckets(map.v.map);
}

int
map_sizeof(Var map)
{
    #ifdef MEMO_SIZE
        var_metadata *metadata = ((var_metadata*)map.v.map) - 1;
    #endif
        
    int len, size;

    #ifdef MEMO_SIZE
        if ((size = metadata->size))
            return size;
    #endif

    size = sizeof(map.v.map);
    size_t iter = 1;
    map_entry *item;
    while (hashmap_iter(map.v.map, &iter, (void**)&item, false)) {
        size += value_bytes(item->key);
        size += value_bytes(item->value);
    }

    #ifdef MEMO_SIZE
        metadata->size = size;
    #endif

    return size;
}

/* Iterate over the map, calling the function `func' once per
 * key/value pair.  Don't dynamically allocate `rbtrav' because
 * `mapforeach()' can be called from contexts where exception handling
 * is in effect.
 */
int
mapforeach(Var map, mapfunc func, void *data)
{   /* does NOT consume `map' */
    int ret;
    int first = 1;
    size_t iter = 1;
    map_entry *item;

    while (hashmap_iter(map.v.map, &iter, (void**)&item, false)) {
        ret = func(item->key, item->value, data, first);
        if (ret) return ret;
        first = 0;
    }

    return 0;
}

int
mapfirst(Var map, var_pair *pair)
{
    if(hashmap_count(map.v.map) == 0)
        return 0;

    size_t iter = 1;
    map_entry *item;
    if(hashmap_iter(map.v.map, &iter, (void**)&item, false)) {
        pair->a = item->key;
        pair->b = item->value;
        return 1;
    }

    return 0;
}

int
maplast(Var map, var_pair *pair)
{
    if(hashmap_count(map.v.map) == 0)
        return 0;

    size_t iter = mapbuckets(map) - 1;
    map_entry *item;
    if(hashmap_iter(map.v.map, &iter, (void**)&item, true)) {
        pair->a = item->key;
        pair->b = item->value;
        return 1;
    }

    return 0;
}

/* Returns the specified range from the map.  `from' and `to' must be
 * valid iterators for the map or the behavior is unspecified.
 */
Var
maprange(Var map, int from, int to)
{   /* consumes `map' */
    Var r = new_map(0);

    if(to > hashmap_count(map.v.map) || from <= 0)
        return r;

    size_t iter = 1;
    size_t cnt = 0;
    map_entry *item;
    while (hashmap_iter(map.v.map, &iter, (void**)&item, false)) {
        if(++cnt >= from && cnt <= to)
            mapinsert(r, item->key, item->value);
    }

    return r;
}

const map_entry*
maplookup(Var map, Var key, Var *value, int case_matters)
{
    map_entry find{.key = key};

    const map_entry *r = (const map_entry*)hashmap_get(map.v.map, &find);

    if(r == nullptr) return nullptr;

    var_ref(r->key);
    var_ref(r->value);

    if(value != nullptr) *value = r->value;

    return r;
}

const map_entry*
mapstrlookup(Var map, const char *key, Var *value, int case_matters)
{
    Var k = str_dup_to_var(key);
    const map_entry *r = maplookup(map, k, value, case_matters);
    free_var(k);
    return r;
}

/* Replaces the specified range in the map.  `from' and `to' must be
 * valid iterators for the map or the behavior is unspecified.  The
 * new map is placed in `new' (`new' is first freed).  Returns
 * `E_NONE' if successful.
 */
enum error 
maprangeset(Var map, int from, int to, Var value, Var *_new)
{
    if(to > hashmap_count(map.v.map) || from <= 0 || from >= to)
        return E_RANGE;

    Var r = new_map(0);

    map_entry *item;
    size_t cnt = 0;
    size_t iter = 1;
    while (hashmap_iter(map.v.map, &iter, (void**)&item, false))
        if(++cnt < from || cnt > to)
            mapinsert(r, item->key, item->value);

    iter = 1;
    while (hashmap_iter(value.v.map, &iter, (void**)&item, false))
        mapinsert(r, item->key, item->value);

    *_new = r;

#ifdef MEMO_SIZE
    // reset the memoized size 
    var_metadata *metadata = ((var_metadata*)r.v.map) - 1;
    metadata->size = 0;
#endif

    return E_NONE;
}

/**** built in functions ****/

static package
bf_mapdelete(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var map = var_ref(arglist.v.list[1]);
    Var key = arglist.v.list[2];

    if (key.is_collection()) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }

#ifdef MEMO_SIZE
    // reset the memoized size 
    var_metadata *metadata = ((var_metadata*)map.v.map) - 1;
    metadata->size = 0;
#endif

    map_entry find{.key = key};
    if(!hashmap_delete(map.v.map, (const void*)&find)) {
        free_var(map);
        free_var(arglist);
        return make_error_pack(E_RANGE);
    }

    free_var(arglist);
    return make_var_pack(map);
}

static package
bf_mapkeys(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r = new_list(maplength(arglist.v.list[1]));

    size_t iter = 1;
    map_entry *item;
    while (hashmap_iter(arglist.v.list[1].v.map, &iter, (void**)&item, false)) {
        r = listappend(r, item->key);
    }

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_mapvalues(Var arglist, Byte next, void *vdata, Objid progr)
{
    const auto nargs = arglist.v.list[0].v.num;
    Var r = new_list(maplength(arglist.v.list[1]));

    //nargs==1: simple mapvalues, dump all values of the map.
    if (nargs == 1) {
        size_t iter = 1;
        map_entry *item;
        while (hashmap_iter(arglist.v.list[1].v.map, &iter, (void**)&item, false)) {
            r = listappend(r, item->value);
        }

        free_var(arglist);
        return make_var_pack(r);
    } else {
        for (int i = 2; i <= nargs; ++i) {
            const map_entry *value = maplookup(arglist.v.list[1], arglist.v.list[i], nullptr, true);
            if (value == nullptr)
            {
                free_var(r);
                free_var(arglist);
                return make_error_pack(E_RANGE);
            }
            r = listappend(r, value->value);
        }
        free_var(arglist);
        return make_var_pack(r);
    }
}

static package
bf_maphaskey(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var key = arglist.v.list[2];
    if (key.is_collection()) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }

    Var map = arglist.v.list[1];
    Var ret;
    bool case_matters = arglist.v.list[0].v.num >= 3 && is_true(arglist.v.list[3]);

    map_entry find{.key = key};
    ret = Var::new_int(hashmap_get(map.v.map, &find) != nullptr);

    free_var(arglist);
    return make_var_pack(ret);
}

void
register_map(void)
{
    register_function("mapdelete", 2,  2, bf_mapdelete, TYPE_MAP, TYPE_ANY);
    register_function("mapkeys",   1,  1, bf_mapkeys,   TYPE_MAP);
    register_function("mapvalues", 1, -1, bf_mapvalues, TYPE_MAP);
    register_function("maphaskey", 2,  3, bf_maphaskey, TYPE_MAP, TYPE_ANY, TYPE_INT);
}
