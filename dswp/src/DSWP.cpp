#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/AssumptionCache.h"

#include "llvm/IR/Mangler.h"
#include "llvm/IR/IRBuilder.h"

#include "../include/LoopDependenceInfo.hpp"
#include "PDG.hpp"
#include "PDGAnalysis.hpp"

#include <unordered_map>

using namespace llvm;

namespace llvm {

  struct DSWP : public ModulePass {
    public:
      static char ID;

      Function *queuePushTemporary, *queuePopTemporary, *stageHandler;
      FunctionType *stageType;

      DSWP() : ModulePass{ID} {}

      bool doInitialization (Module &M) override {
        return false;
      }

      bool runOnModule (Module &M) override {
        errs() << "DSWP for " << M.getName() << "\n";
        queuePushTemporary = M.getFunction("queuePush"); // M.getFunction("_Z9queuePushP15ThreadSafeQueueIiEi");
        queuePopTemporary = M.getFunction("queuePop"); // M.getFunction("_Z8queuePopP15ThreadSafeQueueIiE");
        stageHandler = M.getFunction("parallelizeHandler"); // M.getFunction("_Z18parallelizeHandlerPFiP15ThreadSafeQueueIiEES3_");

        stageType = cast<FunctionType>(cast<PointerType>(stageHandler->arg_begin()->getType())->getElementType());
        stageType->print(errs() << "sT:\t"); errs() << "\n";
        /*
         * Fetch the PDG.
         */
        auto graph = getAnalysis<PDGAnalysis>().getPDG();

        /*
         * Fetch the loop to parallelize
         */
        auto loopDI = fetchLoopToParallelize(M, graph);
        if (loopDI == nullptr){
          return false;
        }

        /*
         * Parallelize the loop
         */
        auto modified = applyDSWP(loopDI);

        delete loopDI;

        return modified;
      }

      void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.addRequired<PDGAnalysis>();
        AU.addRequired<AssumptionCacheTracker>();
        AU.addRequired<DominatorTreeWrapperPass>();
        AU.addRequired<LoopInfoWrapperPass>();
        AU.addRequired<ScalarEvolutionWrapperPass>();
        return ;
      }

    private:
      LoopDependenceInfo *fetchLoopToParallelize (Module &M, PDG *graph){

        /* 
         * ASSUMPTION 1: One function in the entire program.
         * Fetch the entry point of the program.
         */
        auto entryFunction = M.getFunction("main");

        /*
         * Fetch the loops.
         */
        
        auto &LI = getAnalysis<LoopInfoWrapperPass>(*entryFunction).getLoopInfo();
        auto &DT = getAnalysis<DominatorTreeWrapperPass>(*entryFunction).getDomTree();
        auto &SE = getAnalysis<ScalarEvolutionWrapperPass>(*entryFunction).getSE();

        /*
         * ASSUMPTION 2: One loop in the entire function 
         * Choose the loop to parallelize.
         */
        
        for (auto loopIter : LI){
          auto loop = &*loopIter;
          auto loopPDG = graph->createLoopsSubgraph(LI);
          auto instPair = divideLoopInstructions(loop);
          return new LoopDependenceInfo(entryFunction, LI, DT, SE, loop, loopPDG, instPair.first, instPair.second);
        }

        return nullptr;
      }

      std::pair<std::vector<Instruction *>, std::vector<Instruction *>>
      divideLoopInstructions(Loop *loop) {
        std::vector<Instruction *> bodyInst, otherInst;

        /*
         * ASSUMPTION: Canonical induction variable
         */
        auto phiIV = loop->getCanonicalInductionVariable();
        assert(phiIV != nullptr);
        //phiIV->print(errs() << "IV:\t"); errs() << "\n";

        bool nonBodyBB = false;
        for (auto bbi = loop->block_begin(); bbi != loop->block_end(); ++bbi) {
          BasicBlock *bb = *bbi;
          nonBodyBB = loop->isLoopLatch(bb);

          /*
           * Categorize all instructions in latch or exit basic blocks as 'other' instructions
           * Categorize branch, conditional, and induction variable instructions as 'other' instructions
           */
          for (auto ii = bb->begin(); ii != bb->end(); ++ii) {
            Instruction *i = &*ii;
            if (nonBodyBB || TerminatorInst::classof(i) || CmpInst::classof(i) || phiIV == i) {
              otherInst.push_back(i);
            } else {
              bodyInst.push_back(i);
            }
          }
        }

        /*
         * ASSUMPTION: One exiting block only; excluding exit block instructions
         */
        for (auto &I : *(loop->getUniqueExitBlock())) {
          otherInst.push_back(&I);
        }
        return make_pair(bodyInst, otherInst);
      }

