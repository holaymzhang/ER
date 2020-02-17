//===-- ExecutionState.cpp ------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Memory.h"

#include "klee/ExecutionState.h"

#include "klee/Expr/Expr.h"
#include "klee/OptionCategories.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/OptionCategories.h"
#include "CoreStats.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdarg.h>

using namespace llvm;
using namespace klee;

namespace {
cl::opt<bool> DebugLogStateMerge(
    "debug-log-state-merge", cl::init(false),
    cl::desc("Debug information for underlying state merging (default=false)"),
    cl::cat(MergeCat));

/*** HASE options ***/
cl::opt<std::string> PathRecordingEntryPoint(
    "pathrec-entry-point", cl::init(""),
    cl::desc("Path will be recorded after this entry point is called (record all path by default)"),
    cl::cat(HASECat));
cl::opt<bool> IgnorePOSIXPath(
    "ignore-posix-path", cl::init(false),
    cl::desc("Ignore (not recording or using) path traces inside POSIX runtime (default=false"),
    cl::cat(HASECat));
}

/***/

StackFrame::StackFrame(KInstIterator _caller, KFunction *_kf)
  : caller(_caller), kf(_kf), callPathNode(0), 
    minDistToUncoveredOnReturn(0), varargs(0) {
  locals = new Cell[kf->numRegisters];
}

StackFrame::StackFrame(const StackFrame &s) 
  : caller(s.caller),
    kf(s.kf),
    callPathNode(s.callPathNode),
    allocas(s.allocas),
    minDistToUncoveredOnReturn(s.minDistToUncoveredOnReturn),
    varargs(s.varargs) {
  locals = new Cell[s.kf->numRegisters];
  for (unsigned i=0; i<s.kf->numRegisters; i++)
    locals[i] = s.locals[i];
}

StackFrame::~StackFrame() { 
  delete[] locals; 
}

/***/

ExecutionState::ExecutionState(KFunction *kf) :
    pc(kf->instructions),
    prevPC(pc),
    isInUserMain(false),
    isInPOSIX(false),
    POSIXDepth(0),
    depth(0),

    instsSinceCovNew(0),
    coveredNew(false),
    forkDisabled(false),
    replayPosition(0),
    replayDataRecEntriesPosition(0),
    nbranches_rec(0),
    ptreeNode(0),
    steppedInstructions(0){
  pushFrame(0, kf);
  if (PathRecordingEntryPoint.empty()) {
    isInUserMain = true;
  }
}

ExecutionState::ExecutionState(const std::vector<ref<Expr> > &assumptions)
    : constraints(assumptions), replayPosition(0), replayDataRecEntriesPosition(0), nbranches_rec(0), ptreeNode(0) {}

ExecutionState::~ExecutionState() {
  for (unsigned int i=0; i<symbolics.size(); i++)
  {
    const MemoryObject *mo = symbolics[i].first;
    assert(mo->refCount > 0);
    mo->refCount--;
    if (mo->refCount == 0)
      delete mo;
  }

  for (auto cur_mergehandler: openMergeStack){
    cur_mergehandler->removeOpenState(this);
  }


  while (!stack.empty()) popFrame();
}

ExecutionState::ExecutionState(const ExecutionState& state):
    pc(state.pc),
    prevPC(state.prevPC),
    stack(state.stack),
    incomingBBIndex(state.incomingBBIndex),

    addressSpace(state.addressSpace),
    constraints(state.constraints),

    queryCost(state.queryCost),
    fork_queryCost(state.fork_queryCost),
    prev_fork_queryCost(state.prev_fork_queryCost),
    prev_fork_queryCost_single(state.prev_fork_queryCost_single),
    isInUserMain(state.isInUserMain),
    isInPOSIX(state.isInPOSIX),
    POSIXDepth(state.POSIXDepth),
    depth(state.depth),

    pathOS(state.pathOS),
    symPathOS(state.symPathOS),

    instsSinceCovNew(state.instsSinceCovNew),
    coveredNew(state.coveredNew),
    forkDisabled(state.forkDisabled),

    replayPosition(state.replayPosition),
    replayDataRecEntriesPosition(state.replayDataRecEntriesPosition),
    nbranches_rec(state.nbranches_rec),

    coveredLines(state.coveredLines),
    ptreeNode(state.ptreeNode),
    symbolics(state.symbolics),
    arrayNames(state.arrayNames),
    openMergeStack(state.openMergeStack),
    steppedInstructions(state.steppedInstructions)
{
  for (unsigned int i=0; i<symbolics.size(); i++)
    symbolics[i].first->refCount++;

  for (auto cur_mergehandler: openMergeStack)
    cur_mergehandler->addOpenState(this);
}

