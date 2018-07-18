/*
  This file is part of Chinese Chess Zero.
  Copyright (C) 2018 The CCZero Authors

  Chinese Chess Zero is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Chinese Chess Zero is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Chinese Chess Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstdint>
#include <initializer_list>

#pragma once
namespace cczero {

// Tries to scramble @val.
inline uint64_t Hash(uint64_t val) {
  return 0xfad0d7f2fbb059f1ULL * (val + 0xbaad41cdcb839961ULL) +
         0x7acec0050bf82f43ULL * ((val >> 31) + 0xd571b3a92b1b2755ULL);
}

// Appends value to a hash.
inline uint64_t HashCat(uint64_t hash, uint64_t x) {
  hash ^= 0x299799adf0d95defULL + Hash(x) + (hash << 6) + (hash >> 2);
  return hash;
}

// Combines 64-bit values into concatenated hash.
inline uint64_t HashCat(std::initializer_list<uint64_t> args) {
  uint64_t hash = 0;
  for (uint64_t x : args) hash = HashCat(hash, x);
  return hash;
}

}  // namespace cczero
