//=== CanonicalLoop.cpp - High Level Canonical Loop Analyzer ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines classes to identify canonical for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#include "CanonicalLoop.h"
#include "tsar_dbg_output.h"
#include "DefinedMemory.h"
#include "DFRegionInfo.h"
#include "EstimateMemory.h"
#include "tsar_loop_matcher.h"
#include "MemoryAccessUtils.h"
#include "tsar_memory_matcher.h"
#include "SpanningTreeRelation.h"
#include "tsar_transformation.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <typeinfo>

using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "canonical-loop"

STATISTIC(NumCanonical, "Number of canonical  for-loops");
STATISTIC(NumNonCanonical, "Number of non-canonical for-loops");

char CanonicalLoopPass::ID = 0;
INITIALIZE_PASS_BEGIN(CanonicalLoopPass, "canonical-loop",
  "Canonical Form Loop Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(CanonicalLoopPass, "canonical-loop",
  "Canonical Form Loop Analysis", true, true)

namespace {
/// This class visits and analyzes all matched for-loops in a source code.
class CanonicalLoopLabeler : public MatchFinder::MatchCallback {
public:
  /// Creates visitor.
  explicit CanonicalLoopLabeler(DFRegionInfo &DFRI,
      const LoopMatcherPass::LoopMatcher &LM, DefinedMemoryInfo &DefI,
      const MemoryMatchInfo::MemoryMatcher &MM, AliasTree &AT,
      TargetLibraryInfo &TLI, tsar::CanonicalLoopInfo *CLI) :
      mRgnInfo(&DFRI), mLoopInfo(&LM), mDefInfo(&DefI), mMemoryMatcher(&MM),
      mAliasTree(AT), mTLI(TLI), mCanonicalLoopInfo(CLI),
      mSTR(SpanningTreeRelation<const AliasTree *>(&AT)) {}

  /// This function is called each time LoopMatcher finds appropriate loop
  virtual void run(const MatchFinder::MatchResult &Result) {
    const ForStmt *FS = Result.Nodes.getNodeAs<clang::ForStmt>("forLoop");
    ASTContext *Context = Result.Context;
    // We do not want to convert header files!
    if (!FS||!Context->getSourceManager().isWrittenInMainFile(FS->getForLoc()))
      return;
    const VarDecl *InitVar = Result.Nodes.getNodeAs
        <VarDecl>("InitVarName");
    assert(InitVar && "InitVar must not be null!");
    const VarDecl *UnIncVar = Result.Nodes.getNodeAs
        <VarDecl>("UnIncVarName");
    const VarDecl *BinIncVar = Result.Nodes.getNodeAs
        <VarDecl>("BinIncVarName");
    const clang::UnaryOperator *UnaryIncr = Result.Nodes.getNodeAs
        <clang::UnaryOperator>("UnaryIncr");
    const clang::BinaryOperator *BinaryIncr = Result.Nodes.getNodeAs
        <clang::BinaryOperator>("BinaryIncr");
    const VarDecl *AssignmentVar = Result.Nodes.getNodeAs
        <VarDecl>("AssignmentVarName");
    const VarDecl *FirstAssignmentVar = Result.Nodes.getNodeAs
        <VarDecl>("FirstAssignmentVarName");
    const VarDecl *SecondAssignmentVar = Result.Nodes.getNodeAs
        <VarDecl>("SecondAssignmentVarName");
    const VarDecl *FirstConditionVar = Result.Nodes.getNodeAs
        <VarDecl>("FirstConditionVarName");
    const clang::BinaryOperator *Condition = Result.Nodes.getNodeAs
        <clang::BinaryOperator>("Condition");
    assert(Condition && "Condition must not be null!");
    const VarDecl *SecondConditionVar = Result.Nodes.getNodeAs
        <VarDecl>("SecondConditionVarName");
    bool CheckCondVar = false;
    bool ReversedCond = false;
    if (FirstConditionVar && sameVar(InitVar, FirstConditionVar))
      CheckCondVar = true;
    if (SecondConditionVar && sameVar(InitVar, SecondConditionVar)) {
      CheckCondVar = true;
      ReversedCond = true;
    }
    if (((UnIncVar && sameVar(UnIncVar, InitVar)) ||
        (BinIncVar && sameVar(BinIncVar, InitVar)) ||
        (AssignmentVar && sameVar(AssignmentVar, InitVar) &&
        ((FirstAssignmentVar && sameVar(InitVar, FirstAssignmentVar)) ||
        (SecondAssignmentVar && sameVar(InitVar, SecondAssignmentVar))))) &&
        (CheckCondVar &&
        ((UnaryIncr && coherent(UnaryIncr, Condition, ReversedCond)) ||
        (BinaryIncr && coherent(BinaryIncr, Condition, ReversedCond))))) {
      auto Match = mLoopInfo->find<AST>(const_cast<ForStmt*>(FS));
      assert(Match != mLoopInfo->end() && "ForStmt must be specified!");
      tsar::DFNode* Region = mRgnInfo->getRegionFor(Match->get<IR>());
      if (checkLoop(Region, const_cast<VarDecl*>
          (InitVar->getCanonicalDecl()))) {
        auto PLInfo = mCanonicalLoopInfo->insert(Region);
        ++NumCanonical;
        return;
      }
    }
    ++NumNonCanonical;
  }

private:
  /// Checks whether two ValueDecl declares same variable or not
  bool sameVar(const ValueDecl *First, const ValueDecl *Second) {
    return First->getCanonicalDecl() == Second->getCanonicalDecl();
  }
  