ExecutionState *ExecutionState::branch() {
  depth++;

  ExecutionState *falseState = new ExecutionState(*this);
  falseState->coveredNew = false;
  falseState->coveredLines.clear();

  // initialize PathOS based on existence of existing PathOS field
  if (pathOS.isValid()) {
    // Need to update the pathOS.id field of falseState, otherwise the same id
    // is used for both falseState and trueState.
    falseState->pathOS = pathOS.branch();
  }
  if (stackPathOS.isValid()) {
    falseState->stackPathOS = stackPathOS.branch();
  }
  if (consPathOS.isValid()) {
    falseState->consPathOS = consPathOS.branch();
  }
  if (statsPathOS.isValid()) {
    falseState->statsPathOS = statsPathOS.branch();
  }

  return falseState;
}

void ExecutionState::pushFrame(KInstIterator caller, KFunction *kf) {
  stack.push_back(StackFrame(caller,kf));
  ++kf->frequency;
  if (!isInUserMain && (kf->function->getName() == PathRecordingEntryPoint)) {
    isInUserMain = true;
  }
  // NOTE: when enabling POSIX runtime, the entire application will be wrapped
  // into a POSIX function call (i.e. the entry point func belongs to POSIX)
  // So we should only reason about prop "InPOSIX" inside UserMain
  if (isInUserMain && IgnorePOSIXPath && kf->function->hasFnAttribute("InPOSIX")) {
    if(POSIXDepth == 0) {
      isInPOSIX=true;
    }
    ++POSIXDepth;
  }
}

void ExecutionState::popFrame() {
  StackFrame &sf = stack.back();
  for (std::vector<const MemoryObject*>::iterator it = sf.allocas.begin(), 
         ie = sf.allocas.end(); it != ie; ++it)
    addressSpace.unbindObject(*it);
  if (isInUserMain && (sf.kf->function->getName() == PathRecordingEntryPoint)) {
    isInUserMain = false;
  }
  if (isInUserMain && IgnorePOSIXPath && sf.kf->function->hasFnAttribute("InPOSIX")) {
    --POSIXDepth;
    if (POSIXDepth == 0) {
      isInPOSIX = false;
    }
  }
  stack.pop_back();
}

void ExecutionState::addSymbolic(const MemoryObject *mo, const Array *array) { 
  mo->refCount++;
  symbolics.push_back(std::make_pair(mo, array));
}

/**/

llvm::raw_ostream &klee::operator<<(llvm::raw_ostream &os, const MemoryMap &mm) {
  os << "{";
  MemoryMap::iterator it = mm.begin();
  MemoryMap::iterator ie = mm.end();
  if (it!=ie) {
    os << "MO" << it->first->id << ":" << it->second;
    for (++it; it!=ie; ++it)
      os << ", MO" << it->first->id << ":" << it->second;
  }
  os << "}";
  return os;
}

