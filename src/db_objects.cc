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
 * Routines for manipulating DB objects
 *****************************************************************************/

#include <string.h>
#include <ctime>
#include <unordered_map>
#include <queue>

#include "config.h"
#include "db.h"
#include "db_io.h"
#include "db_private.h"
#include "collection.h"
#include "list.h"
#include "program.h"
#include "server.h"
#include "storage.h"
#include "utils.h"
#include "dependencies/xtrapbits.h"
#include "map.h"
#include "options.h"
#include "log.h"

static Object **objects;
static Num num_objects = 0;
static Num max_objects = 0;

static unsigned int nonce = 0;

static Var all_users;

#define MEMENTO_HIGHEST_OBJ -4
static std::unordered_map <Objid, Object*> memento_objects;
static std::atomic_int memento_count = MEMENTO_HIGHEST_OBJ;

#ifdef USE_ANCESTOR_CACHE
static Var ancestor_cache;
#endif /* USE_ANCESTOR_CACHE */

/* used in graph traversals */
static unsigned char *bit_array;
static size_t array_size = 0;

/*********** Objects qua objects ***********/

Object** dbpriv_objects() {
    return objects;
}

static inline bool 
memento(Objid oid)
{
    return (oid <= MEMENTO_HIGHEST_OBJ);
}

int 
is_memento(Objid oid)
{
    return memento(oid) ? 1 : 0;
}

Object *
dbpriv_find_object(Objid oid)
{
    if(memento(oid) && memento_objects.count(oid) > 0)
        return memento_objects[oid];
    else if (oid < 0 || oid >= max_objects)
        return nullptr;
    else
        return objects[oid];
}

int
valid(Objid oid)
{
    return dbpriv_find_object(oid) != nullptr;
}

Objid
db_last_used_objid(void)
{
    return num_objects - 1;
}

void
db_reset_last_used_objid(void)
{
    while (!objects[num_objects - 1])
        num_objects--;
}

void
db_set_last_used_objid(Objid oid)
{
    while (!objects[num_objects - 1] && num_objects > oid)
        num_objects--;
}

static void
extend(unsigned int new_objects)
{
    int size;

    for (size = 128; size <= new_objects; size *= 2)
        ;

    if (max_objects == 0) {
        objects = (Object **)mymalloc(size * sizeof(Object *), M_OBJECT_TABLE);
        memset(objects, 0, size * sizeof(Object *));
        max_objects = size;
    }
    if (size > max_objects) {
        Object **_new = (Object **)mymalloc(size * sizeof(Object *), M_OBJECT_TABLE);
        memcpy(_new, objects, max_objects * sizeof(Object *));
        memset(_new + max_objects, 0, (size - max_objects) * sizeof(Object *));
        myfree(objects, M_OBJECT_TABLE);
        objects = _new;
        max_objects = size;
    }

    for (size = 4096; size <= new_objects; size *= 2)
        ;

    if (array_size == 0) {
        bit_array = (unsigned char *)mymalloc((size / 8) * sizeof(unsigned char), M_ARRAY);
        array_size = size;
    }
    if (size > array_size) {
        myfree(bit_array, M_ARRAY);
        bit_array = (unsigned char *)mymalloc((size / 8) * sizeof(unsigned char), M_ARRAY);
        array_size = size;
    }
}

static void
ensure_new_object(void)
{
    extend(num_objects + 1);
}

void
dbpriv_assign_nonce(Object *o)
{
    o->nonce = nonce++;
}

int
dbpriv_nonce(Object *o)
{
    return o->nonce;
}

void dbpriv_fix_props_for_bmi(DB_Version old_version, DB_Version new_version) {
    oklog("UPGRADE: Upgrading database format from version %d => %d ...\n", old_version, new_version);

    int fixed_props = 0;
    int fixed_objects = 0;

    Object *o;
    for (auto i = 0; i < max_objects; i++) {
        if((o = dbpriv_find_object(i)) != nullptr && valid(o->id) && o->parents.type == TYPE_LIST && o->parents.length() > 1) {

            Var obj = Var::new_obj(o->id);            
            Var propvals = new_map(0);
            Var ancestors = db_ancestors(obj, true);

            listforeach(ancestors, [&obj, &propvals](Var ancestor, int index) -> int {
                return db_for_all_propdefs(ancestor, [&obj, &propvals](void *data, const char *prop_name) -> int {
                    db_prop_handle h = db_find_property(obj, prop_name, nullptr, true);
                    Var v = var_dup(db_property_value(h));

                    if(v.type == TYPE_CLEAR) v.type = TYPE_NONE;
                    propvals = mapinsert(propvals, str_dup_to_var(prop_name), v);

                    return 0;
                }, nullptr);
            });

            mapforeach(propvals, [&obj, &fixed_props](Var key, Var value, int index) -> int {
                db_prop_handle h = db_find_property(obj, key.v.str, nullptr, false);
                Var v = var_ref(value);
                if(v.type == TYPE_NONE) v.type = TYPE_CLEAR;
                db_set_property_value(h, v);
                fixed_props++;

                return 0;
            });

            free_var(propvals);
            free_var(ancestors);

            fixed_objects++;
        }
    }

    if(fixed_props > 0)
        oklog("UPGRADE: Fixed alignment of %d properties on %d objects (breadth-wise multiple inheritance upgrade)\n", fixed_props, fixed_objects);
    
    oklog("UPGRADE: Success! Current DB version: %d\n", new_version);
}

