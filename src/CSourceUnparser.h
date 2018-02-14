//===--- CSourceUnparser.h -- C Source Info Unparser ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines unparser to print metadata objects as constructs in C/C++.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_C_SOURCE_UNPARSER_H
#define TSAR_C_SOURCE_UNPARSER_H

#include "SourceUnparser.h"
#include <llvm/ADT/APInt.h>

namespace tsar {
/// This is an unparser for C and C++ languages.
class CSourceUnparser : public SourceUnparser<CSourceUnparser> {
public:
  ///Creates unparser for a specified expression.
  explicit CSourceUnparser(const DIMemoryLocation &Loc) noexcept :
    SourceUnparser(Loc, true) {}

private:
  friend SourceUnparser<CSourceUnparser>;

  void appendToken(Token T, bool IsSubscript,
      llvm::SmallVectorImpl<char> &Str) {
    switch (T) {
    default: llvm_unreachable("Unsupported kind of token!"); break;
    case TOKEN_ADDRESS: Str.push_back('&'); break;
    case TOKEN_DEREF: Str.push_back('*'); break;
    case TOKEN_PARENTHESES_LEFT: Str.push_back('('); break;
    case TOKEN_PARENTHESES_RIGHT: Str.push_back(')'); break;
    case TOKEN_ADD: Str.push_back('+'); break;
    case TOKEN_SUB: Str.push_back('-'); break;
    case TOKEN_FIELD: Str.push_back('.'); break;
    case TOKEN_UNKNOWN: Str.push_back('?'); break;
    case TOKEN_SEPARATOR:
      if (IsSubscript)
        Str.append({ ']', '[' });
      break;
    case TOKEN_CAST_TO_ADDRESS:
      Str.append({ '(', 'c', 'h', 'a', 'r', '*', ')' }); break;
    }
  }

    void appendUConst(
      uint64_t C, bool IsSubscript, llvm::SmallVectorImpl<char> &Str) {
    llvm::APInt Const(64, C, false);
    Const.toString(Str, 10, false);
  }


  void beginSubscript(llvm::SmallVectorImpl<char> &Str) {
    Str.push_back('[');
  }

  void endSubscript(llvm::SmallVectorImpl<char> &Str) {
    Str.push_back(']');
  }
};
}
#endif//TSAR_C_SOURCE_UNPARSER_H