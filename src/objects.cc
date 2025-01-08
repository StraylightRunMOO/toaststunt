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

#include <vector>

#include "collection.h"
#include "db.h"
#include "db_private.h"
#include "db_io.h"
#include "execute.h"
#include "functions.h"
#include "list.h"
#include "map.h"
#include "numbers.h"
#include "quota.h"
#include "server.h"
#include "storage.h"
#include "structures.h"
#include "utils.h"
#include "log.h"
#include "background.h"   // Threads

static int
controls(Objid who, Objid what)
{
    return is_wizard(who) || who == db_object_owner(what);
}

static int
controls2(Objid who, Var what)
{
    return is_wizard(who) || who == db_object_owner2(what);
}

static Var
make_arglist(Objid what)
{
    Var r;

    r = new_list(1);
    r[1].type = TYPE_OBJ;
    r[1].v.obj = what;

    return r;
}

static bool
all_valid(Var vars)
{
    Var var;
    int i, c;
    FOR_EACH(var, vars, i, c)
    if (!valid(var.v.obj))
        return false;
    return true;
}

static bool
all_allowed(Var vars, Objid progr, db_object_flag f)
{
    Var var;
    int i, c;
    FOR_EACH(var, vars, i, c)
    if (!db_object_allows(var, progr, f))
        return false;
    return true;
}

/*
 * Returns true if `_this' is a descendant of `obj'.
 */
static bool
is_a_descendant(Var _this, Var obj)
{
    Var descendants = db_descendants(obj, true);
    int ret = ismember(_this, descendants, 1);
    free_var(descendants);
    return ret ? true : false;
}

/*
 * Returns true if any of `these' are descendants of `obj'.
 */
static bool
any_are_descendants(Var these, Var obj)
{
    Var _this, descendants = db_descendants(obj, true);
    int i, c;
    FOR_EACH(_this, these, i, c) {
        if (is_a_descendant(_this, obj)) {
            free_var(descendants);
            return true;
        }
    }
    free_var(descendants);
    return false;
}

struct bf_move_data {
    Objid what, where;
    int position;
};

static package
do_move(Var arglist, Byte next, struct bf_move_data *data, Objid progr)
{
    Objid what = data->what, where = data->where;
    int position = data->position, accepts;
    Objid oid, oldloc = FAILED_MATCH;
    Var args;
    enum error e;

    switch (next) {
        case 1:         /* Check validity and decide `accepts' */
            if (!valid(what) || (!valid(where) && where != NOTHING) || position < 0)
                return make_error_pack(E_INVARG);
            else if (!controls(progr, what))
                return make_error_pack(E_PERM);
            else if (where == NOTHING || where == db_object_location(what))
                accepts = 1;
            else if (data->position > 0 && !controls(progr, where))
                return make_error_pack(E_PERM);
            else {
                args = make_arglist(what);
                e = call_verb(where, "accept", Var::new_obj(where), args, 0);
                /* e will not be E_INVIND */

                if (e == E_NONE)
                    return make_call_pack(2, data);
                else {
                    free_var(args);
                    if (e == E_VERBNF)
                        accepts = 0;

                    else        /* (e == E_MAXREC) */
                        return make_error_pack(e);
                }
            }
            goto accepts_decided;

        case 2:         /* Returned from `accepts' call */
            accepts = is_true(arglist);

accepts_decided:
            if (!is_wizard(progr) && accepts == 0)
                return make_error_pack(E_NACC);

            if (!valid(what)
                    || (where != NOTHING && !valid(where))
                    || (db_object_location(what) == where && position == 0))
                return no_var_pack();

            /* Check to see that we're not trying to violate the hierarchy */
            for (oid = where; oid != NOTHING; oid = db_object_location(oid))
                if (oid == what)
                    return make_error_pack(E_RECMOVE);

            oldloc = db_object_location(what);
            db_change_location(what, where, position);

            if (where != oldloc) {
                args = make_arglist(what);
                e = call_verb(oldloc, "exitfunc", Var::new_obj(oldloc), args, 0);

                if (e == E_NONE)
                    return make_call_pack(3, data);
                else {
                    free_var(args);
                    if (e == E_MAXREC)
                        return make_error_pack(e);
                }
                /* e == E_INVIND or E_VERBNF, in both cases fall through */
            }

        case 3:         /* Returned from exitfunc call */
            if (valid(where) && valid(what)
                    && where != oldloc && db_object_location(what) == where) {
                args = make_arglist(what);
                e = call_verb(where, "enterfunc", Var::new_obj(where), args, 0);
                /* e != E_INVIND */

                if (e == E_NONE)
                    return make_call_pack(4, data);
                else {
                    free_var(args);
                    if (e == E_MAXREC)
                        return make_error_pack(e);
                    /* else e == E_VERBNF, fall through */
                }
            }
        case 4:         /* Returned from enterfunc call */
            return no_var_pack();

        default:
            panic_moo("Unknown PC in DO_MOVE");
            return no_var_pack();   /* Dead code to eliminate compiler warning */
    }
}

