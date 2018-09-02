//===- GlobalInfoExtractor.cpp - AST Based Global Information ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements functionality to collect global information about the
// whole translation unit.
//
//===----------------------------------------------------------------------===//

#include "GlobalInfoExtractor.h"
#include "SourceLocationTraverse.h"
#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Debug.h>

#ifdef DEBUG
#  include <llvm/Support/raw_ostream.h>
#  include <clang/Lex/Lexer.h>
#endif

using namespace clang;
using namespace llvm;
using namespace tsar;

#define DEBUG_TYPE "ast-global-info"

bool GlobalInfoExtractor::VisitStmt(Stmt *S) {
  traverseSourceLocation(S, [this](SourceLocation Loc) { visitLoc(Loc); });
  return RecursiveASTVisitor::VisitStmt(S);
}

bool GlobalInfoExtractor::VisitTypeLoc(TypeLoc TL) {
  traverseSourceLocation(TL, [this](SourceLocation Loc) { visitLoc(Loc); });
  return RecursiveASTVisitor::VisitTypeLoc(TL);
}

bool GlobalInfoExtractor::TraverseDecl(Decl *D) {
  if (isa<TranslationUnitDecl>(D))
    return RecursiveASTVisitor::TraverseDecl(D);
  traverseSourceLocation(D, [this](SourceLocation Loc) { visitLoc(Loc); });
#ifdef DEBUG
  auto log = [D, this]() {
    dbgs() << "[GLOBAL INFO]: global declaration with name "
      << cast<NamedDecl>(D)->getName() << " has outermost declaration at ";
    mOutermostDecl->getLocation().dump(mSM);
    dbgs() << "\n";
  };
#endif
  if (!mOutermostDecl) {
    mOutermostDecl = D;
    if (auto ND = dyn_cast<NamedDecl>(D)) {
      mOutermostDecls.try_emplace(ND, mOutermostDecl);
      DEBUG(log());
    }
    auto Res = RecursiveASTVisitor::TraverseDecl(D);
    mOutermostDecl = nullptr;
    return Res;
  }
  if (!mLangOpts.CPlusPlus && isa<TagDecl>(mOutermostDecl) && isa<TagDecl>(D)) {
    mOutermostDecls.try_emplace(cast<NamedDecl>(D), mOutermostDecl);
    DEBUG(log());
  }
  return RecursiveASTVisitor::TraverseDecl(D);
}

void GlobalInfoExtractor::collectIncludes(FileID FID) {
  while (FID.isValid()) {
    // Warning, we use getFileEntryForID() to check that buffer is valid
    // because it seems that dereference of nullptr may occur in getBuffer()
    // (getFile().getContentCash() may return nullptr). It seems that there is
    // an error in getBuffer() method.
    if (mSM.getFileEntryForID(FID)) {
      bool F = mFiles.try_emplace(mSM.getBuffer(FID), FID).second;
      if (F) {
        DEBUG(dbgs() << "[GLOBAL INFO]: visited file "
          << mSM.getFileEntryForID(FID)->getName() << "\n");
      }
    }
    auto IncLoc = mSM.getIncludeLoc(FID);
    if (IncLoc.isValid()) {
      bool F = mVisitedIncludeLocs.insert(IncLoc.getRawEncoding()).second;
      if (F) {
        DEBUG(dbgs() << "[GLOBAL INFO]: visited #include location ";
          IncLoc.dump(mSM); dbgs() << "\n");
      }
    }
    FID = mSM.getFileID(mSM.getIncludeLoc(FID));
  }
}

 void GlobalInfoExtractor::visitLoc(SourceLocation Loc) {
  if (Loc.isInvalid())
    return;
  auto ExpLoc = mSM.getExpansionLoc(Loc);
  mVisitedExpLocs.insert(ExpLoc.getRawEncoding());
  auto FID = mSM.getFileID(ExpLoc);
  collectIncludes(FID);
  if (Loc.isFileID())
    return;
  SmallVector<SourceLocation, 8> LocationStack;
  while (Loc.isMacroID()) {
    // If this is the expansion of a macro argument, point the caret at the
    // use of the argument in the definition of the macro, not the expansion.
    if (mSM.isMacroArgExpansion(Loc)) {
      auto ArgInMacroLoc = mSM.getImmediateExpansionRange(Loc).first;
      LocationStack.push_back(ArgInMacroLoc);
      // Remember file which contains macro definition.
      auto FID = mSM.getFileID(mSM.getSpellingLoc(ArgInMacroLoc));
      collectIncludes(FID);
    } else {
      LocationStack.push_back(Loc);
      // Remember file which contains macro definition.
      auto FID = mSM.getFileID(mSM.getSpellingLoc(Loc));
      collectIncludes(FID);
    }
    Loc = mSM.getImmediateMacroCallerLoc(Loc);
    // Once the location no longer points into a macro, try stepping through
    // the last found location.  This sometimes produces additional useful
    // backtraces.
    if (Loc.isFileID()) {
      Loc = mSM.getImmediateMacroCallerLoc(LocationStack.back());
    }
    assert(Loc.isValid() && "Must have a valid source location here!");
  }
  DEBUG(
    dbgs() << "[GLOBAL INFO]: expanded macros:\n";
    for (auto Loc : LocationStack) {
      dbgs() << "  " << Lexer::getImmediateMacroNameForDiagnostics(
        Loc, mSM, mLangOpts) << " at";
      Loc.dump(mSM);
      dbgs() << "\n";
    });
}