  /// Checks coherence of increment and condition
  bool coherent(const clang::UnaryOperator *Incr,
      const clang::BinaryOperator *Condition, bool ReversedCond) {
    bool Increment = false;
    if (Incr->getOpcodeStr(Incr->getOpcode()) == "++")
      Increment = true;
    bool LessCondition = false;
    if ((Condition->getOpcodeStr(Condition->getOpcode()) == "<") ||
        (Condition->getOpcodeStr(Condition->getOpcode()) == "<="))
      LessCondition = true;
    if (Increment && LessCondition && !(ReversedCond) ||
        Increment && !(LessCondition) && ReversedCond ||
        !(Increment) && !(LessCondition) && !(ReversedCond) ||
        !(Increment) && LessCondition && ReversedCond)
      return true;
    return false;
  }
  
  bool coherent(const clang::BinaryOperator *Incr,
      const clang::BinaryOperator *Condition, bool ReversedCond) {
    bool Increment = false;
    if ((Incr->getOpcodeStr(Incr->getOpcode()) == "+") ||
        (Incr->getOpcodeStr(Incr->getOpcode()) == "+="))
      Increment = true;
    bool LessCondition = false;
    if ((Condition->getOpcodeStr(Condition->getOpcode()) == "<") ||
        (Condition->getOpcodeStr(Condition->getOpcode()) == "<="))
      LessCondition = true;
    if (Increment && LessCondition && !(ReversedCond) ||
        Increment && !(LessCondition) && ReversedCond ||
        !(Increment) && !(LessCondition) && !(ReversedCond) ||
        !(Increment) && LessCondition && ReversedCond)
      return true;
    return false;
  }

  // Checks if Loc is Def or MayDef in L
  bool CheckMemLoc(MemoryLocation &Loc, Loop *L) {
    auto DFN = mRgnInfo->getRegionFor(L);
    assert(DFN && "DFNode must not be null!");
    auto Match = mDefInfo->find(DFN);
    assert(Match != mDefInfo->end() && Match->get<DefUseSet>() &&
        "Data-flow value must be specified!");
    auto &DUS = Match->get<DefUseSet>();
    return !((DUS->hasDef(Loc)) || (DUS->hasMayDef(Loc)));
  }

