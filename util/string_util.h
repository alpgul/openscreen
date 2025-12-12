// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UTIL_STRING_UTIL_H_
#define UTIL_STRING_UTIL_H_

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// String query and manipulation utilities.
// TODO(jophba): remove nested string_util namespace.
namespace openscreen::string_util {

namespace internal {

extern const unsigned char kPropertyBits[256];
extern const char kToLower[256];
extern const char kToUpper[256];

}  // namespace internal

// Determines whether `c` is a valid ASCII alphabetic character code.
inline bool ascii_isalpha(unsigned char c) {
  return (internal::kPropertyBits[c] & 0x01) != 0;
}

// Determines whether `c` is a valid ASCII decimal digit (i.e. [0-9]).
inline bool ascii_isdigit(unsigned char c) {
  return '0' <= c && c <= '9';
}

// Determines whether `c` is a valid ASCII lower case hexadecimal digit
// (i.e. [a-fA-F0-9]).
inline bool ascii_islowerhex(unsigned char c) {
  return ascii_isdigit(c) || ('a' <= c && c <= 'f');
}

// Determines whether `c` is a valid ASCII hexadecimal digit (i.e. [a-fA-F0-9]).
inline bool ascii_ishex(unsigned char c) {
  return ascii_islowerhex(c) || ('A' <= c && c <= 'F');
}

// Determines whether `c` is a valid, printable ASCII digit.
inline bool ascii_isprint(unsigned char c) {
  return c >= 32 && c < 127;
}

// Determines whether `c` is a whitespace character
// (space, tab, vertical tab, formfeed, linefeed, or carriage return).
inline bool ascii_isspace(unsigned char c) {
  return (internal::kPropertyBits[c] & 0x08) != 0;
}

// If `c` is an upper case ASCII character, returns its lower case equivalent.
// Otherwise, returns `c` unchanged.
inline char ascii_tolower(unsigned char c) {
  return internal::kToLower[c];
}

// Converts `s` to lowercase.
void AsciiStrToLower(std::string& s);

// Creates a lowercase string from a given string_view.
std::string AsciiStrToLower(std::string_view s);

inline char ascii_toupper(unsigned char c) {
  return internal::kToUpper[c];
}

// Converts `s` to uppercase.
void AsciiStrToUpper(std::string& s);

// Creates a uppercase string from a given string_view.
std::string AsciiStrToUpper(std::string_view s);

// Returns whether a given string `text` begins with `prefix`.
//
// TODO(jophba): Replace with std::{string,string_view}::starts_with().
inline bool starts_with(std::string_view text, std::string_view prefix) {
  return prefix.empty() ||
         (text.size() >= prefix.size() &&
          memcmp(text.data(), prefix.data(), prefix.size()) == 0);
}

// Returns whether a given string `text` ends with `suffix`.
//
// TODO(jophba): Replace with std::{string,string_view}::ends_with().
inline bool ends_with(std::string_view text, std::string_view suffix) {
  return suffix.empty() || (text.size() >= suffix.size() &&
                            memcmp(text.data() + (text.size() - suffix.size()),
                                   suffix.data(), suffix.size()) == 0);
}

// Returns whether given ASCII strings `piece1` and `piece2` are equal, ignoring
// case in the comparison.
bool EqualsIgnoreCase(std::string_view piece1, std::string_view piece2);

// Returns std::string_view with whitespace stripped from the beginning of the
// given string_view.
inline std::string_view StripLeadingAsciiWhitespace(std::string_view str) {
  auto it = std::find_if_not(str.cbegin(), str.cend(), ascii_isspace);
  return str.substr(static_cast<size_t>(it - str.begin()));
}

// Concatenates arguments into a single string.
[[nodiscard]] std::string StrCat(
    std::initializer_list<std::string_view> pieces);

// Splits `value` into tokens separated by `delim`.  Leading and trailing
// delimeters are stripped, and multiple consecutive delimeters are treated as
// one.
[[nodiscard]] std::vector<std::string_view> Split(std::string_view value,
                                                  char delim);

// Returns a string made by concatenating the strings iterated by `[begin,
// end)`, each separated by `delim`.
template <typename Iterator>
[[nodiscard]] std::string Join(Iterator begin,
                               Iterator end,
                               std::string_view delim) {
  // Compute size of the result.
  size_t result_size = 0;
  for (auto it = begin; it != end; it++) {
    result_size += it->size() + delim.size();
  }
  std::string result;
  if (!result_size) {
    return result;
  } else {
    result_size -= delim.size();
  }
  result.reserve(result_size);

  // Populate the result, which should never allocate.
  for (auto it = begin; it != end; it++) {
    result.append(*it);
    if (it + 1 != end)
      result.append(delim);
  }
  return result;
}

}  // namespace openscreen::string_util

#endif  // UTIL_STRING_UTIL_H_