void
dbpriv_after_load(void)
{
    for (auto i = num_objects; i < max_objects; i++) {
        if (objects[i]) {
            dbpriv_assign_nonce(objects[i]);
            objects[i] = nullptr;
        }
    }
}

/* Both `dbpriv_new_object()' and `dbpriv_new_anonymous_object()'
 * allocate space for an `Object' and put the object into the array of
 * Objects.  The difference is the storage type used.  `M_ANON'
 * includes space for reference counts.
 */
Object *
dbpriv_new_object(Num new_objid)
{
    Object *o;

    if (new_objid <= 0 || new_objid >= num_objects) {
        ensure_new_object();
        new_objid = num_objects;
        num_objects++;
    }

    o = objects[new_objid] = (Object *)mymalloc(sizeof(Object), M_OBJECT);
    o->id = new_objid;
    o->waif_propdefs = nullptr;

    return o;
}

Object *
dbpriv_new_anonymous_object(void)
{
    Object *o;

    ensure_new_object();
    o = objects[num_objects] = (Object *)mymalloc(sizeof(Object), M_ANON);
    o->id = NOTHING;
    num_objects++;

    return o;
}

void
dbpriv_new_recycled_object(void)
{
    ensure_new_object();
    num_objects++;
}

#ifdef USE_ANCESTOR_CACHE
static inline void ancestor_cache_invalidate(Var obj) {
    Var deleted_keys = new_list(0);

    mapforeach(ancestor_cache, [&obj, &deleted_keys](Var key, Var value, int index) -> int {
        if(ismember(obj, key, false) || ismember(obj, value, false)) {
            deleted_keys = listappend(deleted_keys, var_ref(key));
        }
        return 0;
    });

    listforeach(deleted_keys, [](Var value, int index) -> int {
        mapdelete(ancestor_cache, value);
        return 0;
    });
    
    free_var(deleted_keys);
}
#endif

static Var unmoved;

void
db_init_object(Object *o)
{
    if(!clear_last_move && unmoved.v.map == nullptr) {
        unmoved = new_map(2);
        unmoved = mapinsert(unmoved, str_dup_to_var("source"), Var::new_obj(-1));
        unmoved = mapinsert(unmoved, str_dup_to_var("time"), Var::new_int(0));
    } else {
        unmoved.type = TYPE_NONE;
    }

    o->name = str_dup("");
    o->flags = 0;

    o->parents = var_ref(nothing);
    o->children = new_list(0);

    o->location = var_ref(nothing);
    o->last_move = var_ref(unmoved);
    o->contents = new_list(0);

    o->propval = nullptr;
    o->nval = 0;

    o->propdefs.max_length = 0;
    o->propdefs.cur_length = 0;
    o->propdefs.l = nullptr;

    o->verbdefs = nullptr;
}

void db_init_memento_object(Objid oid, Object *o) 
{
    if(!clear_last_move && unmoved.v.map == nullptr) {
        unmoved = new_map(2);
        unmoved = mapinsert(unmoved, str_dup_to_var("source"), Var::new_obj(-1));
        unmoved = mapinsert(unmoved, str_dup_to_var("time"), Var::new_int(0));
    } else {
        unmoved.type = TYPE_NONE;
    }

    o->id = oid;

    o->name      = str_dup("");
    o->flags     = 0;
    o->parents   = var_ref(nothing);
    o->children  = new_list(0);
    o->location  = var_ref(nothing);
    o->last_move = var_ref(unmoved);
    o->contents  = new_list(0);
    
    o->propval             = nullptr;
    o->nval                = 0;
    o->propdefs.max_length = 0;
    o->propdefs.cur_length = 0;
    o->propdefs.l          = nullptr;
    o->verbdefs            = nullptr;
    o->waif_propdefs       = nullptr;
}

Objid
db_create_object(Num new_objid)
{
    Object *o;

    if (new_objid <= 0 || new_objid > num_objects)
        new_objid = num_objects;

    o = dbpriv_new_object(new_objid);
    db_init_object(o);

    return o->id;
}

void db_destroy_memento_object(Objid oid) {
    Object *o = dbpriv_find_object(oid);

    if (!o) 
        panic_moo("DB_DESTROY_MEMENTO_OBJECT: Invalid object!");

    free_str(o->name);
    free_var(o->parents);
    free_var(o->children);
    free_var(o->location);
    free_var(o->last_move);
    free_var(o->contents);

    if((o->location.v.obj != NOTHING) || (o->contents.v.list[0].v.num != 0) || (o->children.v.list[0].v.num != 0))
        panic_moo("DB_DESTROY_MEMENTO_OBJECT: Not a barren orphan!");
    
    Verbdef *v, *w;
    int i;

    db_priv_affected_callable_verb_lookup();    
    for (i = 0; i < o->propdefs.cur_length; i++) {
        /* As an orphan, the only properties on this object are the ones
         * defined on it directly, so these two arrays must be the same length.
         */
        free_str(o->propdefs.l[i].name);
        free_var(o->propval[i].var);
    }

    if (o->propdefs.l)
        myfree(o->propdefs.l, M_PROPDEF);
    if (o->propval)
        myfree(o->propval, M_PVAL);
    o->nval = 0;

    for (v = o->verbdefs; v; v = w) {
        if (v->program)
            free_program(v->program);
        free_str(v->name);
        w = v->next;
        myfree(v, M_VERBDEF);
    }
    
    myfree(memento_objects[oid], M_OBJECT);
    memento_objects.erase(oid);
}