static package
bf_move(Var arglist, Byte next, void *vdata, Objid progr)
{
    struct bf_move_data *data = (bf_move_data *)vdata;
    package p;

    if (next == 1) {
        data = (bf_move_data *)alloc_data(sizeof(*data));
        data->what = arglist[1].v.obj;
        data->where = arglist[2].v.obj;
        data->position = arglist.length() < 3 ? 0 : arglist[3].v.num;
    }
    p = do_move(arglist, next, data, progr);
    free_var(arglist);

    if (p.kind != package::BI_CALL)
        free_data(data);

    return p;
}

static void
bf_move_write(void *vdata)
{
    struct bf_move_data *data = (bf_move_data *)vdata;

    dbio_printf("bf_move data: what = %" PRIdN ", where = %" PRIdN ", position = %" PRIdN "\n",
                data->what, data->where, data->position);
}

static void *
bf_move_read()
{
    struct bf_move_data *data = (bf_move_data *)alloc_data(sizeof(*data));

    if (dbio_scanf("bf_move data: what = %" PRIdN ", where = %" PRIdN ", position = %" PRIdN "\n",
                   &data->what, &data->where, &data->position) == 3)
        return data;
    else
        return nullptr;
}

static package
bf_toobj(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    Num i;
    enum error e;

    r.type = TYPE_OBJ;
    e = become_integer(arglist[1], &i, 0);
    r.v.obj = i;

    free_var(arglist);
    if (e != E_NONE)
        return make_error_pack(e);

    return make_var_pack(r);
}

