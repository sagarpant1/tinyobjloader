//
// Optimized wavefront .obj loader.
// Requires ltalloc and C++11
//

/*
The MIT License (MIT)

Copyright (c) 2012-2016 Syoyo Fujita and many contributors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#ifndef TINOBJ_LOADER_OPT_H_
#define TINOBJ_LOADER_OPT_H_

#ifdef _WIN64
#define atoll(S) _atoi64(S)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>
#endif

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include <atomic>  // C++11
#include <chrono>  // C++11
#include <thread>  // C++11

#include "ltalloc.hpp"

namespace tinyobj_opt {

// ----------------------------------------------------------------------------
// Small vector class useful for multi-threaded environment.
//
// stack_container.h
//
// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This allocator can be used with STL containers to provide a stack buffer
// from which to allocate memory and overflows onto the heap. This stack buffer
// would be allocated on the stack and allows us to avoid heap operations in
// some situations.
//
// STL likes to make copies of allocators, so the allocator itself can't hold
// the data. Instead, we make the creator responsible for creating a
// StackAllocator::Source which contains the data. Copying the allocator
// merely copies the pointer to this shared source, so all allocators created
// based on our allocator will share the same stack buffer.
//
// This stack buffer implementation is very simple. The first allocation that
// fits in the stack buffer will use the stack buffer. Any subsequent
// allocations will not use the stack buffer, even if there is unused room.
// This makes it appropriate for array-like containers, but the caller should
// be sure to reserve() in the container up to the stack buffer size. Otherwise
// the container will allocate a small array which will "use up" the stack
// buffer.
template <typename T, size_t stack_capacity>
class StackAllocator : public std::allocator<T> {
 public:
  typedef typename std::allocator<T>::pointer pointer;
  typedef typename std::allocator<T>::size_type size_type;

  // Backing store for the allocator. The container owner is responsible for
  // maintaining this for as long as any containers using this allocator are
  // live.
  struct Source {
    Source() : used_stack_buffer_(false) {}

    // Casts the buffer in its right type.
    T *stack_buffer() { return reinterpret_cast<T *>(stack_buffer_); }
    const T *stack_buffer() const {
      return reinterpret_cast<const T *>(stack_buffer_);
    }

    //
    // IMPORTANT: Take care to ensure that stack_buffer_ is aligned
    // since it is used to mimic an array of T.
    // Be careful while declaring any unaligned types (like bool)
    // before stack_buffer_.
    //

    // The buffer itself. It is not of type T because we don't want the
    // constructors and destructors to be automatically called. Define a POD
    // buffer of the right size instead.
    char stack_buffer_[sizeof(T[stack_capacity])];

    // Set when the stack buffer is used for an allocation. We do not track
    // how much of the buffer is used, only that somebody is using it.
    bool used_stack_buffer_;
  };

  // Used by containers when they want to refer to an allocator of type U.
  template <typename U>
  struct rebind {
    typedef StackAllocator<U, stack_capacity> other;
  };

  // For the straight up copy c-tor, we can share storage.
  StackAllocator(const StackAllocator<T, stack_capacity> &rhs)
      : source_(rhs.source_) {}

  // ISO C++ requires the following constructor to be defined,
  // and std::vector in VC++2008SP1 Release fails with an error
  // in the class _Container_base_aux_alloc_real (from <xutility>)
  // if the constructor does not exist.
  // For this constructor, we cannot share storage; there's
  // no guarantee that the Source buffer of Ts is large enough
  // for Us.
  // TODO(Google): If we were fancy pants, perhaps we could share storage
  // iff sizeof(T) == sizeof(U).
  template <typename U, size_t other_capacity>
  StackAllocator(const StackAllocator<U, other_capacity> &other)
      : source_(NULL) {
    (void)other;
  }

  explicit StackAllocator(Source *source) : source_(source) {}

  // Actually do the allocation. Use the stack buffer if nobody has used it yet
  // and the size requested fits. Otherwise, fall through to the standard
  // allocator.
  pointer allocate(size_type n, void *hint = 0) {
    if (source_ != NULL && !source_->used_stack_buffer_ &&
        n <= stack_capacity) {
      source_->used_stack_buffer_ = true;
      return source_->stack_buffer();
    } else {
      return std::allocator<T>::allocate(n, hint);
    }
  }

  // Free: when trying to free the stack buffer, just mark it as free. For
  // non-stack-buffer pointers, just fall though to the standard allocator.
  void deallocate(pointer p, size_type n) {
    if (source_ != NULL && p == source_->stack_buffer())
      source_->used_stack_buffer_ = false;
    else
      std::allocator<T>::deallocate(p, n);
  }

 private:
  Source *source_;
};

// A wrapper around STL containers that maintains a stack-sized buffer that the
// initial capacity of the vector is based on. Growing the container beyond the
// stack capacity will transparently overflow onto the heap. The container must
// support reserve().
//
// WATCH OUT: the ContainerType MUST use the proper StackAllocator for this
// type. This object is really intended to be used only internally. You'll want
// to use the wrappers below for different types.
template <typename TContainerType, int stack_capacity>
class StackContainer {
 public:
  typedef TContainerType ContainerType;
  typedef typename ContainerType::value_type ContainedType;
  typedef StackAllocator<ContainedType, stack_capacity> Allocator;

  // Allocator must be constructed before the container!
  StackContainer() : allocator_(&stack_data_), container_(allocator_) {
    // Make the container use the stack allocation by reserving our buffer size
    // before doing anything else.
    container_.reserve(stack_capacity);
  }

  // Getters for the actual container.
  //
  // Danger: any copies of this made using the copy constructor must have
  // shorter lifetimes than the source. The copy will share the same allocator
  // and therefore the same stack buffer as the original. Use std::copy to
  // copy into a "real" container for longer-lived objects.
  ContainerType &container() { return container_; }
  const ContainerType &container() const { return container_; }

  // Support operator-> to get to the container. This allows nicer syntax like:
  //   StackContainer<...> foo;
  //   std::sort(foo->begin(), foo->end());
  ContainerType *operator->() { return &container_; }
  const ContainerType *operator->() const { return &container_; }

#ifdef UNIT_TEST
  // Retrieves the stack source so that that unit tests can verify that the
  // buffer is being used properly.
  const typename Allocator::Source &stack_data() const { return stack_data_; }
#endif

 protected:
  typename Allocator::Source stack_data_;
  unsigned char pad_[7];
  Allocator allocator_;
  ContainerType container_;

  // DISALLOW_EVIL_CONSTRUCTORS(StackContainer);
  StackContainer(const StackContainer &);
  void operator=(const StackContainer &);
};

// StackVector
//
// Example:
//   StackVector<int, 16> foo;
//   foo->push_back(22);  // we have overloaded operator->
//   foo[0] = 10;         // as well as operator[]
template <typename T, size_t stack_capacity>
class StackVector
    : public StackContainer<std::vector<T, StackAllocator<T, stack_capacity> >,
                            stack_capacity> {
 public:
  StackVector()
      : StackContainer<std::vector<T, StackAllocator<T, stack_capacity> >,
                       stack_capacity>() {}

  // We need to put this in STL containers sometimes, which requires a copy
  // constructor. We can't call the regular copy constructor because that will
  // take the stack buffer from the original. Here, we create an empty object
  // and make a stack buffer of its own.
  StackVector(const StackVector<T, stack_capacity> &other)
      : StackContainer<std::vector<T, StackAllocator<T, stack_capacity> >,
                       stack_capacity>() {
    this->container().assign(other->begin(), other->end());
  }

  StackVector<T, stack_capacity> &operator=(
      const StackVector<T, stack_capacity> &other) {
    this->container().assign(other->begin(), other->end());
    return *this;
  }

  // Vectors are commonly indexed, which isn't very convenient even with
  // operator-> (using "->at()" does exception stuff we don't want).
  T &operator[](size_t i) { return this->container().operator[](i); }
  const T &operator[](size_t i) const {
    return this->container().operator[](i);
  }
};

// ----------------------------------------------------------------------------

typedef struct {
  std::string name;

  float ambient[3];
  float diffuse[3];
  float specular[3];
  float transmittance[3];
  float emission[3];
  float shininess;
  float ior;       // index of refraction
  float dissolve;  // 1 == opaque; 0 == fully transparent
  // illumination model (see http://www.fileformat.info/format/material/)
  int illum;

  int dummy;  // Suppress padding warning.

  std::string ambient_texname;             // map_Ka
  std::string diffuse_texname;             // map_Kd
  std::string specular_texname;            // map_Ks
  std::string specular_highlight_texname;  // map_Ns
  std::string bump_texname;                // map_bump, bump
  std::string displacement_texname;        // disp
  std::string alpha_texname;               // map_d
  std::map<std::string, std::string> unknown_parameter;
} material_t;

typedef struct {
  std::string name;  // group name or object name.
  unsigned int face_offset;
  unsigned int length;
} shape_t;

struct vertex_index {
  int v_idx, vt_idx, vn_idx;
  vertex_index() : v_idx(-1), vt_idx(-1), vn_idx(-1) {}
  explicit vertex_index(int idx) : v_idx(idx), vt_idx(idx), vn_idx(idx) {}
  vertex_index(int vidx, int vtidx, int vnidx)
      : v_idx(vidx), vt_idx(vtidx), vn_idx(vnidx) {}
};

typedef struct {
  std::vector<float, lt::allocator<float> > vertices;
  std::vector<float, lt::allocator<float> > normals;
  std::vector<float, lt::allocator<float> > texcoords;
  std::vector<vertex_index, lt::allocator<vertex_index> > faces;
  std::vector<int, lt::allocator<int> > face_num_verts;
  std::vector<int, lt::allocator<int> > material_ids;
} attrib_t;

typedef StackVector<char, 256> ShortString;

#define IS_SPACE(x) (((x) == ' ') || ((x) == '\t'))
#define IS_DIGIT(x) \
  (static_cast<unsigned int>((x) - '0') < static_cast<unsigned int>(10))
#define IS_NEW_LINE(x) (((x) == '\r') || ((x) == '\n') || ((x) == '\0'))

static inline void skip_space(const char **token) {
  while ((*token)[0] == ' ' || (*token)[0] == '\t') {
    (*token)++;
  }
}

static inline void skip_space_and_cr(const char **token) {
  while ((*token)[0] == ' ' || (*token)[0] == '\t' || (*token)[0] == '\r') {
    (*token)++;
  }
}

static inline int until_space(const char *token) {
  const char *p = token;
  while (p[0] != '\0' && p[0] != ' ' && p[0] != '\t' && p[0] != '\r') {
    p++;
  }

  return p - token;
}

static inline int length_until_newline(const char *token, int n) {
  int len = 0;

  // Assume token[n-1] = '\0'
  for (len = 0; len < n - 1; len++) {
    if (token[len] == '\n') {
      break;
    }
    if ((token[len] == '\r') && ((len < (n - 2)) && (token[len + 1] != '\n'))) {
      break;
    }
  }

  return len;
}

// http://stackoverflow.com/questions/5710091/how-does-atoi-function-in-c-work
static inline int my_atoi(const char *c) {
  int value = 0;
  int sign = 1;
  if (*c == '+' || *c == '-') {
    if (*c == '-') sign = -1;
    c++;
  }
  while (((*c) >= '0') && ((*c) <= '9')) {  // isdigit(*c)
    value *= 10;
    value += (int)(*c - '0');
    c++;
  }
  return value * sign;
}

// Make index zero-base, and also support relative index.
static inline int fixIndex(int idx, int n) {
  if (idx > 0) return idx - 1;
  if (idx == 0) return 0;
  return n + idx;  // negative value = relative
}

// Parse raw triples: i, i/j/k, i//k, i/j
static vertex_index parseRawTriple(const char **token) {
  vertex_index vi(
      static_cast<int>(0x80000000));  // 0x80000000 = -2147483648 = invalid

  vi.v_idx = my_atoi((*token));
  //(*token) += strcspn((*token), "/ \t\r");
  while ((*token)[0] != '\0' && (*token)[0] != '/' && (*token)[0] != ' ' &&
         (*token)[0] != '\t' && (*token)[0] != '\r') {
    (*token)++;
  }
  if ((*token)[0] != '/') {
    return vi;
  }
  (*token)++;

  // i//k
  if ((*token)[0] == '/') {
    (*token)++;
    vi.vn_idx = my_atoi((*token));
    //(*token) += strcspn((*token), "/ \t\r");
    while ((*token)[0] != '\0' && (*token)[0] != '/' && (*token)[0] != ' ' &&
           (*token)[0] != '\t' && (*token)[0] != '\r') {
      (*token)++;
    }
    return vi;
  }

  // i/j/k or i/j
  vi.vt_idx = my_atoi((*token));
  //(*token) += strcspn((*token), "/ \t\r");
  while ((*token)[0] != '\0' && (*token)[0] != '/' && (*token)[0] != ' ' &&
         (*token)[0] != '\t' && (*token)[0] != '\r') {
    (*token)++;
  }
  if ((*token)[0] != '/') {
    return vi;
  }

  // i/j/k
  (*token)++;  // skip '/'
  vi.vn_idx = my_atoi((*token));
  //(*token) += strcspn((*token), "/ \t\r");
  while ((*token)[0] != '\0' && (*token)[0] != '/' && (*token)[0] != ' ' &&
         (*token)[0] != '\t' && (*token)[0] != '\r') {
    (*token)++;
  }
  return vi;
}

static inline bool parseString(ShortString *s, const char **token) {
  skip_space(token);                 //(*token) += strspn((*token), " \t");
  size_t e = until_space((*token));  // strcspn((*token), " \t\r");
  (*s)->insert((*s)->end(), (*token), (*token) + e);
  (*token) += e;
  return true;
}

static inline int parseInt(const char **token) {
  skip_space(token);  //(*token) += strspn((*token), " \t");
  int i = my_atoi((*token));
  (*token) += until_space((*token));  // strcspn((*token), " \t\r");
  return i;
}

// Tries to parse a floating point number located at s.
//
// s_end should be a location in the string where reading should absolutely
// stop. For example at the end of the string, to prevent buffer overflows.
//
// Parses the following EBNF grammar:
//   sign    = "+" | "-" ;
//   END     = ? anything not in digit ?
//   digit   = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" ;
//   integer = [sign] , digit , {digit} ;
//   decimal = integer , ["." , integer] ;
//   float   = ( decimal , END ) | ( decimal , ("E" | "e") , integer , END ) ;
//
//  Valid strings are for example:
//   -0  +3.1417e+2  -0.0E-3  1.0324  -1.41   11e2
//
// If the parsing is a success, result is set to the parsed value and true
// is returned.
//
// The function is greedy and will parse until any of the following happens:
//  - a non-conforming character is encountered.
//  - s_end is reached.
//
// The following situations triggers a failure:
//  - s >= s_end.
//  - parse failure.
//
static bool tryParseDouble(const char *s, const char *s_end, double *result) {
  if (s >= s_end) {
    return false;
  }

  double mantissa = 0.0;
  // This exponent is base 2 rather than 10.
  // However the exponent we parse is supposed to be one of ten,
  // thus we must take care to convert the exponent/and or the
  // mantissa to a * 2^E, where a is the mantissa and E is the
  // exponent.
  // To get the final double we will use ldexp, it requires the
  // exponent to be in base 2.
  int exponent = 0;

  // NOTE: THESE MUST BE DECLARED HERE SINCE WE ARE NOT ALLOWED
  // TO JUMP OVER DEFINITIONS.
  char sign = '+';
  char exp_sign = '+';
  char const *curr = s;

  // How many characters were read in a loop.
  int read = 0;
  // Tells whether a loop terminated due to reaching s_end.
  bool end_not_reached = false;

  /*
          BEGIN PARSING.
  */

  // Find out what sign we've got.
  if (*curr == '+' || *curr == '-') {
    sign = *curr;
    curr++;
  } else if (IS_DIGIT(*curr)) { /* Pass through. */
  } else {
    goto fail;
  }

  // Read the integer part.
  end_not_reached = (curr != s_end);
  while (end_not_reached && IS_DIGIT(*curr)) {
    mantissa *= 10;
    mantissa += static_cast<int>(*curr - 0x30);
    curr++;
    read++;
    end_not_reached = (curr != s_end);
  }

  // We must make sure we actually got something.
  if (read == 0) goto fail;
  // We allow numbers of form "#", "###" etc.
  if (!end_not_reached) goto assemble;

  // Read the decimal part.
  if (*curr == '.') {
    curr++;
    read = 1;
    end_not_reached = (curr != s_end);
    while (end_not_reached && IS_DIGIT(*curr)) {
      // pow(10.0, -read)
      double frac_value = 1.0;
      for (int f = 0; f < read; f++) {
        frac_value *= 0.1;
      }
      mantissa += static_cast<int>(*curr - 0x30) * frac_value;
      read++;
      curr++;
      end_not_reached = (curr != s_end);
    }
  } else if (*curr == 'e' || *curr == 'E') {
  } else {
    goto assemble;
  }

  if (!end_not_reached) goto assemble;

  // Read the exponent part.
  if (*curr == 'e' || *curr == 'E') {
    curr++;
    // Figure out if a sign is present and if it is.
    end_not_reached = (curr != s_end);
    if (end_not_reached && (*curr == '+' || *curr == '-')) {
      exp_sign = *curr;
      curr++;
    } else if (IS_DIGIT(*curr)) { /* Pass through. */
    } else {
      // Empty E is not allowed.
      goto fail;
    }

    read = 0;
    end_not_reached = (curr != s_end);
    while (end_not_reached && IS_DIGIT(*curr)) {
      exponent *= 10;
      exponent += static_cast<int>(*curr - 0x30);
      curr++;
      read++;
      end_not_reached = (curr != s_end);
    }
    exponent *= (exp_sign == '+' ? 1 : -1);
    if (read == 0) goto fail;
  }