      bool applyDSWP (LoopDependenceInfo *LDI){
        auto M = LDI->func->getParent();
        auto loop = LDI->loop;
        auto sccSubgraph = LDI->sccBodyDG;
        
        /*
         * Loop and SCC debug printouts
         */
        printLoop(loop);
        //printSCCs(sccSubgraph);

        /*
         * ASSUMPTION 3: Loop trip count is known.
         * ASSUMPTION 4: Loop trip count is 10000.
         */
        auto tripCount = LDI->SE.getSmallConstantTripCount(loop);
        //errs() << "Trip count:\t" << tripCount << "\n";
        if (tripCount == 0) return false;

        /*
         * ASSUMPTION 5: There are only 2 SCC within the loop's body
         */
        // errs() << "Num nodes: " << sccSubgraph->numInternalNodes() << "\n";
        if (sccSubgraph->numInternalNodes() != 2) return false;

        /*
         * ASSUMPTION 6: You only have one variable across the two SCCs
         */
        // errs() << "Num edges: " << std::distance(sccSubgraph->begin_edges(), sccSubgraph->end_edges()) << "\n";
        if (std::distance(sccSubgraph->begin_edges(), sccSubgraph->end_edges()) != 1) return false;
        DGEdge<SCC> *edge = *(sccSubgraph->begin_edges());

        /*
         * ASSUMPTION 7: There aren't memory data dependences
         */
        // errs() << "Mem dep: " << edge->isMemoryDependence() << "\n";
        if (edge->isMemoryDependence()) return false;

        /*
         * Build functions from each SCC
         */
        auto sccPair = edge->getNodePair();
        auto outSCC = sccPair.first->getNode();
        auto inSCC = sccPair.second->getNode();

        /* 
         * ASSUMPTION 8: You have no dependencies from outside instructions
         */
        //TODO on edge
        //errs() << "No dependencies from outside, and no memory dependencies\n";

        /*
         * ASSUMPTION 9: Buffer variable is of type integer 32
         * TODO: Identify scc edge variable, add AttributeList to function creation for the variable
         */

        auto stage0Pipeline = createPipelineStageFromSCC(LDI, outSCC, false);
        auto stage1Pipeline = createPipelineStageFromSCC(LDI, inSCC, true);
        
        std::vector<Value *> stages = { (Value*)stage0Pipeline, (Value*)stage1Pipeline };
        auto pipelineBB = createParallelizedFunctionExecution(LDI, stages);

        /*
         * Link pipeline basic block to function, unlink loop
         */

        auto preheader = loop->getLoopPreheader();
        preheader->getTerminator()->eraseFromParent();
        IRBuilder<> preheaderBuilder(preheader);

        auto int32 = IntegerType::get(M->getContext(), 32);
        auto globalBool = new GlobalVariable(*M, int32, /*isConstant=*/ false, GlobalValue::ExternalLinkage, Constant::getNullValue(int32));
        auto const0 = ConstantInt::get(int32, APInt(32, 0, false));
        auto loadBool = preheaderBuilder.CreateLoad(globalBool);
        auto compareInstruction = preheaderBuilder.CreateICmpEQ(loadBool, const0);
        auto conditionalBranch = preheaderBuilder.CreateCondBr(compareInstruction, pipelineBB, loop->getHeader());
        
        //globalBool->print(errs() << "gbool:\t"); errs() << "\n";
        //const0->print(errs() << "const0:\t"); errs() << "\n";
        //compareInstruction->print(errs() << "cmp:\t"); errs() << "\n";

        formLCSSA(*loop, LDI->DT, &LDI->LI, &LDI->SE);
        LDI->func->print(errs() << "Final function:\n"); errs() << "\n";
        return true;
      }