static package
bf_typeof(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    r.type = _TYPE_TYPE;
    r.v.num = (int) arglist[1].type & TYPE_DB_MASK;
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_valid(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (object) */
    Var r;

    if (arglist[1].is_object()) {
        r.type = TYPE_INT;
        r.v.num = is_valid(arglist[1]);
    }
    else {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }

    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_max_object(Var arglist, Byte next, void *vdata, Objid progr)
{   /* () */
    Var r;

    free_var(arglist);
    r.type = TYPE_OBJ;
    r.v.obj = db_last_used_objid();
    return make_var_pack(r);
}


struct create_args {
    Var parents;
    Objid owner;
    bool memento;
    Var init;
    enum error err;
};

static struct create_args 
parse_create_args(Var arglist, Objid progr)
{
    auto nargs = arglist.length();

    struct create_args args;
    args.owner = progr;
    args.memento = false;
    args.err = E_NONE;

    if (!is_obj_or_list_of_objs(arglist[1])) {
        args.err = E_TYPE;
        return args;
    } else {
        if(arglist[1].type == TYPE_OBJ)
            args.parents = enlist_var(arglist[1]);
        else
            args.parents = arglist[1];
    }

    if (nargs >= 2 && arglist[2].type == TYPE_OBJ)
        args.owner = arglist[2].v.obj; 
    else if (nargs >= 2 && arglist[2].type == TYPE_INT)
        args.memento = arglist[2].num() > 0;
    else if (nargs >= 2 && arglist[2].type == TYPE_LIST)
        args.init = arglist[2];
    else if (nargs >= 2) {
        args.err = E_TYPE;
        return args;
    }

    if (nargs >= 3 && arglist[3].type == TYPE_INT && !args.memento)
        args.memento = arglist[3].num() > 0;
    else if (nargs >= 2 && arglist[3].type == TYPE_LIST && args.init.type != TYPE_LIST)
        args.init = arglist[3];
    else if (nargs >= 3) {
        args.err = E_TYPE;
        return args;
    }

    if (nargs >= 4 && arglist[4].type == TYPE_INT && !args.memento)
        args.memento = arglist[4].num() > 0;
    else if (nargs >= 4 && arglist[4].type == TYPE_LIST && args.init.type != TYPE_LIST)
        args.init = arglist[4];
    else if (nargs >= 4) {
        args.err = E_TYPE;
        return args;
    }

    if(args.init.type != TYPE_LIST)
        args.init = new_list(0);

    return args;
}

static package
bf_create(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (OBJ|LIST parent(s) [, OBJ owner] [, INT anonymous] [, LIST args]) */
    Var *data = (Var *)vdata;
    Var r;

    if (next == 1) {
        struct create_args args = parse_create_args(arglist, progr);

        if(args.err != E_NONE) {
            free_var(arglist);
            return make_error_pack(E_TYPE);
        }

        if ((args.memento && args.owner == NOTHING) || (!valid(args.owner) && args.owner != NOTHING) || !all_valid(args.parents)) {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        } else if ((args.owner != progr && !is_wizard(progr)) || !all_allowed(args.parents, progr, FLAG_FERTILE)) {
            free_var(arglist);
            return make_error_pack(E_PERM);
        }

        if (valid(args.owner) && !decr_quota(args.owner)) {
            free_var(arglist);
            return make_error_pack(E_QUOTA);
        } else {
            enum error e;
            if(!args.memento) {
                Objid last = db_last_used_objid();
                Objid oid = db_create_object(-1);

                db_set_object_owner(oid, !valid(args.owner) ? oid : args.owner);

                if (!db_change_parents(Var::new_obj(oid), args.parents, none)) {
                    db_destroy_object(oid);
                    db_set_last_used_objid(last);
                    free_var(arglist);
                    return make_error_pack(E_INVARG);
                }

                r = Var::new_obj(oid);
            } else {
                r = Var::new_obj(create_memento_object(args.parents, args.owner));
                if(r.obj() == NOTHING) {
                    free_var(arglist);
                    return make_error_pack(E_INVARG);
                }
            }

            data = (Var *)alloc_data(sizeof(Var));
            *data = var_ref(r);

            Var init = var_ref(args.init);
            free_var(arglist);

            e = call_verb(r.obj(), "initialize", r, init, 0);
            /* e will not be E_INVIND */

            if (e == E_NONE) {
                free_var(r);
                return make_call_pack(2, data);
            }

            free_var(*data);
            free_data(data);
            free_var(init);

            if (e == E_MAXREC) {
                free_var(r);
                return make_error_pack(e);
            } else      /* (e == E_VERBNF) do nothing */
                return make_var_pack(r);
        }
    } else {            /* next == 2, returns from initialize verb_call */
        r = var_ref(*data);
        free_var(*data);
        free_data(data);
        return make_var_pack(r);
    }
}

/* Recreate a destroyed object. Mostly a duplicate of bf_create, unfortunately. */
static package
bf_recreate(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (OBJ old_object, OBJ parent [, OBJ owner]) */
    Var *data = (Var *)vdata;
    Var r;

    if (next == 1) {
        if (arglist[1].v.obj <= 0 || arglist[1].v.obj > db_last_used_objid() || is_valid(arglist[1])) {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }

        Objid owner = progr;
        if (arglist.length() > 2 && arglist[3].type == TYPE_OBJ && is_valid(arglist[3]))
            owner = arglist[3].v.obj;

        if ((progr != owner && !is_wizard(progr))
                || (arglist[2].type == TYPE_OBJ
                    && valid(arglist[2].v.obj)
                    && !db_object_allows(arglist[2], progr, FLAG_FERTILE))) {
            free_var(arglist);
            return make_error_pack(E_PERM);
        }

        if (valid(owner) && !decr_quota(owner)) {
            free_var(arglist);
            return make_error_pack(E_QUOTA);
        }
        else {
            enum error e;
            Objid oid = db_create_object(arglist[1].v.obj);

            db_set_object_owner(oid, !valid(owner) ? oid : owner);

            if (!db_change_parents(Var::new_obj(oid), arglist[2], none)) {
                db_destroy_object(oid);
                free_var(arglist);
                return make_error_pack(E_INVARG);
            }

            free_var(arglist);

            r.type = TYPE_OBJ;
            r.v.obj = oid;

            data = (Var *)alloc_data(sizeof(Var));
            *data = var_ref(r);

            e = call_verb(oid, "initialize", r, new_list(0), 0);
            /* e will not be E_INVIND */

            if (e == E_NONE) {
                free_var(r);
                return make_call_pack(2, data);
            }

            free_var(*data);
            free_data(data);

            if (e == E_MAXREC) {
                free_var(r);
                return make_error_pack(e);
            } else      /* (e == E_VERBNF) do nothing */
                return make_var_pack(r);
        }
    } else {            /* next == 2, returns from initialize verb_call */
        r = var_ref(*data);
        free_var(*data);
        free_data(data);
        return make_var_pack(r);
    }
}

static void
bf_create_write(void *vdata)
{
    dbio_printf("bf_create data: oid = %" PRIdN "\n", *((Objid *) vdata));
}

static void *
bf_create_read(void)
{
    Objid *data = (Objid *)alloc_data(sizeof(Objid));

    if (dbio_scanf("bf_create data: oid = %" PRIdN "\n", data) == 1)
        return data;
    else
        return nullptr;
}

static package
bf_chparents(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (OBJ obj, OBJ|LIST what, LIST anon) */
    int nargs = listlength(arglist);
    Var obj = arglist[1];
    Var what = (arglist[2].type == TYPE_OBJ) ? enlist_var(arglist[2]) : arglist[2];

    if(!is_list_of_objs(what)) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    } else if (!all_valid(what) && (what.length() != 1 || what[1].obj() != -1)) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    } else if ((nargs > 2 && !is_wizard(progr)) || !controls2(progr, obj) || !all_allowed(what, progr, FLAG_FERTILE)) {
        free_var(arglist);
        return make_error_pack(E_PERM);
    } else if (any_are_descendants(what, obj)) {
        free_var(arglist);
        return make_error_pack(E_RECMOVE);
    } else if (!db_change_parents(obj, what, (nargs > 2) ? arglist[3] : nothing)) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }

    free_var(arglist);    
    return no_var_pack();
}