assemble :

{
  // = pow(5.0, exponent);
  double a = 5.0;
  for (int i = 0; i < exponent; i++) {
    a = a * a;
  }
  *result =
      //(sign == '+' ? 1 : -1) * ldexp(mantissa * pow(5.0, exponent), exponent);
      (sign == '+' ? 1 : -1) * (mantissa * a) *
      static_cast<double>(1ULL << exponent);  // 5.0^exponent * 2^exponent
}

  return true;
fail:
  return false;
}

static inline float parseFloat(const char **token) {
  skip_space(token);  //(*token) += strspn((*token), " \t");
#ifdef TINY_OBJ_LOADER_OLD_FLOAT_PARSER
  float f = static_cast<float>(atof(*token));
  (*token) += strcspn((*token), " \t\r");
#else
  const char *end =
      (*token) + until_space((*token));  // strcspn((*token), " \t\r");
  double val = 0.0;
  tryParseDouble((*token), end, &val);
  float f = static_cast<float>(val);
  (*token) = end;
#endif
  return f;
}

static inline void parseFloat2(float *x, float *y, const char **token) {
  (*x) = parseFloat(token);
  (*y) = parseFloat(token);
}

static inline void parseFloat3(float *x, float *y, float *z,
                               const char **token) {
  (*x) = parseFloat(token);
  (*y) = parseFloat(token);
  (*z) = parseFloat(token);
}

