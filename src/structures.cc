#include <string_view>
#include <vector>
#include <cstring>

#include "db.h"
#include "db_private.h"
#include "structures.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "numbers.h"
#include "utils.h"
#include "dependencies/xxhash.h"

static inline Var& err(enum error e) {
  static Var err_;
  err_ = Var::new_err(e);
  return std::ref(err_);
}

Var get_call(Var obj, Var verb) {
  Var c = make_call(obj.obj(), verb.v.str);
  
  if(db_verb_definer(*c.v.call).obj() != obj.obj())
    c.v.call->oid = obj.obj();

  return c;
}

static inline std::vector<Var> var_to_vec(Var v) {
    std::vector<Var> vec;

    if(v.type != TYPE_LIST) {
        vec.emplace_back(v);
        return vec;
    }

    auto len = v.length();
    vec.reserve(len); 
    vec.assign(&v[1], &v[len]+1);

    return vec;
}

static inline Var var_from_vec(std::vector<Var> vec) {
  auto size = vec.size();
  if(size == 0) 
      return new_list(0);

  Var v = new_list(size);
  memcpy(&v[1], vec.data(), size * sizeof(Var));

  return v;
}

// Default constructor
Var::Var() : v({.num = 0}), type(TYPE_NONE) {}

// copy constructors
Var::Var(const Var& other) : v(other.v), type(other.type) {}
//Var::Var(const std::vector<Var>& other) { *this = var_from_vec(other); }

// copy assignment operator
Var& Var::operator=(Var& other) {
  if(this == &other)
    return *this;

  (*this).v = other.v;
  (*this).type = other.type;

  return *this;
}

// copy assignment operator
Var& Var::operator=(const Var& other) { 
  if(this == &other)
    return *this;

  (*this).v = other.v;
  (*this).type = other.type;
  
  return *this;
}

// move assignment operator
Var& Var::operator=(Var&& other) {
  if(this == &other)
    return *this;

  *this = other;
  
  return *this;
}

inline Var Var::new_list(size_t size) {
  return new_list(size);
}

inline Var Var::new_map(size_t sz) {
  return new_map(sz);
}

inline hashmap *Var::map() const {
  return v.map;
}

inline Var& Var::map(Var k) const {
  return mapat(*this, k);
}

inline Var* Var::list() const {
  return v.list;
}

inline Var* Var::list(Num i) const {
  if(i < 0 || i > v.list[0].v.num)
    return nullptr;

  #ifdef MEMO_SIZE
    var_metadata *metadata = ((var_metadata*)v.list) - 1;
    metadata->size = 0;
  #endif

  return &(v.list[i]);
}

inline bool Var::contains(Var key) const {
  return maphaskey(*this, key);
}

inline Var& Var::__index(Num i) {
  if(!is_collection() && !is_str()) {
    return ::err(E_TYPE);
  } else if(i < 1 || i > length()) {
    return ::err(E_RANGE);
  }

  switch(type) {
  case TYPE_LIST:
    return std::ref(*list(i));
  case TYPE_MAP:
    return map(Var::new_int(i));
  case TYPE_STR:
    try {
      return *this; // TODO
    } catch (const std::out_of_range& e) {
      return ::err(E_RANGE);
    }
  default:
    break;
  }

  return ::err(E_TYPE);
}

Num Var::length() const {
  if(type == TYPE_LIST) {
    return v.list[0].num();
  } else if(type == TYPE_MAP) {
    return maplength(*this);
  } else if(type == TYPE_STR) {
    return memo_strlen(str());
  }

  return 0;
}

Var& Var::operator[](int i) { return __index(i); }

Var& Var::operator[](Var k) {
  switch(type) {
  case TYPE_LIST:
    return __index(k.num());
  case TYPE_MAP:
    return map(k);
  default:
    break;
  }

  return ::err(E_TYPE);
}

Var& Var::operator[](const char* str) {
  if(type == TYPE_MAP) {
    return map(str_dup_to_var(str));
  }

  return ::err(E_TYPE);
}

Var& Var::operator+=(const Var& rhs)
{
  if(this->is_num())
    *this = do_add(*this, rhs);
  else if(this->type == TYPE_LIST)
    *this = (rhs.type == TYPE_LIST) ? listconcat(var_ref(*this), var_dup(rhs)) : listappend(*this, var_dup(rhs));
  else if(this->type == TYPE_STR) {
    auto len1 = memo_strlen(this->v.str);
    auto len2 = memo_strlen(rhs.v.str);

    /*
    char *str = (char*)mymalloc(len1 + len2 + 1, M_STRING);
    strncpy(str, this->v.str, len1);
    strncat(str, rhs.v.str, len2);

    free_str(this->v.str);
    this->v.str_ = str;
    */

    (*this).v.str_ = (char *)myrealloc((void*)(*this).v.str, len1 + len2 + 1, M_STRING);
    strncat((*this).v.str_, rhs.v.str, len2);
  }

  return *this;
}

Var& Var::operator+(const Var& rhs)
{
  *this += rhs;
  *this = var_ref(*this);
  return *this;
}

Var::operator std::vector<Var>() const {
  return var_to_vec(*this);
}

Var::operator std::string() const {
  return toliteral(*this);
}

Var::operator int() const { return num(); };

std::size_t Var::hash() const {
  std::size_t hash = 0;

  switch (type) {
  case TYPE_STR:
    hash = static_cast<std::size_t>(XXH64((char*)v.str, memo_strlen(v.str), MAP_HASH_SEED1) ^ type);
    break;
  case TYPE_INT:
    hash = static_cast<std::size_t>(XXH64((char*)(&v.num), sizeof(v.num), MAP_HASH_SEED1) ^ type);
    break;
  case TYPE_FLOAT:
    hash = static_cast<std::size_t>(XXH64((char*)(&v.fnum), sizeof(v.fnum), MAP_HASH_SEED1) ^ type);
    break;
  case TYPE_OBJ:
    hash = static_cast<std::size_t>(XXH64((char*)(&v.obj), sizeof(v.obj), MAP_HASH_SEED1) ^ type);
    break;
  case TYPE_ERR:
    hash = static_cast<std::size_t>(XXH64((char*)(&v.err), sizeof(v.err), MAP_HASH_SEED1) ^ type);
    break;
  case TYPE_BOOL:
    hash = static_cast<std::size_t>(XXH64((char*)(&v.truth), sizeof(v.truth), MAP_HASH_SEED1) ^ type);
    break;
  case TYPE_LIST:
    listforeach(*this, [&hash](Var value, int index) -> int {
      hash ^= (value.hash() + Var::new_int(index).hash());
      return 0;
    });
    break;
  case TYPE_MAP:
    mapforeach(*this, [&hash](Var key, Var value, int index) -> int {
      hash ^= (value.hash() ^ key.hash());
      return 0;
    });
    break;
  case TYPE_CALL:
    hash = Var::new_obj(v.call->oid).hash() ^ v.call->verbname.hash() ^ type;
    break;
  case TYPE_COMPLEX:
    hash = Var::new_float(v.complex.real()).hash() ^ Var::new_float(v.complex.imag()).hash() ^ type;
    break;
  default:
    hash = -1;
    break;
  }

  return hash;
}