static package
bf_parents(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (OBJ object) */
    Var r;

    if (!arglist[1].is_object()) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }  else if (!is_valid(arglist[1])) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    } else {
        r = var_ref(db_object_parents2(arglist[1]));
        free_var(arglist);
    }

    if (TYPE_LIST == r.type)
        return make_var_pack(r);

    if (NOTHING == r.v.obj) {
        free_var(r);
        return make_var_pack(new_list(0));
    } else {
        Var t = new_list(1);
        t[1] = r;
        return make_var_pack(t);
    }
}

static package
bf_children(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (object) */
    Var obj = arglist[1];

    if (!obj.is_object()) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    } else if (!is_valid(obj)) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    } else {
        Var r = var_ref(db_object_children2(obj));
        free_var(arglist);
        return make_var_pack(r);
    }
}

static package
bf_ancestors(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (OBJ object) */
    Var obj = arglist[1];
    bool full = (listlength(arglist) > 1 && is_true(arglist[2])) ? true : false;

    if (!obj.is_object()) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    } else if (!is_valid(obj)) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    } else {
        Var r = db_ancestors(obj, full);
        free_var(arglist);
        return make_var_pack(r);
    }
}

static package
bf_descendants(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (OBJ object) */
    Var obj = arglist[1];
    bool full = (listlength(arglist) > 1 && is_true(arglist[2])) ? true : false;

    if (!obj.is_object()) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    } else if (!is_valid(obj)) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }

    Var r = db_descendants(obj, full);
    free_var(arglist);
    return make_var_pack(r);
}

static int
move_to_nothing(Objid oid)
{
    /* All we need to do is change the location and run the exitfunc. */
    Objid oldloc = db_object_location(oid);
    Var args;
    enum error e;

    db_change_location(oid, NOTHING, 0);

    args = make_arglist(oid);
    e = call_verb(oldloc, "exitfunc", Var::new_obj(oldloc), args, 0);

    if (e == E_NONE)
        return 1;

    free_var(args);
    return 0;
}

static int
first_proc(void *data, Objid oid)
{
    Objid *oidp = (Objid *)data;

    *oidp = oid;
    return 1;
}

static Objid
get_first(Objid oid, int (*for_all) (Objid, int (*)(void *, Objid), void *))
{
    Objid result = NOTHING;

    for_all(oid, first_proc, &result);

    return result;
}

static package
bf_recycle(Var arglist, Byte func_pc, void *vdata, Objid progr)
{   /* (OBJ|ANON object) */
    Var *data = (Var *)vdata;
    enum error e;
    Var obj;
    Var args;

    switch (func_pc) {
        case 1:
            obj = var_ref(arglist[1]);
            free_var(arglist);

            if (!obj.is_object()) {
                free_var(obj);
                return make_error_pack(E_TYPE);
            } else if (!is_valid(obj) || (TYPE_ANON == obj.type && dbpriv_object_has_flag(obj.v.anon, FLAG_RECYCLED))) {
                free_var(obj);
                return make_error_pack(E_INVARG);
            } else if (!controls2(progr, obj)) {
                free_var(obj);
                return make_error_pack(E_PERM);
            }

            if (TYPE_ANON == obj.type)
                dbpriv_set_object_flag(obj.v.anon, FLAG_RECYCLED);

            /* Recycle permanent and anonymous objects.
             *
             * At this point in time, an anonymous object may be in the
             * root buffer and may be any color (purple, if the last
             * operation was a decrement, black, if the last operation was
             * an increment) (it _will_ have a reference, however -- a
             * reference to itself, at least).
             */

            data = (Var *)alloc_data(sizeof(Var));
            *data = var_ref(obj);
            args = new_list(0);
            e = call_verb(obj.is_obj() ? obj.v.obj : NOTHING, "recycle", obj, args, 0);
            /* e != E_INVIND */

            if (e == E_NONE) {
                free_var(obj);
                return make_call_pack(2, data);
            }
            /* else e == E_VERBNF or E_MAXREC; fall through */

            free_var(args);

            goto moving_contents;

        case 2:
            obj = var_ref(*data);
            free_var(arglist);

moving_contents:

            if (!is_valid(obj)) {
                free_var(obj);
                free_var(*data);
                free_data(data);
                return make_error_pack(E_INVARG);
            }

            if (TYPE_OBJ == obj.type) {
                Objid c, oid = obj.v.obj;

                while ((c = get_first(oid, db_for_all_contents)) != NOTHING)
                    if (move_to_nothing(c))
                        return make_call_pack(2, data);

                if (db_object_location(oid) != NOTHING && move_to_nothing(oid))
                    /* Return to same case because this :exitfunc might add new */
                    /* contents to OID or even move OID right back in. */
                    return make_call_pack(2, data);

                /* we can now be confident that OID has no contents and no location */

                /* do the same thing for the inheritance hierarchy */
                while ((c = get_first(oid, db_for_all_children)) != NOTHING) {
                    Var cp = db_object_parents(c);
                    Var op = db_object_parents(oid);
                    if (cp.is_obj()) {
                        db_change_parents(Var::new_obj(c), op, none);
                    }
                    else {
                        int i = 1;
                        int j = 1;
                        Var _new = new_list(0);
                        while (i <= cp.length() && cp[i].v.obj != oid) {
                            _new = setadd(_new, var_ref(cp[i]));
                            i++;
                        }
                        if (op.is_obj()) {
                            if (valid(op.v.obj))
                                _new = setadd(_new, var_ref(op));
                        }
                        else {
                            while (j <= op.length()) {
                                _new = setadd(_new, var_ref(op[j]));
                                j++;
                            }
                        }
                        i++;
                        while (i <= cp.length()) {
                            _new = setadd(_new, var_ref(cp[i]));
                            i++;
                        }
                        db_change_parents(Var::new_obj(c), _new, none);
                        free_var(_new);
                    }
                }

                db_change_parents(obj, nothing, none);

#ifdef SAFE_RECYCLE
                db_fixup_owners(obj.v.obj);
#endif

                incr_quota(db_object_owner(oid));

                db_destroy_object(oid);

                free_var(obj);
                free_var(*data);
                free_data(data);
                return no_var_pack();
            }
            else if (TYPE_ANON == obj.type) {
                /* We'd like to run `db_change_parents()' to be consistent
                 * with the pattern laid out for permanent objects, but we
                 * can't because the object can be invalid at this point
                 * due to changes in parentage.
                 */
                /*db_change_parents(obj, nothing, none);*/

                incr_quota(db_object_owner2(obj));

                db_destroy_anonymous_object(obj.v.anon);

                free_var(obj);
                free_var(*data);
                free_data(data);
                return no_var_pack();
            }
    }

    panic_moo("Can't happen in bf_recycle");
    return no_var_pack();
}