static void InitMaterial(material_t *material) {
  material->name = "";
  material->ambient_texname = "";
  material->diffuse_texname = "";
  material->specular_texname = "";
  material->specular_highlight_texname = "";
  material->bump_texname = "";
  material->displacement_texname = "";
  material->alpha_texname = "";
  for (int i = 0; i < 3; i++) {
    material->ambient[i] = 0.f;
    material->diffuse[i] = 0.f;
    material->specular[i] = 0.f;
    material->transmittance[i] = 0.f;
    material->emission[i] = 0.f;
  }
  material->illum = 0;
  material->dissolve = 1.f;
  material->shininess = 1.f;
  material->ior = 1.f;
  material->unknown_parameter.clear();
}

static void LoadMtl(std::map<std::string, int> *material_map,
                    std::vector<material_t> *materials,
                    std::istream *inStream) {
  // Create a default material anyway.
  material_t material;
  InitMaterial(&material);

  size_t maxchars = 8192;           // Alloc enough size.
  std::vector<char> buf(maxchars);  // Alloc enough size.
  while (inStream->peek() != -1) {
    inStream->getline(&buf[0], static_cast<std::streamsize>(maxchars));

    std::string linebuf(&buf[0]);

    // Trim newline '\r\n' or '\n'
    if (linebuf.size() > 0) {
      if (linebuf[linebuf.size() - 1] == '\n')
        linebuf.erase(linebuf.size() - 1);
    }
    if (linebuf.size() > 0) {
      if (linebuf[linebuf.size() - 1] == '\r')
        linebuf.erase(linebuf.size() - 1);
    }

    // Skip if empty line.
    if (linebuf.empty()) {
      continue;
    }

    // Skip leading space.
    const char *token = linebuf.c_str();
    token += strspn(token, " \t");

    assert(token);
    if (token[0] == '\0') continue;  // empty line

    if (token[0] == '#') continue;  // comment line

    // new mtl
    if ((0 == strncmp(token, "newmtl", 6)) && IS_SPACE((token[6]))) {
      // flush previous material.
      if (!material.name.empty()) {
        material_map->insert(std::pair<std::string, int>(
            material.name, static_cast<int>(materials->size())));
        materials->push_back(material);
      }

      // initial temporary material
      InitMaterial(&material);

      // set new mtl name
      char namebuf[4096];
      token += 7;
#ifdef _MSC_VER
      sscanf_s(token, "%s", namebuf, (unsigned)_countof(namebuf));
#else
      sscanf(token, "%s", namebuf);
#endif
      material.name = namebuf;
      continue;
    }

    // ambient
    if (token[0] == 'K' && token[1] == 'a' && IS_SPACE((token[2]))) {
      token += 2;
      float r, g, b;
      parseFloat3(&r, &g, &b, &token);
      material.ambient[0] = r;
      material.ambient[1] = g;
      material.ambient[2] = b;
      continue;
    }

    // diffuse
    if (token[0] == 'K' && token[1] == 'd' && IS_SPACE((token[2]))) {
      token += 2;
      float r, g, b;
      parseFloat3(&r, &g, &b, &token);
      material.diffuse[0] = r;
      material.diffuse[1] = g;
      material.diffuse[2] = b;
      continue;
    }

    // specular
    if (token[0] == 'K' && token[1] == 's' && IS_SPACE((token[2]))) {
      token += 2;
      float r, g, b;
      parseFloat3(&r, &g, &b, &token);
      material.specular[0] = r;
      material.specular[1] = g;
      material.specular[2] = b;
      continue;
    }

    // transmittance
    if (token[0] == 'K' && token[1] == 't' && IS_SPACE((token[2]))) {
      token += 2;
      float r, g, b;
      parseFloat3(&r, &g, &b, &token);
      material.transmittance[0] = r;
      material.transmittance[1] = g;
      material.transmittance[2] = b;
      continue;
    }

    // ior(index of refraction)
    if (token[0] == 'N' && token[1] == 'i' && IS_SPACE((token[2]))) {
      token += 2;
      material.ior = parseFloat(&token);
      continue;
    }

    // emission
    if (token[0] == 'K' && token[1] == 'e' && IS_SPACE(token[2])) {
      token += 2;
      float r, g, b;
      parseFloat3(&r, &g, &b, &token);
      material.emission[0] = r;
      material.emission[1] = g;
      material.emission[2] = b;
      continue;
    }

    // shininess
    if (token[0] == 'N' && token[1] == 's' && IS_SPACE(token[2])) {
      token += 2;
      material.shininess = parseFloat(&token);
      continue;
    }

    // illum model
    if (0 == strncmp(token, "illum", 5) && IS_SPACE(token[5])) {
      token += 6;
      material.illum = parseInt(&token);
      continue;
    }

    // dissolve
    if ((token[0] == 'd' && IS_SPACE(token[1]))) {
      token += 1;
      material.dissolve = parseFloat(&token);
      continue;
    }
    if (token[0] == 'T' && token[1] == 'r' && IS_SPACE(token[2])) {
      token += 2;
      // Invert value of Tr(assume Tr is in range [0, 1])
      material.dissolve = 1.0f - parseFloat(&token);
      continue;
    }

    // ambient texture
    if ((0 == strncmp(token, "map_Ka", 6)) && IS_SPACE(token[6])) {
      token += 7;
      material.ambient_texname = token;
      continue;
    }

    // diffuse texture
    if ((0 == strncmp(token, "map_Kd", 6)) && IS_SPACE(token[6])) {
      token += 7;
      material.diffuse_texname = token;
      continue;
    }

    // specular texture
    if ((0 == strncmp(token, "map_Ks", 6)) && IS_SPACE(token[6])) {
      token += 7;
      material.specular_texname = token;
      continue;
    }

    // specular highlight texture
    if ((0 == strncmp(token, "map_Ns", 6)) && IS_SPACE(token[6])) {
      token += 7;
      material.specular_highlight_texname = token;
      continue;
    }

    // bump texture
    if ((0 == strncmp(token, "map_bump", 8)) && IS_SPACE(token[8])) {
      token += 9;
      material.bump_texname = token;
      continue;
    }

    // alpha texture
    if ((0 == strncmp(token, "map_d", 5)) && IS_SPACE(token[5])) {
      token += 6;
      material.alpha_texname = token;
      continue;
    }

    // bump texture
    if ((0 == strncmp(token, "bump", 4)) && IS_SPACE(token[4])) {
      token += 5;
      material.bump_texname = token;
      continue;
    }

    // displacement texture
    if ((0 == strncmp(token, "disp", 4)) && IS_SPACE(token[4])) {
      token += 5;
      material.displacement_texname = token;
      continue;
    }

    // unknown parameter
    const char *_space = strchr(token, ' ');
    if (!_space) {
      _space = strchr(token, '\t');
    }
    if (_space) {
      std::ptrdiff_t len = _space - token;
      std::string key(token, static_cast<size_t>(len));
      std::string value = _space + 1;
      material.unknown_parameter.insert(
          std::pair<std::string, std::string>(key, value));
    }
  }
  // flush last material.
  material_map->insert(std::pair<std::string, int>(
      material.name, static_cast<int>(materials->size())));
  materials->push_back(material);
}