Objid create_memento_object(Var parents, Objid owner) {
    if(parents.type != TYPE_LIST && parents.type != TYPE_OBJ)
        panic_moo("TRIED TO CREATE MEMENTO OBJECT WITH BAD PARENTS!");

    Objid oid = memento_count--;
    
    Object *o = (Object*)mymalloc(sizeof(Object), M_OBJECT);

    db_init_memento_object(oid, o);
    memento_objects[oid] = o;

    Var memento = Var::new_obj(oid);
    if (!db_change_parents(memento, parents, none)) {
        db_destroy_memento_object(oid);
        memento_count++;
        return -1;
    }

    db_set_object_owner(oid, owner);

    return oid;
}

void
db_destroy_object(Objid oid)
{
    if(memento(oid))
        return db_destroy_memento_object(oid);

    Object *o = dbpriv_find_object(oid);
    Verbdef *v, *w;
    int i;

    db_priv_affected_callable_verb_lookup();

    if (!o)
        panic_moo("DB_DESTROY_OBJECT: Invalid object!");

    if (o->location.v.obj != NOTHING ||
            o->contents.length() != 0 ||
            (o->parents.type == TYPE_OBJ && o->parents.v.obj != NOTHING) ||
            (o->parents.type == TYPE_LIST && o->parents.length() != 0) ||
            o->children.length() != 0)
        panic_moo("DB_DESTROY_OBJECT: Not a barren orphan!");

    free_var(o->parents);
    free_var(o->children);

    free_var(o->location);
    free_var(o->last_move);
    free_var(o->contents);

    if (is_user(oid)) {
        Var t;

        t.type = TYPE_OBJ;
        t.v.obj = oid;
        all_users = setremove(all_users, t);
    }
    free_str(o->name);

    for (i = 0; i < o->propdefs.cur_length; i++) {
        /* As an orphan, the only properties on this object are the ones
         * defined on it directly, so these two arrays must be the same length.
         */
        free_str(o->propdefs.l[i].name);
        free_var(o->propval[i].var);
    }
    if (o->propdefs.l)
        myfree(o->propdefs.l, M_PROPDEF);
    if (o->propval)
        myfree(o->propval, M_PVAL);
    o->nval = 0;

    for (v = o->verbdefs; v; v = w) {
        if (v->program)
            free_program(v->program);
        free_str(v->name);
        w = v->next;
        myfree(v, M_VERBDEF);
    }

#ifdef USE_ANCESTOR_CACHE
    ancestor_cache_invalidate(Var::new_obj(oid));
#endif

    myfree(objects[oid], M_OBJECT);
    objects[oid] = nullptr;
}

Var
db_read_anonymous()
{
    Var r;
    int oid;

    if ((oid = dbio_read_num()) == NOTHING) {
        r.type = TYPE_ANON;
        r.v.anon = nullptr;
    } else if (max_objects && (oid < max_objects && objects[oid])) {
        r.type = TYPE_ANON;
        r.v.anon = objects[oid];
        addref(r.v.anon);
    }
    else {
        /* Back up the permanent object count, temporarily store the
         * database id of this anonymous object as the new count, and
         * allocate a new object -- this will force the new anonymous
         * object to be allocated at that position -- restore the
         * count when finished.
         */
        int sav_objects = num_objects;
        num_objects = oid;
        dbpriv_new_anonymous_object();
        num_objects = sav_objects;
        r.type = TYPE_ANON;
        r.v.anon = objects[oid];
    }

    return r;
}

void
db_write_anonymous(Var v)
{
    Objid oid;
    Object *o = (Object *)v.v.anon;

    if (!is_valid(v))
        oid = NOTHING;
    else if (o->id != NOTHING)
        oid = o->id;
    else {
        ensure_new_object();
        objects[num_objects] = o;
        oid = o->id = num_objects;
        num_objects++;
    }

    dbio_write_num(oid);
}

Object *
db_make_anonymous(Objid oid, Objid last)
{
    Object *o = objects[oid];
    Var old_parents = o->parents;
    Var me = Var::new_obj(oid);

    Var parent;
    int i, c;

    /* remove me from my old parents' children */
    if (old_parents.type == TYPE_OBJ && old_parents.v.obj != NOTHING)
        objects[old_parents.v.obj]->children = setremove(objects[old_parents.v.obj]->children, me);
    else if (old_parents.type == TYPE_LIST)
        FOR_EACH(parent, old_parents, i, c)
        objects[parent.v.obj]->children = setremove(objects[parent.v.obj]->children, me);

    objects[oid] = nullptr;
    db_set_last_used_objid(last);

    o->id = NOTHING;

    free_var(o->children);
    free_var(o->location);
    free_var(o->last_move);
    free_var(o->contents);

    /* Last step, reallocate the memory and copy -- anonymous objects
     * require space for reference counting.
     */
    Object *t = (Object *)mymalloc(sizeof(Object), M_ANON);
    memcpy(t, o, sizeof(Object));
    myfree(o, M_OBJECT);

    return t;
}