static void
bf_recycle_write(void *vdata)
{
    Objid *data = (Objid *)vdata;

    dbio_printf("bf_recycle data: oid = %" PRIdN ", cont = 0\n", *data);
}

static void *
bf_recycle_read(void)
{
    Objid *data = (Objid *)alloc_data(sizeof(*data));
    int dummy;

    /* I use a `dummy' variable here and elsewhere instead of the `*'
     * assignment-suppression syntax of `scanf' because it allows more
     * straightforward error checking; unfortunately, the standard says that
     * suppressed assignments are not counted in determining the returned value
     * of `scanf'...
     */
    if (dbio_scanf("bf_recycle data: oid = %" PRIdN ", cont = %" PRIdN "\n",
                   data, &dummy) == 2)
        return data;
    else
        return nullptr;
}

static package
bf_players(Var arglist, Byte next, void *vdata, Objid progr)
{   /* () */
    free_var(arglist);
    return make_var_pack(var_ref(db_all_users()));
}

static package
bf_is_player(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (object) */
    Var r;
    Objid oid = arglist[1].v.obj;

    free_var(arglist);

    if (!valid(oid))
        return make_error_pack(E_INVARG);

    r.type = TYPE_INT;
    r.v.num = is_user(oid);
    return make_var_pack(r);
}

static package
bf_set_player_flag(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (object, yes/no) */
    Var obj;
    char flag;

    obj = arglist[1];
    flag = is_true(arglist[2]);

    free_var(arglist);

    if (!valid(obj.v.obj))
        return make_error_pack(E_INVARG);
    else if (!is_wizard(progr))
        return make_error_pack(E_PERM);

    if (flag) {
        db_set_object_flag(obj.v.obj, FLAG_USER);
    } else {
        boot_player(obj.v.obj);
        db_clear_object_flag(obj.v.obj, FLAG_USER);
    }
    return no_var_pack();
}

static package
bf_object_bytes(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var obj = arglist[1];

    if (!obj.is_object()) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    } else if (!is_valid(obj)) {
        free_var(arglist);
        return make_error_pack(E_INVIND);
    } else if (!is_wizard(progr)) {
        free_var(arglist);
        return make_error_pack(E_PERM);
    } else {
        Var v;

        v.type = TYPE_INT;
        v.v.num = db_object_bytes(obj);

        free_var(arglist);

        return make_var_pack(v);
    }
}

static package
bf_isa(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (object, parent, return_object) */
    Var object = arglist[1];
    Var parent = arglist[2];
    bool return_obj = (arglist.length() > 2 && is_true(arglist[3]));

    if (!object.is_object() || (!is_obj_or_list_of_objs(parent) && parent.type != TYPE_ANON)) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    }

    package ret = make_var_pack(return_obj ? Var::new_obj(NOTHING) : Var::new_int(0));

    if (object.type == TYPE_WAIF)
        object = Var::new_obj(object.v.waif->_class);

    if (!is_valid(object)) {
        // Do nothing
    } else if (parent.type == TYPE_LIST) {
        int parent_length = parent.length();
        for (int x = 1; x <= parent_length; x++) {
            if (db_object_isa(object, parent[x])) {
                ret = make_var_pack(return_obj ? Var::new_obj(parent[x].v.obj) : Var::new_int(1));
                break;
            }
        }
    } else if (db_object_isa(object, parent)) {
        ret = make_var_pack(return_obj ? Var::new_obj(parent.v.obj) : Var::new_int(1));
    }

    free_var(arglist);
    return ret;
}