typedef enum {
  COMMAND_EMPTY,
  COMMAND_V,
  COMMAND_VN,
  COMMAND_VT,
  COMMAND_F,
  COMMAND_G,
  COMMAND_O,
  COMMAND_USEMTL,
  COMMAND_MTLLIB,

} CommandType;

typedef struct {
  float vx, vy, vz;
  float nx, ny, nz;
  float tx, ty;

  // for f
  std::vector<vertex_index, lt::allocator<vertex_index> > f;
  // std::vector<vertex_index> f;
  std::vector<int, lt::allocator<int> > f_num_verts;

  const char *group_name;
  unsigned int group_name_len;
  const char *object_name;
  unsigned int object_name_len;
  const char *material_name;
  unsigned int material_name_len;

  const char *mtllib_name;
  unsigned int mtllib_name_len;

  CommandType type;
} Command;

struct CommandCount {
  size_t num_v;
  size_t num_vn;
  size_t num_vt;
  size_t num_f;
  size_t num_faces;
  CommandCount() {
    num_v = 0;
    num_vn = 0;
    num_vt = 0;
    num_f = 0;
    num_faces = 0;
  }
};

class
LoadOption
{
 public:
	LoadOption() : req_num_threads(-1), triangulate(true), verbose(false) {}
	
	int  req_num_threads;
	bool triangulate;
	bool verbose;

};