      Function *createPipelineStageFromSCC(LoopDependenceInfo *LDI, SCC *scc, bool incoming) {
        auto M = LDI->func->getParent();
        auto loop = LDI->loop;
        auto int32 = IntegerType::get(M->getContext(), 32);
        //auto vptr = PointerType::getUnqual(IntegerType::get(M->getContext(), 8));
        auto funcConst = incoming ? M->getOrInsertFunction("sccStage1", stageType) : M->getOrInsertFunction("sccStage0", stageType);
        Function * pipelineStage = static_cast<Function *>(funcConst);

        /*
         * TODO: Remove naming of basic blocks to avoid collisions
         */
        BasicBlock* entryBB = BasicBlock::Create(M->getContext(), "entry", pipelineStage);
        BasicBlock* exitBB = BasicBlock::Create(M->getContext(), "exit", pipelineStage);

        CallInst *queueCall;
        Instruction *popStorage;
        LoadInst *loadStorage;

        /*
         * ASSUMPTION: Only one instruction references the incoming SCC edge's variable from the previous stage
         * ASSUMPTION: Only one instruction computes the outgoing SCC edge's variable used in the next stage 
         */
        Instruction *popMatchingInstruction;
        Instruction *pushMatchingInstruction;

        /*
         * Clone loop instructions in given SCC or non-loop-body 
         */
        unordered_map<Instruction *, Instruction *> cloneMap;
        for (auto sccI = scc->begin_internal_node_map(); sccI != scc->end_internal_node_map(); ++sccI) {
          auto I = sccI->first;
          auto newI = I->clone();
          cloneMap[I] = newI;
        }

        /*
         * ASSUMPTION: All instructions outside of SCCs are related to the loop's induction variable that controls the loop
         */
        for (auto &I : LDI->otherInstOfLoop) {
          auto newI = I->clone();
          cloneMap[I] = newI;
        }

        /*
         * ASSUMPTION: Variable computed is stored in a PHI node
         * FIX: Look at outgoing edges of current SCC to identify variables to push to the queue
         */
        ReturnInst *retI;
        IRBuilder<> entryBuilder(entryBB);
        IRBuilder<> exitBuilder(exitBB);
        for (auto sccI = scc->begin_internal_node_map(); sccI != scc->end_internal_node_map(); ++sccI) {
          if (auto phiI = dyn_cast<PHINode>(sccI->first)) {
            retI = exitBuilder.CreateRet((Value*)cloneMap[phiI]);

            /*
             * Locate instruction computing outgoing variable and create queue push call
             */
            if (!incoming) {
              for (auto &op : phiI->incoming_values()) {
                if (auto opI = dyn_cast<Instruction>(op)) {
                  auto cloneOp = cloneMap[opI];
                  pushMatchingInstruction = cloneOp;
                  queueCall = entryBuilder.CreateCall(queuePushTemporary, ArrayRef<Value*>(cloneOp));
                }
              }
            }
            break;
          }
        }

        /*
         * Locate instruction using incoming variable, create queue pop call and load, and point instruction to the load
         */
        if (incoming) {
          popStorage = entryBuilder.CreateAlloca(int32);
          loadStorage = entryBuilder.CreateLoad(popStorage);
          queueCall = entryBuilder.CreateCall(queuePopTemporary, ArrayRef<Value*>(popStorage));

          auto nodePair = (*scc->begin_edges())->getNodePair();
          auto outgoingI = nodePair.first->getNode();
          auto incomingI = nodePair.second->getNode();
          popMatchingInstruction = cloneMap[incomingI];
          /*
           * ASSUMPTION: Incoming instruction was a PHI.
           * FIX: Use edge to identify dependent operand, and replace with the load instruction
           */
          for (auto useI = incomingI->op_begin(); useI != incomingI->op_end(); ++useI) {
            if (auto opI = dyn_cast<Instruction>(useI->get())) {
              if (opI == outgoingI) {
                popMatchingInstruction->setOperand(useI->getOperandNo(), loadStorage);
              }
            }
          }
        }

        /*
         * Clone loop basic blocks that are used by given SCC / non-loop-body basic blocks
         */
        unordered_map<BasicBlock*, BasicBlock*> bbCloneMap;

        /*
         * Map loop preheader to the entry block of the pipeline stage
         */
        bbCloneMap[loop->getLoopPreheader()] = entryBB;

        for (auto bbi = loop->block_begin(); bbi != loop->block_end(); ++bbi) {
          auto bb = *bbi;
          /*
           * FIX: Create basic blocks for induction variables and the SCC in question instead of all the loop's basic blocks
           */
          auto cloneBB = BasicBlock::Create(M->getContext(), bb->getName(), pipelineStage);
          IRBuilder<> builder(cloneBB);

          for (auto &I : *bb) {
            auto cloneIter = cloneMap.find(&I);
            if (cloneIter == cloneMap.end()) {
              continue;
            }
            cloneMap[&I] = builder.Insert(cloneIter->second);
          }

          bbCloneMap[bb] = cloneBB;
        }

        /*
         * Branch from entry of function to the header of the loop 
         */
        entryBuilder.CreateBr(bbCloneMap[loop->getHeader()]);

        /*
         * Insert queue push or queue pop + load
         */
        if (incoming) {
          queueCall->moveBefore(popMatchingInstruction);
          loadStorage->moveBefore(popMatchingInstruction);
        } else {
          /*
           * IMPROVEMENT: Find the successor of the producing instruction; add the queue push before this instruction
           */
          queueCall->moveBefore(pushMatchingInstruction->getParent()->getTerminator());
        }

        /*
         * Replace the rest of the clones' operands with the cloned instructions' versions of the operand
         * IMPROVEMENT: Ignore special cases upfront. If a clone of a general case is not found, abort with a corresponding error 
         */
        for (auto ii = cloneMap.begin(); ii != cloneMap.end(); ++ii) {
          auto cloneInstruction = ii->second;

          /*
           * Replacing operands/basic block pointers of PHINode with clones
           */
          if (auto phiI = dyn_cast<PHINode>(cloneInstruction)) {
            for (auto &op : phiI->operands()) {

              /*
               * Handle replacing operands
               */
              if (auto opI = dyn_cast<Instruction>(op)) {
                auto iCloneIter = cloneMap.find(opI);
                if (iCloneIter != cloneMap.end()) {
                  op.set(cloneMap[opI]);
                }
                continue;
              }
              // Check for constants etc... abort
            }

            for (auto &bb : phiI->blocks()) {

              /*
               * Handle replacing basic blocks
               */
              auto basicBlockIndex = phiI->getBasicBlockIndex(bb);
              phiI->setIncomingBlock(basicBlockIndex, bbCloneMap[bb]);
            }
            continue;
          }

          for (auto &op : cloneInstruction->operands()) {
            auto opV = op.get();
            if (auto opI = dyn_cast<Instruction>(opV)) {
              auto iCloneIter = cloneMap.find(opI);
              if (iCloneIter != cloneMap.end()) {
                op.set(cloneMap[opI]);
              }
              continue;
            }
            if (auto opB = dyn_cast<BasicBlock>(opV)) {
              auto bbCloneIter = bbCloneMap.find(opB);
              if (bbCloneIter != bbCloneMap.end()) {
                op.set(bbCloneIter->second);
              } else {
                // Operand pointed to original exiting block
                op.set(exitBB);
              }
              continue;
            }
            // Add cases such as constants where no clone needs to exist. Abort with an error if no such type is found
          }
        }

        //pipelineStage->print(errs() << "Function printout:\n"); errs() << "\n";
        return pipelineStage;
      }