/* Locate an object in the database by name more quickly than is possible in-DB.
 * To avoid numerous list reallocations, we put everything in a vector and then
 * transfer it over to a list when we know how many values we have. */
void locate_by_name_thread_callback(Var arglist, Var *ret, void *extra_data)
{
    Var name, object;
    object.type = TYPE_OBJ;
    std::vector<int> tmp;

    const int case_matters = arglist.length() < 2 ? 0 : is_true(arglist[2]);
    const int string_length = memo_strlen(arglist[1].v.str);

    const Objid last_objid = db_last_used_objid();
    for (int x = 0; x <= last_objid; x++)
    {
        if (!valid(x))
            continue;

        object.v.obj = x;
        db_find_property(object, "name", &name);
        if (strindex(name.v.str, memo_strlen(name.v.str), arglist[1].v.str, string_length, case_matters))
            tmp.push_back(x);
    }

    *ret = new_list(tmp.size());
    const auto vector_size = tmp.size();
    for (size_t x = 0; x < vector_size; x++) {
        (*ret)[x + 1] = Var::new_obj(tmp[x]);
    }
}

static package
bf_locate_by_name(Var arglist, Byte next, void *vdata, Objid progr)
{
    if (!is_wizard(progr))
    {
        free_var(arglist);
        return make_error_pack(E_PERM);
    }

    return background_thread(locate_by_name_thread_callback, &arglist);
}

static bool multi_parent_isa(Var *object, Var *parents)
{
    if (parents->type == TYPE_OBJ)
        return db_object_isa(*object, *parents);

    for (int y = 1; y <= (*parents).length(); y++)
        if (db_object_isa(*object, (*parents)[y]))
            return true;

    return false;
}

/* Return a list of objects of parent, optionally with a player flag set.
 * With only one argument, player flag is assumed to be the only condition.
 * With two arguments, parent is the only condition.
 * With three arguments, parent is checked first and then the player flag is checked.
 * With four arguments, the parent check is inversed; items that are not descended from <parent> are returned.
 * occupants(LIST objects, OBJ | LIST parent, ?INT player flag set, ?INT inverse match)
 */
static package
bf_occupants(Var arglist, Byte next, void *vdata, Objid progr)
{   /* (object) */
    Var ret = new_list(0);
    int nargs = arglist.length();
    Var contents = arglist[1];
    int content_length = contents.length();
    bool check_parent = nargs == 1 ? false : true;
    Var parent = check_parent ? arglist[2] : nothing;
    bool check_player_flag = (nargs == 1 || (nargs > 2 && is_true(arglist[3])));
    bool inverse_match = (nargs > 3 && is_true(arglist[4]));

    if (check_parent && !is_obj_or_list_of_objs(parent)) {
        free_var(arglist);
        return make_error_pack(E_TYPE);
    } else if (!is_list_of_objs(contents) || !all_valid(contents)) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    }
    
    for (int x = 1; x <= content_length; x++) {
        Objid oid = contents[x].v.obj;
        if ((!check_parent ? 1 : (inverse_match ? !multi_parent_isa(&contents[x], &parent) : multi_parent_isa(&contents[x], &parent)))
                && (!check_player_flag || (check_player_flag && is_user(oid))))
        {
            ret = setadd(ret, contents[x]);
        }
    }

    free_var(arglist);
    return make_var_pack(ret);
}

/* Return a list of nested locations for an object.
 * If base_object is specified, locations will stop at that object. Otherwise,
 *   stop at $nothing (#-1).
 * If check_parent is true, the base_object is assumed to be a parent and an
 *   isa() check is performed.
 * For objects in base_parent, this returns an empty list.
 * locations(OBJ object, ?base_object, ?check_parent)
 */
static package
bf_locations(Var arglist, Byte next, void *vdata, Objid progr)
{
    const Objid what = arglist[1].v.obj;
    const int nargs = arglist.length();
    const Objid base_obj = (nargs > 1 ? arglist[2].v.obj : 0);
    const Var base_obj_var = Var::new_obj(base_obj);
    const bool check_parent = (nargs > 2 ? is_true(arglist[3]) : false);

    free_var(arglist);

    if (!valid(what))
        return make_error_pack(E_INVIND);

    Var locs = new_list(0);

    Objid loc = db_object_location(what);

    while (valid(loc)) {
        const Var loc_var = Var::new_obj(loc);
        if (base_obj && (check_parent ? db_object_isa(loc_var, base_obj_var) : loc == base_obj))
            break;
        locs = setadd(locs, loc_var);
        loc = db_object_location(loc);
    }

    return make_var_pack(locs);
}

