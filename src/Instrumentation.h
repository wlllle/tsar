#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

#include <llvm/IR/InstVisitor.h>
#include <llvm/Analysis/LoopInfo.h>
#include "Intrinsics.h"
#include "Registrator.h"
#include <sstream>

class Instrumentation :public llvm::InstVisitor<Instrumentation> {
public:
  Instrumentation(llvm::LoopInfo &LI, Registrator &R, llvm::Function &F);

  void visitAllocaInst(llvm::AllocaInst &I);
  void visitLoadInst(llvm::LoadInst &I);
  void visitStoreInst(llvm::StoreInst &I);
  void visitCallInst(llvm::CallInst &I);
  void visitInvokeInst(llvm::InvokeInst &I);
  void visitReturnInst(llvm::ReturnInst &I);
  void visitBasicBlock(llvm::BasicBlock &B);
  void visitFunction(llvm::Function &F);
private:
  llvm::LoopInfo& mLoopInfo;
  Registrator& mRegistrator;

  //visitCallInst and visiInvokeInst have completely the same code
  //so template for them
  //
  //NOTE: Instead of template it was possible to overload visitCallSite which 
  //is for both calls and invokes. Maybe i'll change it later. 
  template<class T>
  void FunctionCallInst(T &I) {
    //not llvm function
    if(I.getCalledFunction()->isIntrinsic())
      return;
    //not tsar function
    tsar::IntrinsicId Id;
    if(getTsarLibFunc(I.getCalledFunction()->getName(), Id)) {
      return;
    }
    std::stringstream DebugStr;
    DebugStr << "type=func_call*file=" << I.getModule()->getSourceFileName()
      << "*line1=" << I.getDebugLoc()->getLine() << "*name1=" << 
      I.getCalledFunction()->getSubprogram()->getName().data() << "*rank=" << 
      I.getCalledFunction()->getFunctionType()->getNumParams() << "**";
    auto DICall = prepareStrParam(DebugStr.str(), I);
    auto Fun = getDeclaration(I.getModule(),tsar::IntrinsicId::func_call_begin);
    llvm::CallInst::Create(Fun, {DICall}, "", &I);
    Fun = getDeclaration(I.getModule(), tsar::IntrinsicId::func_call_end);
    auto Call = llvm::CallInst::Create(Fun, {DICall}, "");
    Call->insertAfter(&I);
  }

  void loopBeginInstr(llvm::Loop const *L, llvm::BasicBlock& Header);
  void loopEndInstr(llvm::Loop const *L, llvm::BasicBlock& Header);
  void loopIterInstr(llvm::Loop const *L, llvm::BasicBlock& Header);

  llvm::GetElementPtrInst* prepareStrParam(const std::string& S, 
    llvm::Instruction &I);
};

#endif // INSTRUMENTATION_H