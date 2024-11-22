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

#define SEED0 (entry->key.type + 1) * MAP_HASH_SEED0
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
    if(!is_map) // let destroy_map handle it
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

    #ifdef MAP_DEBUG
        Var k = toliteral(entry->key);
        Var v = toliteral(entry->value);
        oklog("[%d:%d] map[%s] = %s\n", var_refcount(entry->key), var_refcount(entry->value), k.v.str, v.v.str);

        free_var(k);
        free_var(v);
    #endif

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
        hashmap_set_grow_by_power(map.v.map, 2);

        #ifdef MEMO_SIZE
            var_metadata *metadata = ((var_metadata*)map.v.map) - 1;
            metadata->size = static_cast<uint32_t>(sizeof(Var) + sizeof(map.v.map));
        #endif
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

    hashmap_free(map.v.map);
    return true;
}

/* called from utils.c */
Var
map_dup(Var map)
{
    Var _new = new_map(maplength(map));

    mapforeach(map, [&_new](Var key, Var value, int first) -> int { 
        map_entry _new_entry{.key = var_ref(key), .value = var_ref(value)};
        hashmap_set(_new.v.map, &_new_entry);
        return 0;
    });

    #ifdef ENABLE_GC
        gc_set_color(_new.v.map, gc_get_color(map.v.map));
    #endif

    #ifdef MEMO_SIZE
        var_metadata *_old_metadata = ((var_metadata*)map.v.map) - 1;
        var_metadata *_new_metadata = ((var_metadata*)_new.v.map) - 1;

        _new_metadata->size = _old_metadata->size;
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

    size_t size_change = (value_bytes(key) + value_bytes(value));

    if(map.v.map == emptymap.v.map) {
        Var _new = new_map(1);
        free_var(map);
        map = _new;
    } else if(var_refcount(map) > 1) {
        Var _new = map_dup(map);
        free_var(map);
        map = _new;
    }

    map_entry *_old_entry;
    map_entry _new_entry{.key = key, .value = value};
    _old_entry = (map_entry*)hashmap_set(map.v.map, &_new_entry);

    if(_old_entry != NULL) {
        size_change -= (value_bytes(_old_entry->key) + value_bytes(_old_entry->value));
        map_element_free(_old_entry);
    }

    #ifdef ENABLE_GC
        gc_set_color(map.v.map, GC_YELLOW);
    #endif

    #ifdef MEMO_SIZE
        var_metadata *metadata = ((var_metadata*)map.v.map) - 1;
        metadata->size += static_cast<uint32_t>(size_change);
    #endif

    return map;
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
    uint32_t size;

    #ifdef MEMO_SIZE
        var_metadata *metadata = ((var_metadata*)map.v.map) - 1;
        if(size = static_cast<int>(metadata->size)) return size;
    #endif

    size += static_cast<uint32_t>(sizeof(Var) + sizeof(map.v.map));

    mapforeach(map, [&size](Var key, Var value, int first) -> int {
        size += static_cast<uint32_t>(value_bytes(key)) + static_cast<uint32_t>(value_bytes(value));
        return 0;
    });

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
int mapforeach(Var map, map_callback func)
{ /* does NOT consume `map' */
    int ret;
    int first = 1;
    size_t iter = 1;
    map_entry *item;
    while (hashmap_iter(map.v.map, &iter, (void**)&item, false)) {
        ret = func(item->key, item->value, first);
        if (ret) return ret;
        first = 0;
    }

    return 0;
}

int
mapfirst(Var map, Var *value)
{
    if(maplength(map) == 0)
        return 0;

    if(value != nullptr) {
        size_t iter = 1;
        map_entry *item;

        if(hashmap_iter(map.v.map, &iter, (void**)&item, false)) {
            *value = var_ref(item->key);
        }
    }

    return 1;
}

int
maplast(Var map, Var *value)
{
    int len;
    if((len = maplength(map)) == 0)
        return len;

    if(value != nullptr) {
        size_t iter = mapbuckets(map) - 1;
        map_entry *item;

        if(hashmap_iter(map.v.map, &iter, (void**)&item, true)) {
            *value = var_ref(item->key);
        }
    }

    return len;
}

int
mapkeyindex(Var map, Var key)
{
    map_entry find{.key = key};

    int index = 0;
    int count = 0;
    mapforeach(map, [&find, &index, &count](Var k, Var v, int first) -> int {
        count++;
        map_entry next{.key = k};

        if(!map_compare(&find, &next, nullptr)) {
            index = count;
            return 1;
        }

        return 0;
    });

    free_var(key);
    return index;
}

/* Returns the specified range from the map.  `from' and `to' must be
 * valid iterators for the map or the behavior is unspecified.
 */
Var
maprange(Var map, int from, int to)
{   /* consumes `map' */
    Var r;

    if(to > maplength(map) || from <= 0 || from > to) {
        r.type = TYPE_ERR;
        r.v.err = E_RANGE;
        return r;
    }

    r = new_map(to - from + 1);
    auto count = 0;
    mapforeach(map, [&r, &from, &to, &count](Var key, Var value, int first) -> int {
        if(++count >= from && count <= to)
            mapinsert(r, var_ref(key), var_ref(value));
        return 0;
    });

    free_var(map);
    return r;
}

const map_entry*
maplookup(Var map, Var key, Var *value, int case_matters)
{
    map_entry *found;
    map_entry find{.key = key};

    if((found = (map_entry*)hashmap_get(map.v.map, &find)) && found != NULL) {
        if(value != nullptr) *value = found->value;
        return found;
    }

    return nullptr;
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
    if(to > maplength(map) || from <= 0 || from >= to)
        return E_RANGE;

    Var r = new_map(maplength(map) + maplength(value) - (to - from) + 1);
    size_t cnt = 0;

    mapforeach(map, [&cnt, &r, &from, &to](Var key, Var value, int first) -> int {
        if(++cnt < from || cnt > to)
            r = mapinsert(r, key, value);
        return 0;
    });

    mapforeach(value, [&r](Var key, Var value, int first) -> int {
        r = mapinsert(r, key, value);
        return 0;
    });

    *_new = r;
    return E_NONE;
}

map_entry *mapdelete(Var map, Var key)
{
    map_entry *old_entry;
    map_entry find{.key = key};

    if((old_entry = (map_entry*)hashmap_delete(map.v.map, (const void*)&find)) != NULL) {
        #ifdef MEMO_SIZE
            var_metadata *metadata = ((var_metadata*)map.v.map) - 1;
            metadata->size -= static_cast<uint32_t>((value_bytes(old_entry->key) + value_bytes(old_entry->value)));
        #endif
    }

    return old_entry;
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
    } else if(mapdelete(map, key) == NULL) {
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
    Var map = arglist.v.list[1];
    Var r = new_list(maplength(map));

    auto count = 0;
    mapforeach(map, [&r, &count](Var key, Var value, int first) -> int {
        r.v.list[++count] = var_ref(key);
        return 0;
    });

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_mapvalues(Var arglist, Byte next, void *vdata, Objid progr)
{
    const auto nargs = arglist.v.list[0].v.num;
    Var map = arglist.v.list[1];
    Var r;

    //nargs==1: simple mapvalues, dump all values of the map.
    if (nargs == 1) {
        r = new_list(maplength(map));

        auto count = 0;
        mapforeach(map, [&r, &count](Var key, Var value, int first) -> int {
            r.v.list[++count] = var_ref(value);
            return 0;
        });

        free_var(arglist);
        return make_var_pack(r);
    }

    r = new_list(nargs - 1);
    for (int i = 1; i < nargs; ++i) {
        if(const map_entry *entry = maplookup(map, arglist.v.list[i+1], nullptr, true)) {
            r.v.list[i] = var_ref(entry->value);
        } else {
            free_var(r);
            free_var(arglist);
            return make_error_pack(E_RANGE);
        }
    }

    free_var(arglist);
    return make_var_pack(r);
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
    bool case_matters = arglist.v.list[0].v.num >= 3 && is_true(arglist.v.list[3]);

    map_entry find{.key = key};
    Var ret = Var::new_bool(hashmap_get(map.v.map, &find) != nullptr);

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