void
db_destroy_anonymous_object(void *obj)
{
    Object *o = (Object *)obj;
    Verbdef *v, *w;
    int i;

    free_str(o->name);
    o->name = nullptr;

    free_var(o->parents);

    for (i = 0; i < o->propdefs.cur_length; i++)
        free_str(o->propdefs.l[i].name);
    if (o->propdefs.l)
        myfree(o->propdefs.l, M_PROPDEF);
    for (i = 0; i < o->nval; i++)
        free_var(o->propval[i].var);
    if (o->propval)
        myfree(o->propval, M_PVAL);
    o->nval = 0;

    for (v = o->verbdefs; v; v = w) {
        if (v->program)
            free_program(v->program);
        free_str(v->name);
        w = v->next;
        myfree(v, M_VERBDEF);
    }

    dbpriv_set_object_flag(o, FLAG_INVALID);

    /* Since this object could possibly be the root of a cycle, final
     * destruction is handled in the garbage collector if garbage
     * collection is enabled.
     */
    /*myfree(o, M_ANON);*/
}

int
parents_ok(Object *o)
{
    if (TYPE_LIST == o->parents.type) {
        Var parent;
        int i, c;
        FOR_EACH(parent, o->parents, i, c) {
            Objid oid = parent.v.obj;
            if (!valid(oid) || dbpriv_find_object(oid)->nonce > o->nonce) {
                dbpriv_set_object_flag(o, FLAG_INVALID);
                return 0;
            }
        }
    }
    else {
        Objid oid = o->parents.v.obj;
        if (NOTHING != oid && (!valid(oid) || dbpriv_find_object(oid)->nonce > o->nonce)) {
            dbpriv_set_object_flag(o, FLAG_INVALID);
            return 0;
        }
    }

    return 1;
}

int
anon_valid(Object *o)
{
    return o
           && !dbpriv_object_has_flag(o, FLAG_INVALID)
           && parents_ok(o);
}

int
is_valid(Var obj)
{
    return valid(obj.v.obj);
}

Objid
db_renumber_object(Objid old)
{
    Objid _new;
    Object *o;

#ifdef USE_ANCESTOR_CACHE
    ancestor_cache_invalidate(Var::new_obj(old));
#endif /* USE_ANCESTOR_CACHE */

    for (_new = 0; _new < old; _new++) {
        if (objects[_new] == nullptr) {
            /* Change the identity of the object. */
            o = objects[_new] = objects[old];
            objects[old] = nullptr;
            objects[_new]->id = _new;

            /* Fix up the parents/children hierarchy and the
             * location/contents hierarchy.
             */
            int i1, c1, i2, c2;
            Var obj1, obj2;

#define FIX(up, down)                                               \
    if (TYPE_LIST == o->up.type) {                                  \
        FOR_EACH(obj1, o->up, i1, c1) {                             \
            FOR_EACH(obj2, objects[obj1.v.obj]->down, i2, c2)       \
            if (obj2.v.obj == old)                                  \
                break;                                              \
            objects[obj1.v.obj]->down[i2].v.obj = _new;             \
        }                                                           \
    }                                                               \
    else if (TYPE_OBJ == o->up.type && NOTHING != o->up.v.obj) {    \
        FOR_EACH(obj1, objects[o->up.v.obj]->down, i2, c2)          \
        if (obj1.v.obj == old)                                      \
            break;                                                  \
        objects[o->up.v.obj]->down[i2].v.obj = _new;                \
    }                                                               \
    FOR_EACH(obj1, o->down, i1, c1) {                               \
        if (TYPE_LIST == objects[obj1.v.obj]->up.type) {            \
            FOR_EACH(obj2, objects[obj1.v.obj]->up, i2, c2)         \
            if (obj2.v.obj == old)                                  \
                break;                                              \
            objects[obj1.v.obj]->up[i2].v.obj = _new;               \
        }                                                           \
        else {                                                      \
            objects[obj1.v.obj]->up.v.obj = _new;                   \
        }                                                           \
    }

FIX(parents, children);
FIX(location, contents);

#undef FIX

            /* Fix up the list of users, if necessary */
            if (is_user(_new)) {
                int i;

                for (i = 1; i <= all_users.length(); i++)
                    if (all_users[i].v.obj == old) {
                        all_users[i].v.obj = _new;
                        break;
                    }
            }
            /* Fix the owners of verbs, properties and objects */
            {
                Objid oid;

                for (oid = 0; oid < num_objects; oid++) {
                    Object *o = dbpriv_find_object(oid);
                    Verbdef *v;
                    Pval *p;
                    int i, count;

                    if (!o)
                        continue;

                    if (o->owner == _new)
                        o->owner = NOTHING;
                    else if (o->owner == old)
                        o->owner = _new;

                    for (v = o->verbdefs; v; v = v->next)
                        if (v->owner == _new)
                            v->owner = NOTHING;
                        else if (v->owner == old)
                            v->owner = _new;

                    p = o->propval;
                    count = o->nval;
                    for (i = 0; i < count; i++)
                        if (p[i].owner == _new)
                            p[i].owner = NOTHING;
                        else if (p[i].owner == old)
                            p[i].owner = _new;
                }
            }

            return _new;
        }
    }

    /* There are no recycled objects less than `old', so keep its number. */
    return old;
}