static package
bf_clear_ancestor_cache(Var arglist, Byte next, void *vdata, Objid progr)
{
    free_var(arglist);

    if (!is_wizard(progr))
        return make_error_pack(E_PERM);

    db_clear_ancestor_cache();
    return no_var_pack();
}

static package
bf_recycled_objects(Var arglist, Byte next, void *vdata, Objid progr)
{
    free_var(arglist);
    std::vector<Objid> tmp;
    Objid max_obj = db_last_used_objid();

    for (Objid x = 0; x <= max_obj; x++) {
        if (!valid(x))
            tmp.push_back(x);
    }

    Var ret = new_list(tmp.size());
    for (size_t x = 1; x <= tmp.size(); x++) {
        ret[x].type = TYPE_OBJ;
        ret[x].v.obj = tmp[x - 1];
    }

    return make_var_pack(ret);
}

static package
bf_next_recycled_object(Var arglist, Byte next, void *vdata, Objid progr)
{
    Objid i_obj = (arglist.length() == 1 ? arglist[1].v.obj : 0);
    Objid max_obj = db_last_used_objid();
    free_var(arglist);

    if (i_obj > max_obj || i_obj < 0)
        return make_error_pack(E_INVARG);

    package ret = make_var_pack(Var::new_int(0));

    for (; i_obj < max_obj; i_obj++) {
        if (!valid(i_obj)) {
            ret = make_var_pack(Var::new_obj(i_obj));
            break;
        }
    }

    return ret;
}

/* Return a list of all objects in the database owned by who. */
static package
bf_owned_objects(Var arglist, Byte next, void *vdata, Objid progr)
{
    Objid who = arglist[1].v.obj;
    free_var(arglist);

    if (!valid(who))
        return make_error_pack(E_INVIND);

    std::vector<Objid> tmp;
    Objid max_obj = db_last_used_objid();

    for (Objid x = 0; x <= max_obj; x++) {
        if (valid(x) && who == db_object_owner(x))
            tmp.push_back(x);
    }

    Var ret = new_list(tmp.size());
    for (size_t x = 1; x <= tmp.size(); x++) {
        ret[x].type = TYPE_OBJ;
        ret[x].v.obj = tmp[x - 1];
    }

    return make_var_pack(ret);
}

static inline Var object_contents(Var v) {
    Object *o = dbpriv_find_object(v.obj());
    return o != nullptr ? var_ref(dbpriv_object_contents(o)) : new_list(0);
}

static package
bf_all_contents(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var o = arglist[1];

    if(o.obj() == -1) {
        Var r = new_list(0);
        Objid last = db_last_used_objid();
        for(auto i=1; i<=last; i++)
            if(db_object_location(i) == -1)
                r = listappend(r, Var::new_obj(i));

        free_var(arglist);
        return make_var_pack(r);
    } else if(!is_valid(o)) {
        free_var(arglist);
        return make_error_pack(E_INVIND);
    }

    Var r = object_contents(o);
    for(auto i=1; i<=r.length(); i++)
        r = listconcat(r, object_contents(r[i]));

    free_var(arglist);
    return make_var_pack(r);
}

static inline Var corified_make_map()
{
    Var corified = new_map(0);
    
    db_for_all_props(Var::new_obj(SYSTEM_OBJECT), [&corified](Var name, Var value) -> int {
        if(value.type == TYPE_OBJ) {
            Stream *s = new_stream(memo_strlen(name.v.str)+2);
            stream_add_char(s, '$');
            stream_add_string(s, name.v.str);

            if(maphaskey(corified, value)) {
                if(corified[value].type != TYPE_LIST) {
                    Var _new = new_list(2);
                    _new[1] = var_ref(corified[value]);
                    _new[2] = str_dup_to_var(stream_contents(s));
                    corified[value] = _new;
                } else {
                    corified[value] = listappend(corified[value], str_dup_to_var(stream_contents(s)));
                }
            } else {
                corified = mapinsert(corified, var_ref(value), str_dup_to_var(stream_contents(s)));
            }

            free_stream(s);
        } else if(value.type == TYPE_MAP) {
            mapforeach(value, [&name, &corified](Var mapkey, Var mapvalue, int index) -> int {
                if(mapkey.type == TYPE_STR && mapvalue.type == TYPE_OBJ) {
                    Stream *s = new_stream(memo_strlen(name.v.str)+memo_strlen(mapkey.v.str)+6);
                    stream_printf(s, "$%s[\"%s\"]", name.v.str, mapkey.v.str);

                    if(maphaskey(corified, mapvalue)) {
                        if(corified[mapvalue].type != TYPE_LIST) {
                            Var _new = new_list(2);
                            _new[1] = var_ref(corified[mapvalue]);
                            _new[2] = str_dup_to_var(stream_contents(s));
                            corified[mapvalue] = _new;
                        } else {
                            corified[mapvalue] = listappend(corified[mapvalue], str_dup_to_var(stream_contents(s)));
                        }
                    } else {
                        corified = mapinsert(corified, var_ref(mapvalue), str_dup_to_var(stream_contents(s)));
                    }

                    free_stream(s);
                }
                return 0;
            });

        }
        return 0;
    });

    return corified;
}

