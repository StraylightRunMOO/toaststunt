#include <assert.h>
#include <stdlib.h>
#include <algorithm> // std::sort
#include <ctype.h>

#include "my-math.h"
#include "bf_register.h"
#include "collection.h"
#include "config.h"
#include "functions.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "storage.h"
#include "structures.h"
#include "utils.h"

Var
set_intersection(Var s1, Var s2, bool count_dup) {
    Var r = new_list(0);
    Var m = new_map(s2.length());

    listforeach(s2, [&m, &count_dup](Var value, int index) -> int {
        Var _new;
        m = (count_dup && maphaskey(m, value) && maplookup(m, value, &_new, 0) != nullptr) ? 
            mapinsert(m, var_ref(value), Var::new_int(_new.num()+1)) : 
            mapinsert(m, var_ref(value), Var::new_int(1));
        
        return 0;
    });

    listforeach(s1, [&m, &r](Var value, int index) -> int {
        Var count;
        if(maphaskey(m, value) && maplookup(m, value, &count, 0) != nullptr) {
            r = listappend(r, var_ref(value));

            if(count.num() <= 1)
                mapdelete(m, value);
            else
                m = mapinsert(m, var_ref(value), Var::new_int(count.num()-1));
        }

        return 0;
    });

    free_var(m);
    return r;
}

static package
bf_intersection(Var arglist, Byte next, void *vdata, Objid progr)
{
    auto nargs = arglist.length();
    bool count_dup = false;

    if(arglist[1].type != TYPE_LIST || arglist[2].type != TYPE_LIST) {
        free_var(arglist);
        return make_error_pack(E_INVARG);
    } else if(nargs >= 3) {
        if(arglist[nargs].type == TYPE_BOOL || arglist[nargs].type == TYPE_INT) {
            count_dup = is_true(arglist[nargs]);
            arglist = listdelete(arglist, nargs--);
        } else if(arglist[nargs].type != TYPE_LIST) {
            free_var(arglist);
            return make_error_pack(E_INVARG);
        }
    }

    Var r = set_intersection(arglist[1], arglist[2], count_dup);
    if(nargs >= 3) {
        for(auto i=3; i <= nargs; i++) {
            if(arglist[i].type == TYPE_LIST)
                r = set_intersection(r, arglist[i], count_dup);
            else {
                free_var(r);
                free_var(arglist);
                return make_error_pack(E_INVARG);
            }
        }
    }
    
    free_var(arglist);
    return make_var_pack(r);
}

void
register_set(void)
{
    register_function("intersection", 2, -1, bf_intersection, TYPE_LIST, TYPE_LIST, TYPE_ANY);
}