int
db_object_bytes(Var obj)
{
    Object *o = dbpriv_dereference(obj);
    int i, len, count;
    Verbdef *v;

    count = sizeof(Object) + sizeof(Object *);
    count += memo_strlen(o->name) + 1;

    for (v = o->verbdefs; v; v = v->next) {
        count += sizeof(Verbdef);
        count += memo_strlen(v->name) + 1;
        if (v->program)
            count += program_bytes(v->program);
    }

    count += sizeof(Propdef) * o->propdefs.cur_length;
    for (i = 0; i < o->propdefs.cur_length; i++)
        count += memo_strlen(o->propdefs.l[i].name) + 1;

    len = o->nval;
    count += (sizeof(Pval) - sizeof(Var)) * len;
    for (i = 0; i < len; i++)
        count += value_bytes(o->propval[i].var);

    return count;
}

/* Traverse the tree/graph twice.  First to count the maximal number
 * of members, and then to copy the members.  Use the bit array to
 * mark objects that have been copied, to prevent double copies.
 * Note: the final step forcibly sets the length of the list, which
 * may be less than the allocated length.  It's possible to calculate
 * the correct length, but that would require one more round of bit
 * array bookkeeping.
 */
/* The following implementations depend on the fact that an object
 * cannot appear in its own ancestors, descendants, location/contents
 * hierarchy.  It also depends on the fact that an anonymous object
 * cannot appear in any other object's hierarchies.  Consequently,
 * we don't set a bit for the root object (which may not have an id).
 */

#define ARRAY_SIZE_IN_BYTES (array_size / 8)
#define CLEAR_BIT_ARRAY() memset(bit_array, 0, ARRAY_SIZE_IN_BYTES)

static int
db1_count_ancestors(Object* o)
{
    Objid oid;
    Object* o2;

    Var tmp, parents = enlist_var(var_ref(o->parents));

    int i, c, n = 0;
    FOR_EACH(tmp, parents, i, c) {
        if (valid(oid = tmp.v.obj)) {
            if (bit_is_false(bit_array, oid)) {
                bit_true(bit_array, oid);
                o2 = dbpriv_find_object(oid);
                n += db1_count_ancestors(o2) + 1;
            }
        }
    }

    free_var(parents);
    return n;
}

static void
db2_add_ancestors(Object* o, Var* plist, int* px)
{
    Objid oid;
    Object* o2;
  
    Var tmp, parents = enlist_var(var_ref(o->parents));
    
    int i, c;
    FOR_EACH(tmp, parents, i, c) {
        if (valid(oid = tmp.v.obj)) {
            if (bit_is_false(bit_array, oid)) {
                bit_true(bit_array, oid);
                ++(*px);
                (*plist)[*px] = Var::new_obj(oid);
                o2 = dbpriv_find_object(oid);
                db2_add_ancestors(o2, plist, px);
            }
        }
    }

    free_var(parents);
}

Var
db_ancestors_old(Var obj, bool full)
{
    Var list;
    Object* o = dbpriv_dereference(obj);

    int n, i = 0;
    if ((o->parents.type == TYPE_OBJ && o->parents.v.obj == NOTHING) || (o->parents.type == TYPE_LIST && listlength(o->parents) == 0))
        return full ? enlist_var(var_ref(obj)) : new_list(0);
    
    CLEAR_BIT_ARRAY();
    
    n = db1_count_ancestors(o) + (full ? 1 : 0);
    list = new_list(n);
    if (full)
        list[++i] = var_ref(obj);
    
    CLEAR_BIT_ARRAY();
    
    db2_add_ancestors(o, &list, &i);
    list[0].v.num = i;

    return list;
};

static inline Var 
dbpriv_contents(Var v)
{
    Object *o = dbpriv_find_object(v.obj());
    return o != nullptr ? var_ref(dbpriv_object_contents(o)) : new_list(0);
}

static inline Var
dbpriv_children(Var v)
{
    Object *o = dbpriv_find_object(v.obj());

    if(o == nullptr) return new_list(0);

    if(o->children.type != TYPE_LIST)
        o->children = new_list(0);

    return o->children;
}

static inline Var 
dbpriv_parents(Object *o)
{
    if(o == nullptr || !is_valid(Var::new_obj(o->id)))
        return new_list(0);

    if(o->parents.type == TYPE_LIST)
        return o->parents;

    o->parents = (o->parents.type == TYPE_OBJ && o->parents.v.obj != -1) ? enlist_var(o->parents) : new_list(0);
    return dbpriv_parents(o);
}