/// Parse wavefront .obj(.obj string data is expanded to linear char array
/// `buf')
/// -1 to req_num_threads use the number of HW threads in the running system.
bool parseObj(attrib_t *attrib, std::vector<shape_t> *shapes, const char *buf,
              size_t len, const LoadOption& option);

#ifdef TINYOBJ_LOADER_OPT_IMPLEMENTATION

static bool parseLine(Command *command, const char *p, size_t p_len,
                      bool triangulate = true) {
  char linebuf[4096];
  assert(p_len < 4095);
  // StackVector<char, 256> linebuf;
  // linebuf->resize(p_len + 1);
  memcpy(&linebuf, p, p_len);
  linebuf[p_len] = '\0';

  const char *token = linebuf;

  command->type = COMMAND_EMPTY;

  // Skip leading space.
  // token += strspn(token, " \t");
  skip_space(&token);  //(*token) += strspn((*token), " \t");

  assert(token);
  if (token[0] == '\0') {  // empty line
    return false;
  }

  if (token[0] == '#') {  // comment line
    return false;
  }

  // vertex
  if (token[0] == 'v' && IS_SPACE((token[1]))) {
    token += 2;
    float x, y, z;
    parseFloat3(&x, &y, &z, &token);
    command->vx = x;
    command->vy = y;
    command->vz = z;
    command->type = COMMAND_V;
    return true;
  }

  // normal
  if (token[0] == 'v' && token[1] == 'n' && IS_SPACE((token[2]))) {
    token += 3;
    float x, y, z;
    parseFloat3(&x, &y, &z, &token);
    command->nx = x;
    command->ny = y;
    command->nz = z;
    command->type = COMMAND_VN;
    return true;
  }

  // texcoord
  if (token[0] == 'v' && token[1] == 't' && IS_SPACE((token[2]))) {
    token += 3;
    float x, y;
    parseFloat2(&x, &y, &token);
    command->tx = x;
    command->ty = y;
    command->type = COMMAND_VT;
    return true;
  }

  // face
  if (token[0] == 'f' && IS_SPACE((token[1]))) {
    token += 2;
    // token += strspn(token, " \t");
    skip_space(&token);

    StackVector<vertex_index, 8> f;

    while (!IS_NEW_LINE(token[0])) {
      vertex_index vi = parseRawTriple(&token);
      // printf("v = %d, %d, %d\n", vi.v_idx, vi.vn_idx, vi.vt_idx);
      // if (callback.index_cb) {
      //  callback.index_cb(user_data, vi.v_idx, vi.vn_idx, vi.vt_idx);
      //}
      // size_t n = strspn(token, " \t\r");
      // token += n;
      skip_space_and_cr(&token);

      f->push_back(vi);
    }

    command->type = COMMAND_F;

    if (triangulate) {
      vertex_index i0 = f[0];
      vertex_index i1(-1);
      vertex_index i2 = f[1];

      for (size_t k = 2; k < f->size(); k++) {
        i1 = i2;
        i2 = f[k];
        command->f.emplace_back(i0);
        command->f.emplace_back(i1);
        command->f.emplace_back(i2);

        command->f_num_verts.emplace_back(3);
      }

    } else {
      for (size_t k = 0; k < f->size(); k++) {
        command->f.emplace_back(f[k]);
      }

      command->f_num_verts.emplace_back(f->size());
    }

    return true;
  }

  // use mtl
  if ((0 == strncmp(token, "usemtl", 6)) && IS_SPACE((token[6]))) {
    token += 7;

    // int newMaterialId = -1;
    // if (material_map.find(namebuf) != material_map.end()) {
    //  newMaterialId = material_map[namebuf];
    //} else {
    //  // { error!! material not found }
    //}

    // if (newMaterialId != materialId) {
    //  materialId = newMaterialId;
    //}

    // command->material_name = .insert(command->material_name->end(), namebuf,
    // namebuf + strlen(namebuf));
    // command->material_name->push_back('\0');
    skip_space(&token);
    command->material_name = p + (token - linebuf);
    command->material_name_len =
        length_until_newline(token, p_len - (token - linebuf)) + 1;
    command->type = COMMAND_USEMTL;

    return true;
  }

  // load mtl
  if ((0 == strncmp(token, "mtllib", 6)) && IS_SPACE((token[6]))) {
    // By specification, `mtllib` should be appear only once in .obj
    token += 7;

    skip_space(&token);
    command->mtllib_name = p + (token - linebuf);
    command->mtllib_name_len =
        length_until_newline(token, p_len - (token - linebuf)) + 1;
    command->type = COMMAND_MTLLIB;

    return true;
  }

  // group name
  if (token[0] == 'g' && IS_SPACE((token[1]))) {
    // @todo { multiple group name. }
    token += 2;

    command->group_name = p + (token - linebuf);
    command->group_name_len =
        length_until_newline(token, p_len - (token - linebuf)) + 1;
    command->type = COMMAND_G;

    return true;
  }

  // object name
  if (token[0] == 'o' && IS_SPACE((token[1]))) {
    // @todo { multiple object name? }
    token += 2;

    command->object_name = p + (token - linebuf);
    command->object_name_len =
        length_until_newline(token, p_len - (token - linebuf)) + 1;
    command->type = COMMAND_O;

    return true;
  }

  return false;
}

