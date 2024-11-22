#ifndef MAP_H
#define MAP_H 1

#include <functional>

#include "structures.h"
#include "hashmap.h"

typedef struct map_entry {
    Var key;
    Var value;
} map_entry;

extern Var new_map(size_t size);
extern bool destroy_map(Var map);
extern Var map_dup(Var map);

extern Var mapinsert(Var map, Var key, Var value);
extern const map_entry *maplookup(Var map, Var key, Var *value, int case_matters);
extern const map_entry *mapstrlookup(Var map, const char *key, Var *value, int case_matters);
extern int mapseek(Var map, Var key, Var *iter, int case_matters);
extern int mapequal(Var lhs, Var rhs, int case_matters);
extern Num maplength(Var map);
extern int mapempty(Var map);
extern Num mapbuckets(Var map);

extern int map_sizeof(Var map);

extern int mapfirst(Var map, Var *value);
extern int maplast(Var map, Var *value);

extern Var maprange(Var map, int from, int to);
extern enum error maprangeset(Var map, int from, int to, Var value, Var *_new);
extern int mapkeyindex(Var map, Var key);

typedef std::function<int(Var, Var, int)> map_callback;
extern int mapforeach(Var map, map_callback func);

#endif