  /// Finds last instruction of block w/ inductive variable
  Instruction* FindLastInstruction(AliasEstimateNode *ANI, BasicBlock *BB) {
    Instruction *LastInstruction = nullptr;
    auto I = BB->rbegin();
    auto InstrEnd = BB->rend();
    while ((I != InstrEnd) && (!(LastInstruction))) {
      bool RequiredInstruction = false;
      for_each_memory(*I, mTLI,
        [this, &ANI, &RequiredInstruction] (Instruction &I,
            MemoryLocation &&Loc, unsigned Idx, AccessInfo, AccessInfo W) {
          auto EM = mAliasTree.find(Loc);
          assert(EM && "Estimate memory location must not be null!");
          auto AN = EM->getAliasNode(mAliasTree);
          assert(AN && "Alias node must not be null!");
          if (!mSTR.isUnreachable(ANI, AN) && (W != AccessInfo::No)) {
            RequiredInstruction = true;
          }
        },
        [this, &ANI, &RequiredInstruction] (Instruction &I, AccessInfo,
            AccessInfo W) {
          auto AN = mAliasTree.findUnknown(I);
          if (!AN)
            return;
          if (!mSTR.isUnreachable(ANI, AN) && (W != AccessInfo::No)) {
            RequiredInstruction = true;
          }
        }
      );
      if (RequiredInstruction)
        LastInstruction = &(*I);
      ++I;
    }
    return LastInstruction;
  }

  /// Checks if operands of inductive variable are static
  bool CheckMemLocsFromInstr(Instruction *I, EstimateMemory *EMI, Loop *L) {
    bool Result = true;
    for_each_memory(*I, mTLI,
      [this, &EMI, &L, &Result] (Instruction &Instr,
          MemoryLocation &&Loc, unsigned Idx, AccessInfo R, AccessInfo W) {
        printLocationSource(dbgs(), Loc);
        auto EM = mAliasTree.find(Loc);
        assert(EM && "Estimate memory location must not be null!");
        if (EM == EMI)
          return;
        Result &= CheckMemLoc(Loc, L);
      },
      [this, &EMI, &L, &Result] (Instruction &Instr,
          AccessInfo, AccessInfo) {
        // don't know what to do here
      }
    );
    if (!Result)
      return false;
    auto OpE = I->op_end();
    for (auto J = I->op_begin(); J != OpE; ++J)
      if (auto Instr = llvm::dyn_cast<Instruction>(*J))
        if (!CheckMemLocsFromInstr(Instr, EMI, L))
          return false;
    return true;
  }
  
  bool CheckMemLocsFromBlock(BasicBlock *BB, EstimateMemory *EMI, Loop *L) {
    bool Result = true;
    for_each_memory(*BB, mTLI,
      [this, &EMI, &L, &Result] (Instruction &Instr,
          MemoryLocation &&Loc, unsigned Idx, AccessInfo R, AccessInfo W) {
        printLocationSource(dbgs(), Loc);
        auto EM = mAliasTree.find(Loc);
        assert(EM && "Estimate memory location must not be null!");
        if (EM == EMI)
          return;
        Result &= CheckMemLoc(Loc, L);
      },
      [this, &EMI, &L, &Result] (Instruction &Instr,
          AccessInfo, AccessInfo) {
        // don't know what to do here
      }
    );
    return Result;
  }
  
  // ToDo: delete this after the finish of another checks
  bool CheckFuncCallsFromBlock(BasicBlock *BB) {
    BB->dump();
    auto &AA = mAliasTree.getAliasAnalysis();
    for (auto &Inst : *BB) {
      if (llvm::isa<CallInst>(Inst)) {
        CallInst &CI = llvm::cast<CallInst>(Inst);
        llvm::ImmutableCallSite ICS(&CI);
        if (!(AA.onlyReadsMemory(ICS)))
          return false;
      }
      if (llvm::isa<InvokeInst>(Inst)) {
        InvokeInst &CI = llvm::cast<InvokeInst>(Inst);
        llvm::ImmutableCallSite ICS(&CI);
        if (!(AA.onlyReadsMemory(ICS)))
          return false;
      }
    }
    return true;
  }