      BasicBlock *createParallelizedFunctionExecution(LoopDependenceInfo *LDI, std::vector<Value *> &stages) {
        auto M = LDI->func->getParent();
        BasicBlock* pipelineBB = BasicBlock::Create(M->getContext(), "parallel", LDI->func);
        IRBuilder<> builder(pipelineBB);
        builder.CreateCall(stageHandler, ArrayRef<Value*>(stages));

        /*
         * ASSUMPTION: Only one unique exiting basic block from the loop
         */
        builder.CreateBr(LDI->loop->getExitBlock());
        return pipelineBB;
      }

      /*
      Function * createPipelineFromSCCDG(LoopDependenceInfo *LDI, std::vector<Function *> &stages) {
        //TODO: Return a function that carries out the pipeline
      }

      void linkParallelizedLoop(LoopDependenceInfo *LDI, Function *parallelizedLoop) {
        //TODO: Alter loop header to call parallelized loop and redirect terminator inst to exit bb
      }
      */

      void printLoop(Loop *loop) {
        errs() << "Applying DSWP on loop\n";
        auto header = loop->getHeader();
        errs() << "Number of bbs: " << std::distance(loop->block_begin(), loop->block_end()) << "\n";
        for (auto bbi = loop->block_begin(); bbi != loop->block_end(); ++bbi){
          auto bb = *bbi;
          if (header == bb) {
            errs() << "Header:\n";
          } else if (loop->isLoopLatch(bb)) {
            errs() << "Loop latch:\n";
          } else if (loop->isLoopExiting(bb)) {
            errs() << "Loop exiting:\n";
          } else {
            errs() << "Loop body:\n";
          }
          for (auto &I : *bb) {
            I.print(errs());
            errs() << "\n";
          }
        }
      }

      void printSCCs(SCCDG *sccSubgraph) {
        errs() << "\nInternal SCCs\n";
        for (auto sccI = sccSubgraph->begin_internal_node_map(); sccI != sccSubgraph->end_internal_node_map(); ++sccI) {
          sccI->first->print(errs());
        }
        errs() << "Number of SCCs: " << sccSubgraph->numInternalNodes() << "\n";
        for (auto edgeI = sccSubgraph->begin_edges(); edgeI != sccSubgraph->end_edges(); ++edgeI) {
          (*edgeI)->print(errs());
        }
        errs() << "Number of edges: " << std::distance(sccSubgraph->begin_edges(), sccSubgraph->end_edges()) << "\n";
      }
  };

}

// Next there is code to register your pass to "opt"
char llvm::DSWP::ID = 0;
static RegisterPass<DSWP> X("DSWP", "DSWP parallelization");

// Next there is code to register your pass to "clang"
static DSWP * _PassMaker = NULL;
static RegisterStandardPasses _RegPass1(PassManagerBuilder::EP_OptimizerLast,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
        if(!_PassMaker){ PM.add(_PassMaker = new DSWP());}}); // ** for -Ox
static RegisterStandardPasses _RegPass2(PassManagerBuilder::EP_EnabledOnOptLevel0,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
        if(!_PassMaker){ PM.add(_PassMaker = new DSWP());}});// ** for -O0