bool ExecutionState::merge(const ExecutionState &b) {
  if (DebugLogStateMerge)
    llvm::errs() << "-- attempting merge of A:" << this << " with B:" << &b
                 << "--\n";
  if (pc != b.pc)
    return false;

  // XXX is it even possible for these to differ? does it matter? probably
  // implies difference in object states?
  if (symbolics!=b.symbolics)
    return false;

  {
    std::vector<StackFrame>::const_iterator itA = stack.begin();
    std::vector<StackFrame>::const_iterator itB = b.stack.begin();
    while (itA!=stack.end() && itB!=b.stack.end()) {
      // XXX vaargs?
      if (itA->caller!=itB->caller || itA->kf!=itB->kf)
        return false;
      ++itA;
      ++itB;
    }
    if (itA!=stack.end() || itB!=b.stack.end())
      return false;
  }

  std::set< ref<Expr> > aConstraints(constraints.begin(), constraints.end());
  std::set< ref<Expr> > bConstraints(b.constraints.begin(), 
                                     b.constraints.end());
  std::set< ref<Expr> > commonConstraints, aSuffix, bSuffix;
  std::set_intersection(aConstraints.begin(), aConstraints.end(),
                        bConstraints.begin(), bConstraints.end(),
                        std::inserter(commonConstraints, commonConstraints.begin()));
  std::set_difference(aConstraints.begin(), aConstraints.end(),
                      commonConstraints.begin(), commonConstraints.end(),
                      std::inserter(aSuffix, aSuffix.end()));
  std::set_difference(bConstraints.begin(), bConstraints.end(),
                      commonConstraints.begin(), commonConstraints.end(),
                      std::inserter(bSuffix, bSuffix.end()));
  if (DebugLogStateMerge) {
    llvm::errs() << "\tconstraint prefix: [";
    for (std::set<ref<Expr> >::iterator it = commonConstraints.begin(),
                                        ie = commonConstraints.end();
         it != ie; ++it)
      llvm::errs() << *it << ", ";
    llvm::errs() << "]\n";
    llvm::errs() << "\tA suffix: [";
    for (std::set<ref<Expr> >::iterator it = aSuffix.begin(),
                                        ie = aSuffix.end();
         it != ie; ++it)
      llvm::errs() << *it << ", ";
    llvm::errs() << "]\n";
    llvm::errs() << "\tB suffix: [";
    for (std::set<ref<Expr> >::iterator it = bSuffix.begin(),
                                        ie = bSuffix.end();
         it != ie; ++it)
      llvm::errs() << *it << ", ";
    llvm::errs() << "]\n";
  }

  // We cannot merge if addresses would resolve differently in the
  // states. This means:
  // 
  // 1. Any objects created since the branch in either object must
  // have been free'd.
  //
  // 2. We cannot have free'd any pre-existing object in one state
  // and not the other

  if (DebugLogStateMerge) {
    llvm::errs() << "\tchecking object states\n";
    llvm::errs() << "A: " << addressSpace.objects << "\n";
    llvm::errs() << "B: " << b.addressSpace.objects << "\n";
  }
    
  std::set<const MemoryObject*> mutated;
  MemoryMap::iterator ai = addressSpace.objects.begin();
  MemoryMap::iterator bi = b.addressSpace.objects.begin();
  MemoryMap::iterator ae = addressSpace.objects.end();
  MemoryMap::iterator be = b.addressSpace.objects.end();
  for (; ai!=ae && bi!=be; ++ai, ++bi) {
    if (ai->first != bi->first) {
      if (DebugLogStateMerge) {
        if (ai->first < bi->first) {
          llvm::errs() << "\t\tB misses binding for: " << ai->first->id << "\n";
        } else {
          llvm::errs() << "\t\tA misses binding for: " << bi->first->id << "\n";
        }
      }
      return false;
    }
    if (ai->second != bi->second) {
      if (DebugLogStateMerge)
        llvm::errs() << "\t\tmutated: " << ai->first->id << "\n";
      mutated.insert(ai->first);
    }
  }
  if (ai!=ae || bi!=be) {
    if (DebugLogStateMerge)
      llvm::errs() << "\t\tmappings differ\n";
    return false;
  }
  
  // merge stack

  ref<Expr> inA = ConstantExpr::alloc(1, Expr::Bool);
  ref<Expr> inB = ConstantExpr::alloc(1, Expr::Bool);
  for (std::set< ref<Expr> >::iterator it = aSuffix.begin(), 
         ie = aSuffix.end(); it != ie; ++it)
    inA = AndExpr::create(inA, *it);
  for (std::set< ref<Expr> >::iterator it = bSuffix.begin(), 
         ie = bSuffix.end(); it != ie; ++it)
    inB = AndExpr::create(inB, *it);

  // XXX should we have a preference as to which predicate to use?
  // it seems like it can make a difference, even though logically
  // they must contradict each other and so inA => !inB

  std::vector<StackFrame>::iterator itA = stack.begin();
  std::vector<StackFrame>::const_iterator itB = b.stack.begin();
  for (; itA!=stack.end(); ++itA, ++itB) {
    StackFrame &af = *itA;
    const StackFrame &bf = *itB;
    for (unsigned i=0; i<af.kf->numRegisters; i++) {
      ref<Expr> &av = af.locals[i].value;
      const ref<Expr> &bv = bf.locals[i].value;
      if (av.isNull() || bv.isNull()) {
        // if one is null then by implication (we are at same pc)
        // we cannot reuse this local, so just ignore
      } else {
        av = SelectExpr::create(inA, av, bv);
      }
    }
  }

  for (std::set<const MemoryObject*>::iterator it = mutated.begin(), 
         ie = mutated.end(); it != ie; ++it) {
    const MemoryObject *mo = *it;
    const ObjectState *os = addressSpace.findObject(mo);
    const ObjectState *otherOS = b.addressSpace.findObject(mo);
    assert(os && !os->readOnly && 
           "objects mutated but not writable in merging state");
    assert(otherOS);

    ObjectState *wos = addressSpace.getWriteable(mo, os);
    for (unsigned i=0; i<mo->size; i++) {
      ref<Expr> av = wos->read8(i);
      ref<Expr> bv = otherOS->read8(i);
      uint64_t flags = wos->getFlags(i);
      flags |= Expr::FLAG_OPTIMIZATION;
      KInstruction *kinst = wos->getKInst(i);
      wos->write(i, SelectExpr::create(inA, av, bv), flags, kinst);
    }
  }

  constraints = ConstraintManager();
  for (std::set< ref<Expr> >::iterator it = commonConstraints.begin(), 
         ie = commonConstraints.end(); it != ie; ++it)
    constraints.addConstraint(*it);
  constraints.addConstraint(OrExpr::create(inA, inB));

  return true;
}