static inline Var
db_all_ancestors(Var o) 
{
    using queue = std::queue<Objid>;

    if(o.type != TYPE_OBJ || !valid(o))
        return new_list(0);

    queue q;
    q.push(o.obj());

    Var parents, ancestors = enlist_var(o);

    while(!q.empty()) {        
        parents = var_ref(dbpriv_parents(dbpriv_find_object(q.front())));

        if(parents.length() > 0) {
            listforeach(parents, [&q](Var value, int index) -> int {
                if(value.obj() != -1) q.push(value.obj());
                return 0;
            });

            free_var(parents);
        }

        ancestors = setadd(ancestors, Var::new_obj(q.front()));
        q.pop();
    }

    return ancestors;
}

#ifdef USE_ANCESTOR_CACHE

static inline Var db_all_ancestors_cached(Var obj) {
    if(ancestor_cache.v.map == nullptr)
        ancestor_cache = new_map(0);

    Object *o = dbpriv_dereference(obj);
    Var ancestors, parents = var_dup(o->parents);

    if(!maphaskey(ancestor_cache, parents)) {
        ancestors      = db_all_ancestors(obj);
        ancestors[1]   = Var::new_obj(NOTHING);
        ancestor_cache = mapinsert(ancestor_cache, parents, ancestors);
    }

    if(maplookup(ancestor_cache, parents, &ancestors, 0) == nullptr) {
        ancestors = new_list(0);
        free_var(parents);
    } else {
        ancestors    = var_ref(ancestors);
        ancestors[1] = obj;
    }

    return ancestors;
}

#endif

Var 
db_all_contents(Var o, Var parent)
{
    if(!valid(o.obj())) return new_list(0);

    Var r = dbpriv_contents(o);
    for(auto i=1; i<=r.length(); i++)
        r = listconcat(r, dbpriv_contents(r[i]));

    return r;
}

Var
db_descendants(Var obj, bool full)
{
    Var r = dbpriv_children(obj);
    for (auto i = 1; i <= r.length(); i++)
        r = listconcat(var_ref(r), var_ref(dbpriv_children(r[i])));

    return full ? listappend(r, obj) : r;
}

Var 
db_all_locations(Var obj, bool full) {
    Objid oid = obj.obj();
    Object *o;

    Var locations = new_list(0);
    while((o = dbpriv_find_object(oid)) != nullptr && valid(o->location.obj()) && (oid = o->location.obj()))
        locations = listappend(locations, Var::new_obj(o->location.obj()));

    return locations;
}

Var 
db_ancestors(Var obj, bool full)
{
    if(obj.obj() > max_objects || obj.obj() <= memento_count || !is_valid(obj))
        return new_list(0);

#ifdef USE_ANCESTOR_CACHE
    Var ancestors = db_all_ancestors_cached(obj);    
#else
    Var ancestors = db_all_ancestors(obj);
#endif

    if(!full) {
        if(ancestors.length() > 1)
            ancestors = sublist(ancestors, 2, ancestors.length());
        else {
            free_var(ancestors);
            ancestors = new_list(0);
        }
    }

    return ancestors;
}

/*********** Object attributes ***********/

Objid
db_object_owner2(Var obj)
{
    return (TYPE_ANON == obj.type) ?
           dbpriv_object_owner(obj.v.anon) :
           db_object_owner(obj.v.obj);
}

Var
db_object_parents2(Var obj)
{
    return (TYPE_ANON == obj.type) ?
           dbpriv_object_parents(obj.v.anon) :
           db_object_parents(obj.v.obj);
}

Var
db_object_children2(Var obj)
{
    return (TYPE_ANON == obj.type) ?
           dbpriv_object_children(obj.v.anon) :
           db_object_children(obj.v.obj);
}

int
db_object_has_flag2(Var obj, db_object_flag f)
{
    return (TYPE_ANON == obj.type) ?
           dbpriv_object_has_flag(obj.v.anon, f) :
           db_object_has_flag(obj.v.obj, f);
}

void
db_set_object_flag2(Var obj, db_object_flag f)
{
    (TYPE_ANON == obj.type) ?
    dbpriv_set_object_flag(obj.v.anon, f) :
    db_set_object_flag(obj.v.obj, f);
}

void
db_clear_object_flag2(Var obj, db_object_flag f)
{
    (TYPE_ANON == obj.type) ?
    dbpriv_clear_object_flag(obj.v.anon, f) :
    db_clear_object_flag(obj.v.obj, f);
}

Objid
dbpriv_object_owner(Object *o)
{
    return o->owner;
}

void
dbpriv_set_object_owner(Object *o, Objid owner)
{
    o->owner = owner;
}

Objid
db_object_owner(Objid oid)
{
    return dbpriv_object_owner(dbpriv_find_object(oid));
}

void
db_set_object_owner(Objid oid, Objid owner)
{
    dbpriv_set_object_owner(dbpriv_find_object(oid), owner);
}

const char *
dbpriv_object_name(Object *o)
{
    return o->name;
}

void
dbpriv_set_object_name(Object *o, const char *name)
{
    if (o->name)
        free_str(o->name);
    o->name = name;
}

const char *
db_object_name(Objid oid)
{
    return dbpriv_object_name(dbpriv_find_object(oid));
}

void
db_set_object_name(Objid oid, const char *name)
{
    dbpriv_set_object_name(dbpriv_find_object(oid), name);
}

