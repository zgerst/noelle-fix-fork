/*
 * Copyright 2016 - 2019  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "PDG.hpp"
#include "SCCDAG.hpp"
#include "Architecture.hpp"
#include "LoopDependenceInfo.hpp"

using namespace llvm;

LoopDependenceInfo::LoopDependenceInfo(
  PDG *fG,
  Loop *l,
  DominatorSummary &DS,
  ScalarEvolution &SE,
  uint32_t maxCores
) : LoopDependenceInfo{fG, l, DS, SE, maxCores, {}} {}

LoopDependenceInfo::LoopDependenceInfo(
  PDG *fG,
  Loop *l,
  DominatorSummary &DS,
  ScalarEvolution &SE,
  uint32_t maxCores,
  std::unordered_set<LoopDependenceInfoOptimization> optimizations
) : DOALLChunkSize{8},
    maximumNumberOfCoresForTheParallelization{maxCores},
    liSummary{l},
    enabledOptimizations{optimizations}
  {

  /*
   * Enable all transformations.
   */
  this->enableAllTransformations();

  /*
   * Fetch the PDG of the loop and its SCCDAG.
   */
  this->fetchLoopAndBBInfo(l, SE);
  auto ls = getLoopStructure();
  auto loopExitBlocks = ls->getLoopExitBasicBlocks();
  auto DGs = this->createDGsForLoop(l, fG, DS);
  this->loopDG = DGs.first;
  auto loopSCCDAG = DGs.second;

  /*
   * Create the environment for the loop.
   */
  this->environment = new LoopEnvironment(loopDG, loopExitBlocks);

  /*
   * Create the invariant manager.
   */
  auto topLoop = this->liSummary.getLoopNestingTreeRoot();
  this->invariantManager = new InvariantManager(topLoop, this->loopDG);

  /*
   * Merge SCCs where separation is unnecessary
   * Calculate various attributes on remaining SCCs
   */
  LoopCarriedDependencies lcd(this->liSummary, DS, *loopSCCDAG);
  SCCDAGNormalizer normalizer{*loopSCCDAG, this->liSummary, lcd};
  normalizer.normalizeInPlace();
  lcd = LoopCarriedDependencies(this->liSummary, DS, *loopSCCDAG);
  this->inductionVariables = new InductionVariableManager(liSummary, *invariantManager, SE, *loopSCCDAG, *environment);
  this->sccdagAttrs = SCCDAGAttrs(loopDG, loopSCCDAG, this->liSummary, SE, lcd, *inductionVariables, DS);

  this->domainSpaceAnalysis = new LoopIterationDomainSpaceAnalysis(liSummary, *this->inductionVariables, SE);

  /*
   * Collect induction variable information
   */
  auto iv = this->inductionVariables->getLoopGoverningInductionVariable(*liSummary.getLoop(*l->getHeader()));
  loopGoverningIVAttribution = iv == nullptr ? nullptr
    : new LoopGoverningIVAttribution(*iv, *loopSCCDAG->sccOfValue(iv->getLoopEntryPHI()), loopExitBlocks);

  /*
   * Cache the post-dominator tree.
   */
  for (auto bb : l->blocks()) {
    loopBBtoPD[&*bb] = DS.PDT.getNode(&*bb)->getIDom()->getBlock();
  }

  return ;
}

void LoopDependenceInfo::copyParallelizationOptionsFrom (LoopDependenceInfo *otherLDI) {
  this->DOALLChunkSize = otherLDI->DOALLChunkSize;
  this->maximumNumberOfCoresForTheParallelization = otherLDI->maximumNumberOfCoresForTheParallelization;
  this->enabledTransformations = otherLDI->enabledTransformations;

  return ;
}

/*
 * Fetch the number of exit blocks.
 */
uint32_t LoopDependenceInfo::numberOfExits (void) const{
  return this->getLoopStructure()->getLoopExitBasicBlocks().size();
}