  /// Checks if labeled loop is canonical
  bool checkLoop(tsar::DFNode* Region, VarDecl *Var) {
    auto MemMatch = mMemoryMatcher->find<AST>(Var);
    if (MemMatch == mMemoryMatcher->end()) {
      return false;
    }
    auto alloca = MemMatch->get<IR>();
    if (!(alloca->getType() && alloca->getType()->isPointerTy())) {
      return false;
    }
    llvm::MemoryLocation MemLoc(alloca, 1);
    auto EMI = mAliasTree.find(MemLoc);
    assert(EMI && "Estimate memory location must not be null!");
    auto ANI = EMI->getAliasNode(mAliasTree);
    assert(ANI && "Alias node must not be null!");
    tsar::DFLoop* DFFor = llvm::dyn_cast<DFLoop>(Region);
    assert(DFFor && "DFNode must not be null!");
    llvm::Loop *LLoop = DFFor->getLoop();
    auto BlocksEnd = LLoop->block_end();
    for (auto I = LLoop->block_begin(); I != BlocksEnd; ++I) {
      auto DFN = mRgnInfo->getRegionFor(*I);
      assert(DFN && "DFNode must not be null!");
      auto Match = mDefInfo->find(DFN);
      assert(Match != mDefInfo->end() && Match->get<ReachSet>() &&
          Match->get<DefUseSet>() && "Data-flow value must be specified!");
      auto &DUS = Match->get<DefUseSet>();
      if (!((DUS->hasDef(MemLoc)) || (DUS->hasMayDef(MemLoc))))
        continue;
      tsar::DFBlock* DFB = llvm::dyn_cast<DFBlock>(DFN);
      if (!DFB)
        continue;
      if (!(DFFor->getLoop()->isLoopLatch(DFB->getBlock())))
        return false;
      int NumOfWrites = 0;
      for_each_memory(*(DFB->getBlock()), mTLI,
        [this, &ANI, &NumOfWrites] (Instruction &I,
            MemoryLocation &&Loc, unsigned Idx, AccessInfo, AccessInfo W) {
          auto EM = mAliasTree.find(Loc);
          assert(EM && "Estimate memory location must not be null!");
          auto AN = EM->getAliasNode(mAliasTree);
          assert(AN && "Alias node must not be null!");
          if (!(mSTR.isUnreachable(ANI, AN)) && (W != AccessInfo::No))
            ++NumOfWrites;
        },
        [this, &ANI, &NumOfWrites] (Instruction &I, AccessInfo,
            AccessInfo W) {
          auto AN = mAliasTree.findUnknown(I);
          if (!AN)
            return;
          if (!(mSTR.isUnreachable(ANI, AN)) && (W != AccessInfo::No))
            ++NumOfWrites;
        }
      );
      if (NumOfWrites != 1)
        return false;
    }
    Instruction *LI = FindLastInstruction(ANI, LLoop->getLoopPreheader());
    assert(LI && "Last instruction should not be nullptr!");
    if (!CheckMemLocsFromInstr(LI, EMI, LLoop))
      return false;
    LI = FindLastInstruction(ANI, LLoop->getLoopLatch());
    assert(LI && "Last instruction should not be nullptr!");
    if (!CheckMemLocsFromInstr(LI, EMI, LLoop))
      return false;
    if (!CheckFuncCallsFromBlock(LLoop->getLoopLatch()))
      return false;
    if (!CheckMemLocsFromBlock(LLoop->getHeader(), EMI, LLoop))
      return false;
    if (!CheckFuncCallsFromBlock(LLoop->getHeader()))
      return false;
    return true;
  }
  