Var
dbpriv_object_parents(Object *o)
{
    if(o->parents.type != TYPE_LIST)
        o->parents = enlist_var(o->parents);

    return o->parents;
}

Var
dbpriv_object_children(Object *o)
{
    return memento(o->id) ? new_list(0) : o->children;
}

Var
db_object_parents(Objid oid)
{
    return dbpriv_object_parents(dbpriv_find_object(oid));
}

Var
db_object_children(Objid oid)
{
    return dbpriv_object_children(dbpriv_find_object(oid));
}

int
db_count_children(Objid oid)
{
    return memento(oid) ? 0 : listlength(objects[oid]->children);
}

int
db_for_all_children(Objid oid, int (*func) (void *, Objid), void *data)
{
    int i, c = db_count_children(oid);

    for (i = 1; i <= c; i++)
        if (func(data, objects[oid]->children[i].v.obj))
            return 1;

    return 0;
}

static int
check_for_duplicates(Var list)
{
    int i, j, c;

    if (TYPE_LIST != list.type || (c = listlength(list)) < 1)
        return 1;

    for (i = 1; i <= c; i++)
        for (j = i + i; j <= c; j++)
            if (equality(list[i], list[j], 1))
                return 0;

    return 1;
}

static int
check_children_of_object(Var obj, Var anon_kids)
{
    Var kid;
    int i, c;

    if (TYPE_LIST != anon_kids.type || listlength(anon_kids) < 1)
        return 1;

    FOR_EACH (kid, anon_kids, i, c) {
        Var parents = db_object_parents2(kid);
        if (TYPE_ANON != kid.type
                || ((TYPE_OBJ == parents.type && !equality(obj, parents, 0))
                    || (TYPE_LIST == parents.type && !ismember(obj, parents, 0))))
            return 0;
    }

    return 1;
}

int
db_change_parents(Var obj, Var new_parents, Var anon_kids)
{
    if (!check_for_duplicates(new_parents))
        return 0;
    if (!check_for_duplicates(anon_kids))
        return 0;
    if (!check_children_of_object(obj, anon_kids))
        return 0;
    if (!dbpriv_check_properties_for_chparent(obj, new_parents, anon_kids))
        return 0;

    Object *o = dbpriv_dereference(obj);

    if (o->verbdefs == nullptr
            && listlength(o->children) == 0
            && (TYPE_LIST != anon_kids.type || listlength(anon_kids) == 0)) {
        /* Since this object has no children and no verbs, we know that it
           can't have had any part in affecting verb lookup, since we use first
           parent with verbs as a key in the verb lookup cache. */
        /* The "no kids" rule is necessary because potentially one of the
           children could have verbs on it--and that child could have cache
           entries for THIS object's parentage. */
        /* In any case, don't clear the cache. */
        ;
    } else {
        db_priv_affected_callable_verb_lookup();
    }

    Var old_parents = o->parents;

    /* save this; we need it later */
    Var old_ancestors = db_ancestors(obj, true);

    if (TYPE_OBJ == obj.type) {
        Var parent;
        int i, c;

        /* remove me/obj from my old parents' children */
        if (old_parents.type == TYPE_OBJ && old_parents.v.obj != NOTHING)
            objects[old_parents.v.obj]->children = setremove(objects[old_parents.v.obj]->children, obj);
        else if (old_parents.type == TYPE_LIST)
            FOR_EACH(parent, old_parents, i, c)
            if(valid(parent.v.obj)) objects[parent.v.obj]->children = setremove(objects[parent.v.obj]->children, obj);

        /* add me/obj to my new parents' children */
        if (new_parents.type == TYPE_OBJ && new_parents.v.obj != NOTHING)
            objects[new_parents.v.obj]->children = setadd(objects[new_parents.v.obj]->children, obj);
        else if (new_parents.type == TYPE_LIST)
            FOR_EACH(parent, new_parents, i, c)
            if(valid(parent.v.obj)) objects[parent.v.obj]->children = setadd(objects[parent.v.obj]->children, obj);
    }

    free_var(o->parents);
    o->parents = var_dup(new_parents);

    /* Nothing between this point and the completion of
     * `dbpriv_fix_properties_after_chparent' may call `anon_valid'
     * because `o' is currently invalid (the nonce is out of date and
     * the aforementioned call will fix that).
     */

#ifdef USE_ANCESTOR_CACHE
    /* Invalidate the cache for all descendants of the object that is changing parents. */
    if(obj.type == TYPE_OBJ)
        ancestor_cache_invalidate(obj);
#endif /* USE_ANCESTOR_CACHE */

    Var new_ancestors = db_ancestors(obj, true);

    dbpriv_fix_properties_after_chparent(obj, old_ancestors, new_ancestors, anon_kids);

    free_var(old_ancestors);
    free_var(new_ancestors);

    return 1;
}

Var
dbpriv_object_location(Object *o)
{
    return o->location;
}

Var
dbpriv_object_last_move(Object *o)
{
    return (!clear_last_move && (o->last_move.type != TYPE_MAP || !o->last_move.length())) ? unmoved : o->last_move;
}

Objid
db_object_location(Objid oid)
{
    return dbpriv_object_location(dbpriv_find_object(oid)).v.obj;
}