void LoopDependenceInfo::fetchLoopAndBBInfo (
  Loop *l,
  ScalarEvolution &SE
  ){

  /*
   * Compute the trip counts of all loops in the loop tree that starts with @l.
   */
  auto loopTripCount = this->computeTripCounts(l, SE);
  if (loopTripCount > 0){
    this->compileTimeKnownTripCount = true;
    this->tripCount = loopTripCount;

  } else {
    this->compileTimeKnownTripCount = false;
  }

  return ;
}

uint64_t LoopDependenceInfo::computeTripCounts (
  Loop *l,
  ScalarEvolution &SE
  ){

  /*
   * Fetch the trip count of the loop given as input.
   */
  auto tripCount = SE.getSmallConstantTripCount(l);

  return tripCount;
}

std::pair<PDG *, SCCDAG *> LoopDependenceInfo::createDGsForLoop (Loop *l, PDG *functionDG, DominatorSummary &DS){

  /*
   * Set the loop dependence graph.
   */
  auto loopDG = functionDG->createLoopsSubgraph(l);

  /*
   * Build a SCCDAG of loop-internal instructions
   */
  std::vector<Value *> loopInternals;
  for (auto internalNode : loopDG->internalNodePairs()) {
      loopInternals.push_back(internalNode.first);
  }
  auto loopInternalDG = loopDG->createSubgraphFromValues(loopInternals, false);
  if (enabledOptimizations.find(LoopDependenceInfoOptimization::MEMORY_CLONING_ID) != enabledOptimizations.end()) {
    removeUnnecessaryDependenciesThatCloningMemoryNegates(loopInternalDG, DS);
  }
  auto loopSCCDAG = new SCCDAG(loopInternalDG);

  /*
   * Safety check: check that the SCCDAG includes all instructions of the loop given as input.
   */
  #ifdef DEBUG

  /*
   * Check that all loop instructions belong to LDI-specific containers.
   */
  {
  int64_t numberOfInstructionsInLoop = 0;
  for (auto bbIter : l->blocks()){
    for (auto &I : *bbIter){
      assert(std::find(loopInternals.begin(), loopInternals.end(), &I) != loopInternals.end());
      assert(loopInternalDG->isInternal(&I));
      assert(loopSCCDAG->doesItContain(&I));
      numberOfInstructionsInLoop++;
    }
  }

  /*
   * Check that all LDI-specific containers include only loop instructions.
   */
  assert(loopInternals.size() == numberOfInstructionsInLoop);
  assert(loopInternalDG->numNodes() == loopInternals.size());
  }
  #endif

  return std::make_pair(loopDG, loopSCCDAG);
}

void LoopDependenceInfo::removeUnnecessaryDependenciesThatCloningMemoryNegates (
  PDG *loopInternalDG,
  DominatorSummary &DS
) {
  auto rootLoop = liSummary.getLoopNestingTreeRoot();
  LoopCarriedDependencies lcd(liSummary, DS, *loopInternalDG);
  this->memoryCloningAnalysis = new MemoryCloningAnalysis(rootLoop, DS);

  std::unordered_set<DGEdge<Value> *> edgesToRemove;
  for (auto edge : lcd.getLoopCarriedDependenciesForLoop(*rootLoop)) {
    if (!edge->isMemoryDependence()) continue;

    auto producer = dyn_cast<Instruction>(edge->getOutgoingT());
    auto consumer = dyn_cast<Instruction>(edge->getIncomingT());
    if (!producer || !consumer) continue;

    auto locationProducer = this->memoryCloningAnalysis->getClonableMemoryLocationFor(producer);
    auto locationConsumer = this->memoryCloningAnalysis->getClonableMemoryLocationFor(consumer);
    if (!locationProducer || !locationConsumer) continue;

    // producer->print(errs() << "Found alloca location for producer: "); errs() << "\n";
    // consumer->print(errs() << "Found alloca location for consumer: "); errs() << "\n";
    // locationProducer->getAllocation()->print(errs() << "Alloca: "); errs() << "\n";
    // locationConsumer->getAllocation()->print(errs() << "Alloca: "); errs() << "\n";

    edgesToRemove.insert(edge);
  }

  for (auto edge : edgesToRemove) {
    loopInternalDG->removeEdge(edge);
  }
}
 