void ExecutionState::dumpStackPathOS() {
  struct StringInstStats stack;
  llvm::raw_string_ostream sos(stack.str);
  dumpStack(sos);
  sos.flush();
  stack.instcnt = stats::instructions;
  stackPathOS << stack;
}

void ExecutionState::dumpStatsPathOS() {
  struct ExecutionStats exstats;
  time::Span current_cost = fork_queryCost - prev_fork_queryCost;
  time::Span current_cost_increment = current_cost - prev_fork_queryCost_single;
  prev_fork_queryCost = fork_queryCost;
  const InstructionInfo *iinfo = this->prevPC->info;
  if (current_cost.toMicroseconds() > 0) {
    prev_fork_queryCost_single = current_cost;
    exstats.instructions_cnt = stats::instructions;
    llvm::raw_string_ostream sos(exstats.llvm_inst_str);
    this->prevPC->inst->print(sos);
    exstats.file_loc = iinfo->file + ":" + std::to_string(iinfo->line);
    exstats.queryCost_us = current_cost.toMicroseconds();
    exstats.queryCost_increment_us = current_cost_increment.toMicroseconds();
    
    statsPathOS << exstats;
  }
}
void ExecutionState::dumpConsPathOS(const std::string &cons) {
  struct StringInstStats constats;
  constats.instcnt = stats::instructions;
  constats.str = cons;

  consPathOS << constats;
}

void ExecutionState::dumpConstraints(llvm::raw_ostream &out) const {
  for (ConstraintManager::const_iterator i = constraints.begin();
      i != constraints.end(); i++) {
    out << '*';
    (*i)->print(out);
    out << '\n';
  }
}
void ExecutionState::dumpConstraints() const {
  dumpConstraints(llvm::errs());
}

void ExecutionState::dumpStack(llvm::raw_ostream &out) const {
  unsigned idx = 0;
  const KInstruction *target = prevPC;
  for (ExecutionState::stack_ty::const_reverse_iterator
         it = stack.rbegin(), ie = stack.rend();
       it != ie; ++it) {
    const StackFrame &sf = *it;
    Function *f = sf.kf->function;
    const InstructionInfo &ii = *target->info;
    out << "\t#" << idx++;
    std::stringstream AssStream;
    AssStream << std::setw(8) << std::setfill('0') << ii.assemblyLine;
    out << AssStream.str();
    out << " in " << f->getName().str() << " (";
    // Yawn, we could go up and print varargs if we wanted to.
    unsigned index = 0;
    for (Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
         ai != ae; ++ai) {
      if (ai!=f->arg_begin()) out << ", ";

      out << ai->getName().str();
      // XXX should go through function
      ref<Expr> value = sf.locals[sf.kf->getArgRegister(index++)].value;
      if (value.get() && isa<ConstantExpr>(value))
        out << "=" << value;
    }
    out << ")";
    if (ii.file != "")
      out << " at " << ii.file << ":" << ii.line;
    out << "\n";
    target = sf.caller;
  }
}
void ExecutionState::dumpStack() const {
  dumpStack(llvm::errs());
}