Var
dbpriv_object_contents(Object *o)
{
    return o->contents;
}

int
db_count_contents(Objid oid)
{
    return listlength(dbpriv_find_object(oid)->contents);
}

int
db_for_all_contents(Objid oid, int (*func) (void *, Objid), void *data)
{
    Object *o;
    if((o = dbpriv_find_object(oid)) == nullptr)
        return 0;

    int i, c = db_count_contents(oid);

    for (i = 1; i <= c; i++)
        if (func(data, dbpriv_find_object(oid)->contents[i].v.obj))
            return 1;

    return 0;
}

void
db_change_location(Objid oid, Objid new_loc, int position)
{
    static Var time_key = str_dup_to_var("time");
    static Var source_key = str_dup_to_var("source");

    Var me = Var::new_obj(oid);
    Object *o = dbpriv_find_object(oid);

    Objid old_loc = o->location.obj();
    Object *old_location = dbpriv_find_object(old_loc);
    Object *new_location = dbpriv_find_object(new_loc);

    if (old_location != nullptr && valid(old_location->id))
        old_location->contents = setremove(old_location->contents, var_dup(me));

    if (new_location != nullptr && valid(new_location->id)) {
        if (position <= 0)
            position = new_location->contents.length() + 1;

        new_location->contents = listinsert(new_location->contents, me, position);
    }

    free_var(o->location);
    o->location = Var::new_obj(new_loc);

    if (!clear_last_move) {
        if (o->last_move.type != TYPE_MAP) {
            free_var(o->last_move);
            o->last_move = new_map(0);
        }

        Var last_move = o->last_move;
        last_move = mapinsert(last_move, var_ref(time_key), Var::new_int(time(nullptr)));
        last_move = mapinsert(last_move, var_ref(source_key), Var::new_obj(old_loc));

        o->last_move = last_move;
    }
}

int
dbpriv_object_has_flag(Object *o, db_object_flag f)
{
    return (o->flags & (1 << f)) != 0;
}

void
dbpriv_set_object_flag(Object *o, db_object_flag f)
{
    o->flags |= (1 << f);
}

void
dbpriv_clear_object_flag(Object *o, db_object_flag f)
{
    o->flags &= ~(1 << f);
}

int
db_object_has_flag(Objid oid, db_object_flag f)
{
    return dbpriv_object_has_flag(dbpriv_find_object(oid), f);
}

void
db_set_object_flag(Objid oid, db_object_flag f)
{
    dbpriv_set_object_flag(dbpriv_find_object(oid), f);

    if (f == FLAG_USER)
        all_users = setadd(all_users, Var::new_obj(oid));
}

void
db_clear_object_flag(Objid oid, db_object_flag f)
{
    dbpriv_clear_object_flag(dbpriv_find_object(oid), f);
    if (f == FLAG_USER)
        all_users = setremove(all_users, Var::new_obj(oid));
}

int
db_object_allows(Var obj, Objid progr, db_object_flag f)
{
    return (is_wizard(progr)
            || progr == db_object_owner2(obj)
            || db_object_has_flag2(obj, f));
}

int
is_wizard(Objid oid)
{
    return valid(oid) && db_object_has_flag(oid, FLAG_WIZARD);
}

int
is_programmer(Objid oid)
{
    return valid(oid) && db_object_has_flag(oid, FLAG_PROGRAMMER);
}

int
is_user(Objid oid)
{
    return valid(oid) && db_object_has_flag(oid, FLAG_USER);
}

Var
db_all_users(void)
{
    return all_users;
}

void
dbpriv_set_all_users(Var v)
{
    all_users = v;
}

int
db_object_isa(Var object, Var parent)
{
    if (equality(object, parent, 0))
        return 1;

    Object *o, *t;

    o = (TYPE_OBJ == object.type) ?
        dbpriv_find_object(object.v.obj) :
        object.v.anon;

    Var ancestor, ancestors = enlist_var(var_ref(o->parents));

    while (listlength(ancestors) > 0) {
        POP_TOP(ancestor, ancestors);

        if (ancestor.v.obj == NOTHING)
            continue;

        if (equality(ancestor, parent, 0)) {
            free_var(ancestors);
            return 1;
        }

        t = dbpriv_find_object(ancestor.v.obj);

        ancestors = listconcat(ancestors, enlist_var(var_ref(t->parents)));
    }

    free_var(ancestors);
    return 0;
}

void
db_fixup_owners(const Objid obj)
{
    for (Objid oid = 0; oid < num_objects; oid++) {
        Object *o = dbpriv_find_object(oid);
        Pval *p;

        if (!o)
            continue;

        if (o->owner == obj)
            o->owner = NOTHING;

        for (Verbdef *v = o->verbdefs; v; v = v->next)
            if (v->owner == obj)
                v->owner = NOTHING;

        p = o->propval;
        for (int i = 0, count = o->nval; i < count; i++)
            if (p[i].owner == obj)
                p[i].owner = NOTHING;
    }
}

void
db_clear_ancestor_cache(void)
{
#ifdef USE_ANCESTOR_CACHE /*Just in case */
    free_var(ancestor_cache);
    ancestor_cache.v.map = nullptr;
#endif
}
