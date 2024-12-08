#ifndef MAP_H
#define MAP_H 1

#include <functional>

#include "structures.h"
#include "hashmap.h"

typedef struct map_entry {
    Var key;
    Var value;
} map_entry;

extern Var new_map(size_t, int preserve_order = MAP_SAVE_INSERTION_ORDER_DEFAULT);
extern bool destroy_map(Var);
extern Var map_dup(Var);

extern bool mapdelete(Var, Var);
extern Var mapinsert(Var, Var, Var);
extern const map_entry *maplookup(Var, Var, Var*, int);
extern const map_entry *mapstrlookup(Var, const char*, Var*, int);
extern Var& mapat(Var, Var);

extern int mapseek(Var, Var, Var*, int);
extern int mapequal(Var, Var, int);
extern Num maplength(Var);
extern int mapempty(Var);
extern Num mapbuckets(Var);
extern int map_sizeof(Var);

extern int mapfirst(Var, Var*);
extern int maplast(Var, Var*);

extern Var maprange(Var, int, int);
extern enum error maprangeset(Var, int, int, Var, Var*);
extern bool maphaskey(Var, Var);
extern int mapkeyindex(Var, Var);

typedef std::function<int(Var, Var, int)> map_callback;
extern int mapforeach(Var, map_callback);

#endif
