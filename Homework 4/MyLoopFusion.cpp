#include "llvm/Transforms/Utils/MyLoopFusion.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"

using namespace llvm;

BasicBlock* MyLoopFusion::getLoopHead(Loop *L) {
    return L->isGuarded() ?
        L->getLoopGuardBranch()->getParent() : L->getLoopPreheader();
}

BasicBlock* MyLoopFusion::getLoopExit(Loop *L) {
    if(L->isGuarded()) {
        auto *B = L->getLoopGuardBranch();

        if(!B || !B->isConditional()) {

        for(unsigned int i = 0; i < B->getNumSuccessors();i++) {
            if(!L->contains(B->getSuccessor(i))) return B->getSuccessor(i);
        }

        }
    }
    else {
        return L->getExitBlock();
    }




}

bool MyLoopFusion::areLoopsAdjacent(Loop *Lprev, Loop *Lnext) {
    auto LprevExit = getLoopExit(Lprev);
    auto LnextHead = getLoopHead(Lnext);

    return ((LprevExit == LnextHead));
}

bool MyLoopFusion::areLoopsCFE(Loop *Lprev, Loop *Lnext, Function &F, FunctionAnalysisManager &FAM) {

    BasicBlock* LprevHead = getLoopHead(Lprev);
    BasicBlock* LnextHead = getLoopHead(Lnext);

    DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    PostDominatorTree &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);

    return DT.dominates(LprevHead,LnextHead) && PDT.dominates(LnextHead,LprevHead);

}

bool MyLoopFusion::areLoopsTCE(Loop *Lprev, Loop *Lnext, Function &F, FunctionAnalysisManager &FAM) {

    ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
    auto LprevTC = SE.getBackedgeTakenCount(Lprev);
    auto LnextTC = SE.getBackedgeTakenCount(Lnext);

    if(isa<SCEVCouldNotCompute>(LprevTC) || isa<SCEVCouldNotCompute>(LnextTC)){
        return false;
    }
    else if (SE.isKnownPredicate(CmpInst::ICMP_EQ,LprevTC,LnextTC))
    {
        return true;
    }


    return false;
}

bool MyLoopFusion::areLoopsIndependent(Loop *Lprev, Loop *Lnext, Function &F, FunctionAnalysisManager &FAM) {
    DependenceInfo &DI = FAM.getResult<DependenceAnalysis>(F);

    for(auto *BB : Lprev->getBlocks()) {
        for(auto &I : *BB) {
            for(auto *BB2 : Lnext->getBlocks()) {
                for(auto &I2 : *BB2) {
                    if (((isa<StoreInst>(&I)) && (isa<LoadInst>(&I2) || isa<StoreInst>(&I2))) && ((isa<StoreInst>(&I2)) && (isa<LoadInst>(&I) || isa<StoreInst>(&I))) ) {
                        if(DI.depends(&I,&I2,true)) return false;

                    }

                }
            }

        }
    }

    return true;

}

PHINode* MyLoopFusion::getIVForNonRotatedLoops(Loop *L, ScalarEvolution &SE) {

    if(L->isCanonical(SE)) return L->getCanonicalInductionVariable();

    for(auto &PHI : L->getHeader()->phis()) {
        const SCEV *PHISCEV = SE.getSCEV(&PHI);

        if(auto *PHIasADDREC = dyn_cast<SCEVAddRecExpr>(PHISCEV)) {
            if(PHIasADDREC->getLoop() == L) {
                return &PHI;
            }
        }
    }

    return nullptr;
}

Loop* MyLoopFusion::merge(Loop *Lprev, Loop *Lnext, Function &F, FunctionAnalysisManager &FAM){
    ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
    

    BasicBlock *PL = Lprev->getLoopLatch();
    BasicBlock* PB = PL->getSinglePredecessor();
    BasicBlock *PH = Lprev->getHeader();
    BasicBlock *PE = Lprev->getExitBlock();

  
    BasicBlock *NL = Lnext->getLoopLatch();
    BasicBlock* NB = NL->getSinglePredecessor();
    BasicBlock *NH = Lnext->getHeader();
    BasicBlock *NE = Lnext->getExitBlock();


    auto PIV = getIVForNonRotatedLoops(Lprev,SE);
    auto NIV = getIVForNonRotatedLoops(Lnext,SE);

    NIV->replaceAllUsesWith(PIV);
    NIV->removeFromParent();

    PH->getTerminator()->replaceSuccessorWith(PE,NE);
    PB->getTerminator()->replaceSuccessorWith(PL,NB);
    NB->getTerminator()->replaceSuccessorWith(NL,PL);
    NH->getTerminator()->replaceSuccessorWith(NB,NL);

    Lprev->addBasicBlockToLoop(NB,LI);
    Lnext->removeBlockFromLoop(NB);
    LI.erase(Lnext);
    EliminateUnreachableBlocks(F);

    return Lprev;

}

PreservedAnalyses MyLoopFusion::run(Function &F, FunctionAnalysisManager &FAM) {
    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
    
    std::vector<Loop*> TLL = LI.getTopLevelLoops();
    std::reverse(TLL.begin(),TLL.end());
    Loop* Lprev = nullptr;
    bool hasBeenOptimized = false;
    for(auto iter = TLL.begin(); iter != TLL.end(); ++iter) {

        Loop* L = *iter;

        if(Lprev) {
            if(areLoopsAdjacent(Lprev,L) && areLoopsTCE(Lprev,L,F,FAM) && areLoopsCFE(Lprev,L,F,FAM) && areLoopsIndependent(Lprev,L,F,FAM)) {
                hasBeenOptimized = true;
                Lprev = merge(Lprev,L,F,FAM);
            }
            else {
                Lprev = L;
            }
        }
        else {
            Lprev = L;
        }
    }


    
    return hasBeenOptimized ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

 