#include <string_view>

#include "structures.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "utils.h"

static inline Var& err(enum error e) {
  static Var err_;
  err_ = Var::new_err(e);
  return std::ref(err_);
}

// Default constructor
Var::Var() : v({.num = 0}), type(TYPE_NONE) {}

// copy constructor
Var::Var(const Var& other) : v(other.v), type(other.type) {}

// copy assignment operator
Var& Var::operator=(Var& other) {
  if(this == &other)
    return *this;

  this->v = std::move(other.v);
  this->type = other.type;

  return *this;
}

// copy assignment operator
Var& Var::operator=(const Var& other) { 
  if(this == &other)
    return *this;

  this->v = std::move(other.v);
  this->type = other.type;

  return *this;
}

// move assignment operator
Var& Var::operator=(Var&& other) {
  if(this == &other)
    return *this;

  std::swap(this->v, other.v);
  std::swap(this->type, other.type);
  
  return *this;
}

// move assignment operator
Var& Var::operator=(const Var&& other) { 
  if(this == &other)
    return *this;

  this->v = std::move(other.v);
  this->type = other.type;

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
    return list(0)->num();
  } else if(type == TYPE_MAP) {
    return maplength(*this);
  } else if(type == TYPE_STR) {
    return memo_strlen(str());
  }

  return 0;
}

Var& Var::operator[](Num i) { return __index(i); }

Var& Var::operator[](Var k) {
  switch(type) {
  case TYPE_LIST:
    return __index(k.num());
  case TYPE_MAP:
    if(k.is_str() && !contains(k)) addref((void*)k.str());
    return map(k);
  default:
    break;
  }

  return ::err(E_TYPE);
}

Var& Var::operator[](const char* key) {
  if(type == TYPE_MAP)
    return map(Var::new_str(key, true));

  return ::err(E_TYPE);
}