  DFRegionInfo *mRgnInfo;
  const LoopMatcherPass::LoopMatcher *mLoopInfo;
  DefinedMemoryInfo *mDefInfo;
  const MemoryMatchInfo::MemoryMatcher *mMemoryMatcher;
  tsar::AliasTree &mAliasTree;
  TargetLibraryInfo &mTLI;
  tsar::CanonicalLoopInfo *mCanonicalLoopInfo;
  SpanningTreeRelation<const AliasTree *> mSTR;
};

/// returns LoopMatcher that matches loops that can be canonical
DeclarationMatcher makeLoopMatcher() {
  return functionDecl(forEachDescendant(
      forStmt(
        hasLoopInit(eachOf(
          declStmt(hasSingleDecl(
            varDecl(hasType(isInteger()))
            .bind("InitVarName"))),
          binaryOperator(
            hasOperatorName("="),
            hasLHS(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("InitVarName"))))))),
        hasIncrement(eachOf(
          unaryOperator(
            eachOf(
              hasOperatorName("++"),
              hasOperatorName("--")),
            hasUnaryOperand(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("UnIncVarName"))))).bind("UnaryIncr"),
          binaryOperator(
            eachOf(
              hasOperatorName("+="),
              hasOperatorName("-=")),
            hasLHS(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("BinIncVarName"))))).bind("BinaryIncr"),
          binaryOperator(
            hasOperatorName("="),
            hasLHS(declRefExpr(to(
              varDecl(hasType(isInteger()))
              .bind("AssignmentVarName")))),
            hasRHS(eachOf(
              binaryOperator(
                hasOperatorName("+"),
                eachOf(
                  hasLHS(implicitCastExpr(
                    hasImplicitDestinationType(isInteger()),
                    hasSourceExpression(declRefExpr(to(
                      varDecl(hasType(isInteger()))
                      .bind("FirstAssignmentVarName")))))),
                  hasRHS(implicitCastExpr(
                    hasImplicitDestinationType(isInteger()),
                    hasSourceExpression(declRefExpr(to(
                      varDecl(hasType(isInteger()))
                      .bind("SecondAssignmentVarName")))))))).bind("BinaryIncr"),
              binaryOperator(
                hasOperatorName("-"),
                hasLHS(implicitCastExpr(
                  hasImplicitDestinationType(isInteger()),
                  hasSourceExpression(declRefExpr(to(
                    varDecl(hasType(isInteger()))
                    .bind("FirstAssignmentVarName"))))))).bind("BinaryIncr")))))),
        hasCondition(binaryOperator(
          eachOf(
            hasOperatorName("<"),
            hasOperatorName("<="),
            hasOperatorName("="),
            hasOperatorName(">="),
            hasOperatorName(">")),
          eachOf(
            hasLHS(implicitCastExpr(
              hasImplicitDestinationType(isInteger()),
              hasSourceExpression(declRefExpr(to(
                varDecl(hasType(isInteger()))
                .bind("FirstConditionVarName")))))),
            hasRHS(implicitCastExpr(
              hasImplicitDestinationType(isInteger()),
              hasSourceExpression(declRefExpr(to(
                varDecl(hasType(isInteger()))
                .bind("SecondConditionVarName")))))))).bind("Condition")))
      .bind("forLoop")));
}
}

bool CanonicalLoopPass::runOnFunction(Function &F) {
  auto M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  auto &RgnInfo = getAnalysis<DFRegionInfoPass>().getRegionInfo();
  auto &LoopInfo = getAnalysis<LoopMatcherPass>().getMatcher();
  auto &DefInfo = getAnalysis<DefinedMemoryPass>().getDefInfo();
  auto &MemInfo =
    getAnalysis<MemoryMatcherImmutableWrapper>()->Matcher;
  auto &ATree = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  DeclarationMatcher LoopMatcher = makeLoopMatcher();
  CanonicalLoopLabeler Labeler(RgnInfo, LoopInfo, DefInfo, MemInfo, ATree,
      TLI, &mCanonicalLoopInfo);
  auto &Context = FuncDecl->getASTContext();
  auto Nodes = match<DeclarationMatcher, Decl>(LoopMatcher, *FuncDecl, Context);
  while (!Nodes.empty()) {
    MatchFinder::MatchResult Result(Nodes.back(), &Context);
    Labeler.run(Result);
    Nodes.pop_back();
  }
  return false;
}

void CanonicalLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<LoopMatcherPass>();
  AU.addRequired<DefinedMemoryPass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createCanonicalLoopPass() {
  return new CanonicalLoopPass();
}