bool LoopDependenceInfo::isTransformationEnabled (Transformation transformation){
  auto exist = this->enabledTransformations.find(transformation) != this->enabledTransformations.end();

  return exist;
}

void LoopDependenceInfo::enableAllTransformations (void){
  for (int32_t i = Transformation::First; i <= Transformation::Last; i++){
    auto t = static_cast<Transformation>(i);
    this->enabledTransformations.insert(t);
  }

  return ;
}

void LoopDependenceInfo::disableTransformation (Transformation transformationToDisable){
  this->enabledTransformations.erase(transformationToDisable);

  return ;
}

bool LoopDependenceInfo::isOptimizationEnabled (LoopDependenceInfoOptimization optimization) {
  auto enabled = this->enabledOptimizations.find(optimization) != this->enabledOptimizations.end();
  return enabled;
}

PDG * LoopDependenceInfo::getLoopDG (void) const {
  return this->loopDG;
}

bool LoopDependenceInfo::iterateOverSubLoopsRecursively (
  std::function<bool (const LoopStructure &child)> funcToInvoke
  ){

  /*
   * Iterate over the children.
   */
  for (auto subloop : this->liSummary.loops){
    if (funcToInvoke(*subloop)){
      return true ;
    }
  }

  return false;
}

uint64_t LoopDependenceInfo::getID (void) const {

  /*
   * Fetch the loop structure.
   */
  auto ls = this->getLoopStructure();

  /*
   * Fetch the ID.
   */
  auto ID = ls->getID();

  return ID;
}

LoopStructure * LoopDependenceInfo::getLoopStructure (void) const {
  return this->liSummary.getLoopNestingTreeRoot();
}

LoopStructure * LoopDependenceInfo::getNestedMostLoopStructure (Instruction *I) const {
  return this->liSummary.getLoop(*I);
}

bool LoopDependenceInfo::isSCCContainedInSubloop (SCC *scc) const {
  return this->sccdagAttrs.isSCCContainedInSubloop(this->liSummary, scc);
}

InductionVariableManager * LoopDependenceInfo::getInductionVariableManager (void) const {
  return inductionVariables;
}

LoopGoverningIVAttribution * LoopDependenceInfo::getLoopGoverningIVAttribution (void) const {
  return loopGoverningIVAttribution;
}

MemoryCloningAnalysis * LoopDependenceInfo::getMemoryCloningAnalysis (void) const {
  assert(this->memoryCloningAnalysis != nullptr
    && "Requesting memory cloning analysis without having specified LoopDependenceInfoOptimization::MEMORY_CLONING");
  return this->memoryCloningAnalysis;
}

bool LoopDependenceInfo::doesHaveCompileTimeKnownTripCount (void) const {
  return this->compileTimeKnownTripCount;
}

uint64_t LoopDependenceInfo::getCompileTimeTripCount (void) const {
  return this->tripCount;
}

uint32_t LoopDependenceInfo::getMaximumNumberOfCores (void) const {
  return this->maximumNumberOfCoresForTheParallelization;
}

InvariantManager * LoopDependenceInfo::getInvariantManager (void) const {
  return this->invariantManager;
}

LoopIterationDomainSpaceAnalysis * LoopDependenceInfo::getLoopIterationDomainSpaceAnalysis (void) const {
  return this->domainSpaceAnalysis;
}

const LoopsSummary & LoopDependenceInfo::getLoopHierarchyStructures (void) const {
  return this->liSummary;
}

SCCDAGAttrs * LoopDependenceInfo::getSCCManager (void) {
  return & (this->sccdagAttrs);
}

LoopDependenceInfo::~LoopDependenceInfo() {
  delete this->loopDG;
  delete this->environment;

  if (this->inductionVariables){
    delete this->inductionVariables;
  }
  if (this->loopGoverningIVAttribution){
    delete this->loopGoverningIVAttribution;
  }

  assert(this->invariantManager);
  delete this->invariantManager;

  delete this->domainSpaceAnalysis;

  return ;
}