typedef struct {
  size_t pos;
  size_t len;
} LineInfo;

// Idea come from https://github.com/antonmks/nvParse
// 1. mmap file
// 2. find newline(\n, \r\n, \r) and list of line data.
// 3. Do parallel parsing for each line.
// 4. Reconstruct final mesh data structure.

#define kMaxThreads (32)

static inline bool is_line_ending(const char *p, size_t i, size_t end_i) {
  if (p[i] == '\0') return true;
  if (p[i] == '\n') return true;  // this includes \r\n
  if (p[i] == '\r') {
    if (((i + 1) < end_i) && (p[i + 1] != '\n')) {  // detect only \r case
      return true;
    }
  }
  return false;
}

bool parseObj(attrib_t *attrib, std::vector<shape_t> *shapes, const char *buf,
              size_t len, const LoadOption& option)
{
  attrib->vertices.clear();
  attrib->normals.clear();
  attrib->texcoords.clear();
  attrib->faces.clear();
  attrib->face_num_verts.clear();
  attrib->material_ids.clear();
  shapes->clear();

  if (len < 1) return false;

  auto num_threads = (option.req_num_threads < 0) ? std::thread::hardware_concurrency()
                                           : option.req_num_threads;
  num_threads =
      std::max(1, std::min(static_cast<int>(num_threads), kMaxThreads));

	if (option.verbose) {
		std::cout << "# of threads = " << num_threads << std::endl;
	}

  auto t1 = std::chrono::high_resolution_clock::now();

  std::vector<LineInfo, lt::allocator<LineInfo> > line_infos[kMaxThreads];
  for (size_t t = 0; t < static_cast<size_t>(num_threads); t++) {
    // Pre allocate enough memory. len / 128 / num_threads is just a heuristic
    // value.
    line_infos[t].reserve(len / 128 / num_threads);
  }

  std::chrono::duration<double, std::milli> ms_linedetection;
  std::chrono::duration<double, std::milli> ms_alloc;
  std::chrono::duration<double, std::milli> ms_parse;
  std::chrono::duration<double, std::milli> ms_load_mtl;
  std::chrono::duration<double, std::milli> ms_merge;
  std::chrono::duration<double, std::milli> ms_construct;

  // 1. Find '\n' and create line data.
  {
    StackVector<std::thread, 16> workers;

    auto start_time = std::chrono::high_resolution_clock::now();
    auto chunk_size = len / num_threads;

    for (size_t t = 0; t < static_cast<size_t>(num_threads); t++) {
      workers->push_back(std::thread([&, t]() {
        auto start_idx = (t + 0) * chunk_size;
        auto end_idx = std::min((t + 1) * chunk_size, len - 1);
        if (t == static_cast<size_t>((num_threads - 1))) {
          end_idx = len - 1;
        }

        size_t prev_pos = start_idx;
        for (size_t i = start_idx; i < end_idx; i++) {
          if (is_line_ending(buf, i, end_idx)) {
            if ((t > 0) && (prev_pos == start_idx) &&
                (!is_line_ending(buf, start_idx - 1, end_idx))) {
              // first linebreak found in (chunk > 0), and a line before this
              // linebreak belongs to previous chunk, so skip it.
              prev_pos = i + 1;
              continue;
            } else {
              LineInfo info;
              info.pos = prev_pos;
              info.len = i - prev_pos;

              if (info.len > 0) {
                line_infos[t].push_back(info);
              }

              prev_pos = i + 1;
            }
          }
        }

        // Find extra line which spand across chunk boundary.
        if ((t < num_threads) && (buf[end_idx - 1] != '\n')) {
          auto extra_span_idx = std::min(end_idx - 1 + chunk_size, len - 1);
          for (size_t i = end_idx; i < extra_span_idx; i++) {
            if (is_line_ending(buf, i, extra_span_idx)) {
              LineInfo info;
              info.pos = prev_pos;
              info.len = i - prev_pos;

              if (info.len > 0) {
                line_infos[t].push_back(info);
              }

              break;
            }
          }
        }
      }));
    }

    for (size_t t = 0; t < workers->size(); t++) {
      workers[t].join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();

    ms_linedetection = end_time - start_time;
  }

  auto line_sum = 0;
  for (size_t t = 0; t < num_threads; t++) {
    // std::cout << t << ": # of lines = " << line_infos[t].size() << std::endl;
    line_sum += line_infos[t].size();
  }
  // std::cout << "# of lines = " << line_sum << std::endl;

  std::vector<Command> commands[kMaxThreads];

  // 2. allocate buffer
  auto t_alloc_start = std::chrono::high_resolution_clock::now();
  {
    for (size_t t = 0; t < num_threads; t++) {
      commands[t].reserve(line_infos[t].size());
    }
  }

  CommandCount command_count[kMaxThreads];
  // Array index to `mtllib` line. According to wavefront .obj spec, `mtllib'
  // should appear only once in .obj.
  int mtllib_t_index = -1;
  int mtllib_i_index = -1;

  ms_alloc = std::chrono::high_resolution_clock::now() - t_alloc_start;

  // 2. parse each line in parallel.
  {
    StackVector<std::thread, 16> workers;
    auto t_start = std::chrono::high_resolution_clock::now();

    for (size_t t = 0; t < num_threads; t++) {
      workers->push_back(std::thread([&, t]() {

        for (size_t i = 0; i < line_infos[t].size(); i++) {
          Command command;
          bool ret = parseLine(&command, &buf[line_infos[t][i].pos],
                               line_infos[t][i].len, option.triangulate);
          if (ret) {
            if (command.type == COMMAND_V) {
              command_count[t].num_v++;
            } else if (command.type == COMMAND_VN) {
              command_count[t].num_vn++;
            } else if (command.type == COMMAND_VT) {
              command_count[t].num_vt++;
            } else if (command.type == COMMAND_F) {
              command_count[t].num_f += command.f.size();
              command_count[t].num_faces++;
            }

            if (command.type == COMMAND_MTLLIB) {
              mtllib_t_index = t;
              mtllib_i_index = commands->size();
            }

            commands[t].emplace_back(std::move(command));
          }
        }

      }));
    }

    for (size_t t = 0; t < workers->size(); t++) {
      workers[t].join();
    }

    auto t_end = std::chrono::high_resolution_clock::now();

    ms_parse = t_end - t_start;
  }

  std::map<std::string, int> material_map;
  std::vector<material_t> materials;

  // Load material(if exits)
  if (mtllib_i_index >= 0 && mtllib_t_index >= 0 &&
      commands[mtllib_t_index][mtllib_i_index].mtllib_name &&
      commands[mtllib_t_index][mtllib_i_index].mtllib_name_len > 0) {
    std::string material_filename =
        std::string(commands[mtllib_t_index][mtllib_i_index].mtllib_name,
                    commands[mtllib_t_index][mtllib_i_index].mtllib_name_len);
    // std::cout << "mtllib :" << material_filename << std::endl;

    auto t1 = std::chrono::high_resolution_clock::now();

    std::ifstream ifs(material_filename);
    if (ifs.good()) {
      LoadMtl(&material_map, &materials, &ifs);

      // std::cout << "maetrials = " << materials.size() << std::endl;

      ifs.close();
    }

    auto t2 = std::chrono::high_resolution_clock::now();

    ms_load_mtl = t2 - t1;
  }

  auto command_sum = 0;
  for (size_t t = 0; t < num_threads; t++) {
    // std::cout << t << ": # of commands = " << commands[t].size() <<
    // std::endl;
    command_sum += commands[t].size();
  }
  // std::cout << "# of commands = " << command_sum << std::endl;

  size_t num_v = 0;
  size_t num_vn = 0;
  size_t num_vt = 0;
  size_t num_f = 0;
  size_t num_faces = 0;
  for (size_t t = 0; t < num_threads; t++) {
    num_v += command_count[t].num_v;
    num_vn += command_count[t].num_vn;
    num_vt += command_count[t].num_vt;
    num_f += command_count[t].num_f;
    num_faces += command_count[t].num_faces;
  }
  // std::cout << "# v " << num_v << std::endl;
  // std::cout << "# vn " << num_vn << std::endl;
  // std::cout << "# vt " << num_vt << std::endl;
  // std::cout << "# f " << num_f << std::endl;

  // 4. merge
  // @todo { parallelize merge. }
  {
    auto t_start = std::chrono::high_resolution_clock::now();

    attrib->vertices.resize(num_v * 3);
    attrib->normals.resize(num_vn * 3);
    attrib->texcoords.resize(num_vt * 2);
    attrib->faces.resize(num_f);
    attrib->face_num_verts.resize(num_faces);
    attrib->material_ids.resize(num_faces);

    size_t v_offsets[kMaxThreads];
    size_t n_offsets[kMaxThreads];
    size_t t_offsets[kMaxThreads];
    size_t f_offsets[kMaxThreads];
    size_t face_offsets[kMaxThreads];

    v_offsets[0] = 0;
    n_offsets[0] = 0;
    t_offsets[0] = 0;
    f_offsets[0] = 0;
    face_offsets[0] = 0;

    for (size_t t = 1; t < num_threads; t++) {
      v_offsets[t] = v_offsets[t - 1] + command_count[t - 1].num_v;
      n_offsets[t] = n_offsets[t - 1] + command_count[t - 1].num_vn;
      t_offsets[t] = t_offsets[t - 1] + command_count[t - 1].num_vt;
      f_offsets[t] = f_offsets[t - 1] + command_count[t - 1].num_f;
      face_offsets[t] = face_offsets[t - 1] + command_count[t - 1].num_faces;
    }

    StackVector<std::thread, 16> workers;

    for (size_t t = 0; t < num_threads; t++) {
      int material_id = -1;  // -1 = default unknown material.
      workers->push_back(std::thread([&, t]() {
        size_t v_count = v_offsets[t];
        size_t n_count = n_offsets[t];
        size_t t_count = t_offsets[t];
        size_t f_count = f_offsets[t];
        size_t face_count = face_offsets[t];

        for (size_t i = 0; i < commands[t].size(); i++) {
          if (commands[t][i].type == COMMAND_EMPTY) {
            continue;
          } else if (commands[t][i].type == COMMAND_USEMTL) {
            if (commands[t][i].material_name &&
                commands[t][i].material_name_len > 0) {
              std::string material_name(commands[t][i].material_name,
                                        commands[t][i].material_name_len);

              if (material_map.find(material_name) != material_map.end()) {
                material_id = material_map[material_name];
              } else {
                // Assign invalid material ID
                material_id = -1;
              }
            }
          } else if (commands[t][i].type == COMMAND_V) {
            attrib->vertices[3 * v_count + 0] = commands[t][i].vx;
            attrib->vertices[3 * v_count + 1] = commands[t][i].vy;
            attrib->vertices[3 * v_count + 2] = commands[t][i].vz;
            v_count++;
          } else if (commands[t][i].type == COMMAND_VN) {
            attrib->normals[3 * n_count + 0] = commands[t][i].nx;
            attrib->normals[3 * n_count + 1] = commands[t][i].ny;
            attrib->normals[3 * n_count + 2] = commands[t][i].nz;
            n_count++;
          } else if (commands[t][i].type == COMMAND_VT) {
            attrib->texcoords[2 * t_count + 0] = commands[t][i].tx;
            attrib->texcoords[2 * t_count + 1] = commands[t][i].ty;
            t_count++;
          } else if (commands[t][i].type == COMMAND_F) {
            for (size_t k = 0; k < commands[t][i].f.size(); k++) {
              vertex_index &vi = commands[t][i].f[k];
              int v_idx = fixIndex(vi.v_idx, v_count);
              int vn_idx = fixIndex(vi.vn_idx, n_count);
              int vt_idx = fixIndex(vi.vt_idx, t_count);
              attrib->faces[f_count + k] = vertex_index(v_idx, vn_idx, vt_idx);
            }
            attrib->material_ids[face_count] = material_id;
            attrib->face_num_verts[face_count] = commands[t][i].f.size();

            f_count += commands[t][i].f.size();
            face_count++;
          }
        }
      }));
    }

    for (size_t t = 0; t < workers->size(); t++) {
      workers[t].join();
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    ms_merge = t_end - t_start;
  }

  auto t4 = std::chrono::high_resolution_clock::now();

  // 5. Construct shape information.
  {
    auto t_start = std::chrono::high_resolution_clock::now();

    // @todo { Can we boost the performance by multi-threaded execution? }
    int face_count = 0;
    shape_t shape;
    shape.face_offset = 0;
    shape.length = 0;
    int face_prev_offset = 0;
    for (size_t t = 0; t < num_threads; t++) {
      for (size_t i = 0; i < commands[t].size(); i++) {
        if (commands[t][i].type == COMMAND_O ||
            commands[t][i].type == COMMAND_G) {
          std::string name;
          if (commands[t][i].type == COMMAND_O) {
            name = std::string(commands[t][i].object_name,
                               commands[t][i].object_name_len);
          } else {
            name = std::string(commands[t][i].group_name,
                               commands[t][i].group_name_len);
          }

          if (face_count == 0) {
            // 'o' or 'g' appears before any 'f'
            shape.name = name;
            shape.face_offset = face_count;
            face_prev_offset = face_count;
          } else {
            if (shapes->size() == 0) {
              // 'o' or 'g' after some 'v' lines.
              // create a shape with null name
              shape.length = face_count - face_prev_offset;
              face_prev_offset = face_count;

              shapes->push_back(shape);

            } else {
              if ((face_count - face_prev_offset) > 0) {
                // push previous shape
                shape.length = face_count - face_prev_offset;
                shapes->push_back(shape);
                face_prev_offset = face_count;
              }
            }

            // redefine shape.
            shape.name = name;
            shape.face_offset = face_count;
            shape.length = 0;
          }
        }
        if (commands[t][i].type == COMMAND_F) {
          face_count++;
        }
      }
    }

    if ((face_count - face_prev_offset) > 0) {
      shape.length = face_count - shape.face_offset;
      if (shape.length > 0) {
        shapes->push_back(shape);
      }
    } else {
      // Guess no 'v' line occurrence after 'o' or 'g', so discards current
      // shape information.
    }

    auto t_end = std::chrono::high_resolution_clock::now();

    ms_construct = t_end - t_start;
  }

  std::chrono::duration<double, std::milli> ms_total = t4 - t1;
	if (option.verbose) {
		std::cout << "total parsing time: " << ms_total.count() << " ms\n";
		std::cout << "  line detection : " << ms_linedetection.count() << " ms\n";
		std::cout << "  alloc buf      : " << ms_alloc.count() << " ms\n";
		std::cout << "  parse          : " << ms_parse.count() << " ms\n";
		std::cout << "  merge          : " << ms_merge.count() << " ms\n";
		std::cout << "  construct      : " << ms_construct.count() << " ms\n";
		std::cout << "  mtl load       : " << ms_load_mtl.count() << " ms\n";
		std::cout << "# of vertices = " << attrib->vertices.size() << std::endl;
		std::cout << "# of normals = " << attrib->normals.size() << std::endl;
		std::cout << "# of texcoords = " << attrib->texcoords.size() << std::endl;
		std::cout << "# of face indices = " << attrib->faces.size() << std::endl;
		std::cout << "# of faces = " << attrib->material_ids.size() << std::endl;
		std::cout << "# of shapes = " << shapes->size() << std::endl;
	}

  return true;
}
#endif  // TINYOBJ_LOADER_OPT_IMPLEMENTATION

}  // namespace tinyobj_opt

#endif  // TINOBJ_LOADER_OPT_H_