static inline int check_nonce() {
    static int last_nonce = dbpriv_nonce(dbpriv_dereference(Var::new_obj(SYSTEM_OBJECT)));

    int mode = 0;
    int nonce = dbpriv_nonce(dbpriv_dereference(Var::new_obj(SYSTEM_OBJECT)));
    if(last_nonce != nonce) {
        last_nonce = nonce;
        mode = -1;
    }
    return mode;
}

Var corified_as(Var obj, int mode) {
    static Var corified = corified_make_map();

    if(mode < 0) {
        free_var(corified);
        corified = corified_make_map();
    }

    Var r;
    if(maphaskey(corified, obj)) {
        r = var_ref((mode == 0 && corified[obj].type == TYPE_LIST) ? corified[obj][1] : corified[obj]);
    } else {
        size_t nlen = log10(abs(obj.obj())) + 3;
        if(obj.obj() < 0) nlen++;

        Stream *s = new_stream(nlen);
        stream_printf(s, "#%lld", obj.obj());

        r = str_dup_to_var(stream_contents(s));
        
        free_stream(s);
    }

    return r;
}

static package
bf_corified_as(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r = corified_as(arglist[1], arglist.length() >= 2 ? arglist[2].num() : check_nonce());
    free_var(arglist);
    return make_var_pack(r);
}

Var nothing;        /* useful constant */
Var clear;          /* useful constant */
Var none;           /* useful constant */

void
register_objects(void)
{
    nothing.type  = TYPE_OBJ;
    nothing.v.obj = NOTHING;
    clear.type    = TYPE_CLEAR;
    none.type     = TYPE_NONE;

    register_function("toobj",  1, 1, bf_toobj, TYPE_ANY);
    register_function("typeof", 1, 1, bf_typeof, TYPE_ANY);

    register_function_with_read_write("create",   1, 4, bf_create, bf_create_read, bf_create_write, TYPE_ANY, TYPE_ANY, TYPE_ANY, TYPE_ANY);
    register_function_with_read_write("recreate", 2, 3, bf_recreate, bf_create_read, bf_create_write, TYPE_OBJ, TYPE_OBJ, TYPE_OBJ);
    register_function_with_read_write("recycle",  1, 1, bf_recycle, bf_recycle_read, bf_recycle_write, TYPE_ANY);
    register_function_with_read_write("move",     2, 3, bf_move, bf_move_read, bf_move_write, TYPE_OBJ, TYPE_OBJ, TYPE_INT);

    register_function("object_bytes",    1, 1, bf_object_bytes, TYPE_ANY);
    register_function("valid",           1, 1, bf_valid, TYPE_ANY);
    register_function("chparents",       2, 3, bf_chparents, TYPE_ANY, TYPE_ANY, TYPE_LIST);
    register_function("parents",         1, 1, bf_parents, TYPE_ANY);
    register_function("children",        1, 1, bf_children, TYPE_ANY);
    register_function("ancestors",       1, 2, bf_ancestors, TYPE_ANY, TYPE_ANY);
    register_function("descendants",     1, 2, bf_descendants, TYPE_ANY, TYPE_ANY);
    register_function("max_object",      0, 0, bf_max_object);
    register_function("players",         0, 0, bf_players);
    register_function("is_player",       1, 1, bf_is_player, TYPE_OBJ);
    register_function("set_player_flag", 2, 2, bf_set_player_flag, TYPE_OBJ, TYPE_ANY);
    register_function("isa",             2, 3, bf_isa, TYPE_ANY, TYPE_ANY, TYPE_INT);
    register_function("locate_by_name",  1, 2, bf_locate_by_name, TYPE_STR, TYPE_INT);
    register_function("occupants",       1, 4, bf_occupants, TYPE_LIST, TYPE_ANY, TYPE_INT, TYPE_INT);
    register_function("locations",       1, 3, bf_locations, TYPE_OBJ, TYPE_OBJ, TYPE_INT);

#ifdef USE_ANCESTOR_CACHE
    register_function("clear_ancestor_cache", 0, 0, bf_clear_ancestor_cache);
#endif

    register_function("recycled_objects",     0, 0, bf_recycled_objects);
    register_function("next_recycled_object", 0, 1, bf_next_recycled_object, TYPE_OBJ);
    register_function("owned_objects",        1, 1, bf_owned_objects, TYPE_OBJ);
    register_function("all_contents",         1, 1, bf_all_contents, TYPE_OBJ);
    register_function("corified_as",          1, 2, bf_corified_as, TYPE_OBJ, TYPE_INT);
}
