//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Executor.h"

#include "../Expr/ArrayExprOptimizer.h"
#include "Context.h"
#include "CoreStats.h"
#include "ExternalDispatcher.h"
#include "ImpliedValue.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "PTree.h"
#include "Searcher.h"
#include "SeedInfo.h"
#include "SpecialFunctionHandler.h"
#include "StatsTracker.h"
#include "TimingSolver.h"
#include "UserSearcher.h"
#include "ExecutorDebugHelper.h"
#include "ExecutorCmdLine.h"

#include "klee/Common.h"
#include "klee/Config/Version.h"
#include "klee/ExecutionState.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprPPrinter.h"
#include "klee/Expr/ExprSMTLIBPrinter.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include "klee/Internal/Support/FileHandling.h"
#include "klee/Internal/Support/FloatEvaluation.h"
#include "klee/Internal/Support/ModuleUtil.h"
#include "klee/Internal/System/MemoryUsage.h"
#include "klee/Internal/System/Time.h"
#include "klee/Interpreter.h"
#include "klee/OptionCategories.h"
#include "klee/Solver/SolverCmdLine.h"
#include "klee/Solver/SolverStats.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/util/GetElementPtrTypeIterator.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cxxabi.h>
#include <fstream>
#include <iomanip>
#include <iosfwd>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <vector>

using namespace llvm;
using namespace klee;

namespace klee {
cl::OptionCategory DebugCat("Debugging options",
                            "These are debugging options.");

cl::OptionCategory ExtCallsCat("External call policy options",
                               "These options impact external calls.");

cl::OptionCategory SeedingCat(
    "Seeding options",
    "These options are related to the use of seeds to start exploration.");

cl::OptionCategory
    TerminationCat("State and overall termination options",
                   "These options control termination of the overall KLEE "
                   "execution and of individual states.");

cl::OptionCategory TestGenCat("Test generation options",
                              "These options impact test generation.");

cl::opt<std::string> MaxTime(
    "max-time",
    cl::desc("Halt execution after the specified duration.  "
             "Set to 0s to disable (default=0s)"),
    cl::init("0s"),
    cl::cat(TerminationCat));
} // namespace klee

namespace {

/*** Test generation options ***/

cl::opt<bool> DumpStatesOnHalt(
    "dump-states-on-halt",
    cl::init(true),
    cl::desc("Dump test cases for all active states on exit (default=true)"),
    cl::cat(TestGenCat));

cl::opt<bool> OnlyOutputStatesCoveringNew(
    "only-output-states-covering-new",
    cl::init(false),
    cl::desc("Only output test cases covering new code (default=false)"),
    cl::cat(TestGenCat));

cl::opt<bool> EmitAllErrors(
    "emit-all-errors", cl::init(false),
    cl::desc("Generate tests cases for all errors "
             "(default=false, i.e. one per (error,instruction) pair)"),
    cl::cat(TestGenCat));

/* Constraint solving options */

cl::opt<unsigned> MaxSymArraySize(
    "max-sym-array-size",
    cl::desc(
        "If a symbolic array exceeds this size (in bytes), symbolic addresses "
        "into this array are concretized.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(SolvingCat));

cl::opt<bool>
    SimplifySymIndices("simplify-sym-indices",
                       cl::init(false),
                       cl::desc("Simplify symbolic accesses using equalities "
                                "from other constraints (default=false)"),
                       cl::cat(SolvingCat));

cl::opt<bool>
    EqualitySubstitution("equality-substitution", cl::init(true),
                         cl::desc("Simplify equality expressions before "
                                  "querying the solver (default=true)"),
                         cl::cat(SolvingCat));


/*** External call policy options ***/

enum class ExternalCallPolicy {
  None,     // No external calls allowed
  Concrete, // Only external calls with concrete arguments allowed
  All,      // All external calls allowed
};

cl::opt<ExternalCallPolicy> ExternalCalls(
    "external-calls",
    cl::desc("Specify the external call policy"),
    cl::values(
        clEnumValN(
            ExternalCallPolicy::None, "none",
            "No external function calls are allowed.  Note that KLEE always "
            "allows some external calls with concrete arguments to go through "
            "(in particular printf and puts), regardless of this option."),
        clEnumValN(ExternalCallPolicy::Concrete, "concrete",
                   "Only external function calls with concrete arguments are "
                   "allowed (default)"),
        clEnumValN(ExternalCallPolicy::All, "all",
                   "All external function calls are allowed.  This concretizes "
                   "any symbolic arguments in calls to external functions.")
            KLEE_LLVM_CL_VAL_END),
    cl::init(ExternalCallPolicy::Concrete),
    cl::cat(ExtCallsCat));

cl::opt<bool> SuppressExternalWarnings(
    "suppress-external-warnings",
    cl::init(false),
    cl::desc("Supress warnings about calling external functions."),
    cl::cat(ExtCallsCat));

cl::opt<bool> AllExternalWarnings(
    "all-external-warnings",
    cl::init(false),
    cl::desc("Issue a warning everytime an external call is made, "
             "as opposed to once per function (default=false)"),
    cl::cat(ExtCallsCat));


/*** Seeding options ***/

cl::opt<bool> AlwaysOutputSeeds(
    "always-output-seeds",
    cl::init(true),
    cl::desc(
        "Dump test cases even if they are driven by seeds only (default=true)"),
    cl::cat(SeedingCat));

cl::opt<bool> OnlyReplaySeeds(
    "only-replay-seeds",
    cl::init(false),
    cl::desc("Discard states that do not have a seed (default=false)."),
    cl::cat(SeedingCat));

cl::opt<bool> OnlySeed("only-seed",
                       cl::init(false),
                       cl::desc("Stop execution after seeding is done without "
                                "doing regular search (default=false)."),
                       cl::cat(SeedingCat));

cl::opt<bool>
    AllowSeedExtension("allow-seed-extension",
                       cl::init(false),
                       cl::desc("Allow extra (unbound) values to become "
                                "symbolic during seeding (default=false)."),
                       cl::cat(SeedingCat));

cl::opt<bool> ZeroSeedExtension(
    "zero-seed-extension",
    cl::init(false),
    cl::desc(
        "Use zero-filled objects if matching seed not found (default=false)"),
    cl::cat(SeedingCat));

cl::opt<bool> AllowSeedTruncation(
    "allow-seed-truncation",
    cl::init(false),
    cl::desc("Allow smaller buffers than in seeds (default=false)."),
    cl::cat(SeedingCat));

cl::opt<bool> NamedSeedMatching(
    "named-seed-matching",
    cl::init(false),
    cl::desc("Use names to match symbolic objects to inputs (default=false)."),
    cl::cat(SeedingCat));

cl::opt<std::string>
    SeedTime("seed-time",
             cl::desc("Amount of time to dedicate to seeds, before normal "
                      "search (default=0s (off))"),
             cl::cat(SeedingCat));


/*** Termination criteria options ***/

cl::list<Executor::TerminateReason> ExitOnErrorType(
    "exit-on-error-type",
    cl::desc(
        "Stop execution after reaching a specified condition (default=false)"),
    cl::values(
        clEnumValN(Executor::Abort, "Abort", "The program crashed"),
        clEnumValN(Executor::Assert, "Assert", "An assertion was hit"),
        clEnumValN(Executor::BadVectorAccess, "BadVectorAccess",
                   "Vector accessed out of bounds"),
        clEnumValN(Executor::Exec, "Exec",
                   "Trying to execute an unexpected instruction"),
        clEnumValN(Executor::External, "External",
                   "External objects referenced"),
        clEnumValN(Executor::Free, "Free", "Freeing invalid memory"),
        clEnumValN(Executor::Model, "Model", "Memory model limit hit"),
        clEnumValN(Executor::Overflow, "Overflow", "An overflow occurred"),
        clEnumValN(Executor::Ptr, "Ptr", "Pointer error"),
        clEnumValN(Executor::ReadOnly, "ReadOnly", "Write to read-only memory"),
        clEnumValN(Executor::ReportError, "ReportError",
                   "klee_report_error called"),
        clEnumValN(Executor::User, "User", "Wrong klee_* functions invocation"),
        clEnumValN(Executor::Unhandled, "Unhandled",
                   "Unhandled instruction hit"),
         clEnumValN(Executor::ReplayPath, "ReplayPath",
                    "Hit invalid branch in replay") KLEE_LLVM_CL_VAL_END),
    cl::ZeroOrMore,
    cl::cat(TerminationCat));

cl::opt<unsigned long long> MaxInstructions(
    "max-instructions",
    cl::desc("Stop execution after this many instructions.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(TerminationCat));

cl::opt<unsigned>
    MaxForks("max-forks",
             cl::desc("Only fork this many times.  Set to -1 to disable (default=-1)"),
             cl::init(~0u),
             cl::cat(TerminationCat));

cl::opt<unsigned> MaxDepth(
    "max-depth",
    cl::desc("Only allow this many symbolic branches.  Set to 0 to disable (default=0)"),
    cl::init(0),
    cl::cat(TerminationCat));

cl::opt<unsigned> MaxMemory("max-memory",
                            cl::desc("Refuse to fork when above this amount of "
                                     "memory (in MB) (default=2000)"),
                            cl::init(2000),
                            cl::cat(TerminationCat));

cl::opt<bool> MaxMemoryInhibit(
    "max-memory-inhibit",
    cl::desc(
        "Inhibit forking at memory cap (vs. random terminate) (default=true)"),
    cl::init(true),
    cl::cat(TerminationCat));

cl::opt<unsigned> RuntimeMaxStackFrames(
    "max-stack-frames",
    cl::desc("Terminate a state after this many stack frames.  Set to 0 to "
             "disable (default=8192)"),
    cl::init(8192),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticForkPct(
    "max-static-fork-pct", cl::init(1.),
    cl::desc("Maximum percentage spent by an instruction forking out of the "
             "forking of all instructions (default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticSolvePct(
    "max-static-solve-pct", cl::init(1.),
    cl::desc("Maximum percentage of solving time that can be spent by a single "
             "instruction over total solving time for all instructions "
             "(default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticCPForkPct(
    "max-static-cpfork-pct", cl::init(1.),
    cl::desc("Maximum percentage spent by an instruction of a call path "
             "forking out of the forking of all instructions in the call path "
             "(default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<double> MaxStaticCPSolvePct(
    "max-static-cpsolve-pct", cl::init(1.),
    cl::desc("Maximum percentage of solving time that can be spent by a single "
             "instruction of a call path over total solving time for all "
             "instructions (default=1.0 (always))"),
    cl::cat(TerminationCat));

cl::opt<std::string> TimerInterval(
    "timer-interval",
    cl::desc("Minimum interval to check timers. "
             "Affects -max-time, -istats-write-interval, -stats-write-interval, and -uncovered-update-interval (default=1s)"),
    cl::init("1s"),
    cl::cat(TerminationCat));


/*** Debugging options ***/

/// The different query logging solvers that can switched on/off
enum PrintDebugInstructionsType {
  STDERR_ALL, ///
  STDERR_SRC,
  STDERR_COMPACT,
  FILE_ALL,    ///
  FILE_SRC,    ///
  FILE_COMPACT ///
};

llvm::cl::bits<PrintDebugInstructionsType> DebugPrintInstructions(
    "debug-print-instructions",
    llvm::cl::desc("Log instructions during execution."),
    llvm::cl::values(
        clEnumValN(STDERR_ALL, "all:stderr",
                   "Log all instructions to stderr "
                   "in format [src, inst_id, "
                   "llvm_inst]"),
        clEnumValN(STDERR_SRC, "src:stderr",
                   "Log all instructions to stderr in format [src, inst_id]"),
        clEnumValN(STDERR_COMPACT, "compact:stderr",
                   "Log all instructions to stderr in format [inst_id]"),
        clEnumValN(FILE_ALL, "all:file",
                   "Log all instructions to file "
                   "instructions.txt in format [src, "
                   "inst_id, llvm_inst]"),
        clEnumValN(FILE_SRC, "src:file",
                   "Log all instructions to file "
                   "instructions.txt in format [src, "
                   "inst_id]"),
        clEnumValN(FILE_COMPACT, "compact:file",
                   "Log all instructions to file instructions.txt in format "
                   "[inst_id]") KLEE_LLVM_CL_VAL_END),
    llvm::cl::CommaSeparated,
    cl::cat(DebugCat));

#ifdef HAVE_ZLIB_H
cl::opt<bool> DebugCompressInstructions(
    "debug-compress-instructions", cl::init(false),
    cl::desc(
        "Compress the logged instructions in gzip format (default=false)."),
    cl::cat(DebugCat));
#endif

cl::opt<bool> DebugCheckForImpliedValues(
    "debug-check-for-implied-values", cl::init(false),
    cl::desc("Debug the implied value optimization"),
    cl::cat(DebugCat));
/*** HASE related Options ***/
cl::opt<bool>
    CallSolver("call-solver", cl::init(true),
               cl::desc("Call solver at Executor::fork. (default=true)"),
               cl::cat(HASECat));
cl::opt<bool>
    DoOutofBoundaryCheck("oob-check", cl::init(true),
                         cl::desc("Disable out of boundary check during memory "
                                  "operations (default=true)"),
                         cl::cat(HASECat));
cl::opt<bool> AllowSymbolicPOSIXCall(
    "sym-posix-call", cl::init(false),
    cl::desc("Try concretizing symbolic POSIX call args. If disable this flag, "
             "klee will stop replaying or dump symbolic args for "
             "ptwrite instrumentation (default=false)"),
    cl::cat(HASECat));
cl::opt<bool> AllowSymbolicMalloc(
    "sym-malloc", cl::init(false),
    cl::desc("Try concretizing the size of a malloc. If disable this flag, "
             "klee will stop replaying and dump symbolic args for "
             "ptwrite instrumentation (default=false)"),
    cl::cat(HASECat));
cl::opt<bool>
    DebugScheduling("debug-schedule", cl::init(false),
                    cl::desc("Print debug info related to scheduling, context "
                             "switch, etc. (default=false)"),
                    cl::cat(HASECat));
} // namespace


// exported command line options
namespace klee {
  extern cl::opt<std::string> OracleKTest;
} // namespace klee
namespace klee {
  RNG theRNG;
}

// XXX hack
extern "C" unsigned dumpStates, dumpPTree;
unsigned dumpStates = 0, dumpPTree = 0;

const char *Executor::TerminateReasonNames[] = {
  [ Abort ] = "abort",
  [ Assert ] = "assert",
  [ BadVectorAccess ] = "bad_vector_access",
  [ Exec ] = "exec",
  [ External ] = "external",
  [ Free ] = "free",
  [ Model ] = "model",
  [ Overflow ] = "overflow",
  [ Ptr ] = "ptr",
  [ ReadOnly ] = "readonly",
  [ ReportError ] = "reporterror",
  [ User ] = "user",
  [ Unhandled ] = "xxx",
  [ ReplayPath ] = "replaypath",
};


Executor::Executor(LLVMContext &ctx, const InterpreterOptions &opts,
                   InterpreterHandler *ih)
    : Interpreter(opts), interpreterHandler(ih), searcher(0),
      externalDispatcher(new ExternalDispatcher(ctx)), statsTracker(0),
      pathWriter(0), pathDataRecWriter(0), symPathWriter(0),
      stackPathWriter(0), consPathWriter(0), statsPathWriter(0),
      specialFunctionHandler(0), timers{time::Span(TimerInterval)},
      replayKTest(0), oracle_eval(0), replayPath(0), usingSeeds(0),
      atMemoryLimit(false), inhibitForking(false), haltExecution(false),
      ivcEnabled(false), debugLogBuffer(debugBufferString), info_requested(false) {

  const time::Span maxTime{MaxTime};
  if (maxTime) timers.add(
        std::make_unique<Timer>(maxTime, [&]{
        klee_message("HaltTimer invoked");
        setHaltExecution(true);
      }));

  coreSolverTimeout = time::Span{MaxCoreSolverTime};
  if (coreSolverTimeout) UseForkedCoreSolver = true;
  Solver *coreSolver = klee::createCoreSolver(CoreSolverToUse);
  if (!coreSolver) {
    klee_error("Failed to create core solver\n");
  }

  Solver *solver = constructSolverChain(
      coreSolver,
      interpreterHandler->getOutputFilename(ALL_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(ALL_QUERIES_KQUERY_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_KQUERY_FILE_NAME));

  this->solver = new TimingSolver(solver, EqualitySubstitution);

  if (OracleKTest != "") {
    oracle_eval = new OracleEvaluator(OracleKTest);
  }

  memory = new MemoryManager(&arrayCache);

  initializeSearchOptions();

  if (OnlyOutputStatesCoveringNew && !StatsTracker::useIStats())
    klee_error("To use --only-output-states-covering-new, you need to enable --output-istats.");

  if (DebugPrintInstructions.isSet(FILE_ALL) ||
      DebugPrintInstructions.isSet(FILE_COMPACT) ||
      DebugPrintInstructions.isSet(FILE_SRC)) {
    std::string debug_file_name =
        interpreterHandler->getOutputFilename("instructions.txt");
    std::string error;
#ifdef HAVE_ZLIB_H
    if (!DebugCompressInstructions) {
#endif
      debugInstFile = klee_open_output_file(debug_file_name, error);
#ifdef HAVE_ZLIB_H
    } else {
      debug_file_name.append(".gz");
      debugInstFile = klee_open_compressed_output_file(debug_file_name, error);
    }
#endif
    if (!debugInstFile) {
      klee_error("Could not open file %s : %s", debug_file_name.c_str(),
                 error.c_str());
    }
  }
}

llvm::Module *
Executor::setModule(std::vector<std::unique_ptr<llvm::Module>> &modules,
                    const ModuleOptions &opts) {
  assert(!kmodule && !modules.empty() &&
         "can only register one module"); // XXX gross

  kmodule = std::unique_ptr<KModule>(new KModule());

  // Preparing the final module happens in multiple stages

  // Link with KLEE intrinsics library before running any optimizations
  SmallString<128> LibPath(opts.LibraryDir);
  llvm::sys::path::append(LibPath, "libkleeRuntimeIntrinsic.bca");
  std::string error;
  if (!klee::loadFile(LibPath.str(), modules[0]->getContext(), modules,
                      error)) {
    klee_error("Could not load KLEE intrinsic file %s", LibPath.c_str());
  }

  // 1.) Link the modules together
  while (kmodule->link(modules, opts.EntryPoint)) {
    // 2.) Apply different instrumentation
    kmodule->instrument(opts);
  }

  // 3.) Optimise and prepare for KLEE

  // Create a list of functions that should be preserved if used
  std::vector<const char *> preservedFunctions;
  specialFunctionHandler = new SpecialFunctionHandler(*this);
  specialFunctionHandler->prepare(preservedFunctions);

  preservedFunctions.push_back(opts.EntryPoint.c_str());

  // Preserve the free-standing library calls
  preservedFunctions.push_back("memset");
  preservedFunctions.push_back("memcpy");
  preservedFunctions.push_back("memcmp");
  preservedFunctions.push_back("memmove");

  // Assign ID for newly added instructions
  std::string prefix = "POST";
  KModule::assignID(kmodule->module.get(), prefix);

  kmodule->optimiseAndPrepare(opts, preservedFunctions);
  kmodule->checkModule();

  // 4.) Manifest the module
  kmodule->manifest(interpreterHandler, StatsTracker::useStatistics());

  specialFunctionHandler->bind();

  if (StatsTracker::useStatistics() || userSearcherRequiresMD2U()) {
    statsTracker =
      new StatsTracker(*this,
                       interpreterHandler->getOutputFilename("assembly.ll"),
                       userSearcherRequiresMD2U());
  }

  // Initialize the context.
  DataLayout *TD = kmodule->targetData.get();
  Context::initialize(TD->isLittleEndian(),
                      (Expr::Width)TD->getPointerSizeInBits());

  return kmodule->module.get();
}

Executor::~Executor() {
  delete memory;
  delete externalDispatcher;
  delete specialFunctionHandler;
  delete statsTracker;
  delete solver;
}

/***/

void Executor::initializeGlobalObject(ExecutionState &state, ObjectState *os,
                                      const Constant *c,
                                      unsigned offset) {
  const auto targetData = kmodule->targetData.get();
  if (const ConstantVector *cp = dyn_cast<ConstantVector>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cp->getType()->getElementType());
    for (unsigned i=0, e=cp->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cp->getOperand(i),
			     offset + i*elementSize);
  } else if (isa<ConstantAggregateZero>(c)) {
    unsigned i, size = targetData->getTypeStoreSize(c->getType());
    for (i=0; i<size; i++)
      os->write8(offset+i, (uint8_t) 0, Expr::FLAG_INITIALIZATION, nullptr);
  } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(ca->getType()->getElementType());
    for (unsigned i=0, e=ca->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, ca->getOperand(i),
			     offset + i*elementSize);
  } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
    const StructLayout *sl =
      targetData->getStructLayout(cast<StructType>(cs->getType()));
    for (unsigned i=0, e=cs->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cs->getOperand(i),
			     offset + sl->getElementOffset(i));
  } else if (const ConstantDataSequential *cds =
               dyn_cast<ConstantDataSequential>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cds->getElementType());
    for (unsigned i=0, e=cds->getNumElements(); i != e; ++i)
      initializeGlobalObject(state, os, cds->getElementAsConstant(i),
                             offset + i*elementSize);
  } else if (!isa<UndefValue>(c) && !isa<MetadataAsValue>(c)) {
    unsigned StoreBits = targetData->getTypeStoreSizeInBits(c->getType());
    ref<ConstantExpr> C = evalConstant(c);

    // Extend the constant if necessary;
    assert(StoreBits >= C->getWidth() && "Invalid store size!");
    if (StoreBits > C->getWidth())
      C = C->ZExt(StoreBits);

    os->write(offset, C, Expr::FLAG_INITIALIZATION, nullptr);
  }
}

MemoryObject * Executor::addExternalObject(ExecutionState &state,
                                           void *addr, unsigned size,
                                           bool isReadOnly) {
  auto mo = memory->allocateFixed(reinterpret_cast<std::uint64_t>(addr),
                                  size, nullptr);
  ObjectState *os = bindObjectInState(state, mo, false);
  for(unsigned i = 0; i < size; i++)
    os->write8(i, ((uint8_t*)addr)[i], Expr::FLAG_INITIALIZATION, nullptr);
  if(isReadOnly)
    os->setReadOnly(true);
  return mo;
}


extern void *__dso_handle __attribute__ ((__weak__));

void Executor::initializeGlobals(ExecutionState &state) {
  Module *m = kmodule->module.get();

  if (m->getModuleInlineAsm() != "")
    klee_warning("executable has module level assembly (ignoring)");
  // represent function globals using the address of the actual llvm function
  // object. given that we use malloc to allocate memory in states this also
  // ensures that we won't conflict. we don't need to allocate a memory object
  // since reading/writing via a function pointer is unsupported anyway.
  for (Module::iterator i = m->begin(), ie = m->end(); i != ie; ++i) {
    Function *f = &*i;
    ref<ConstantExpr> addr(0);

    // If the symbol has external weak linkage then it is implicitly
    // not defined in this module; if it isn't resolvable then it
    // should be null.
    if (f->hasExternalWeakLinkage() &&
        !externalDispatcher->resolveSymbol(f->getName())) {
      addr = Expr::createPointer(0);
    } else {
      addr = Expr::createPointer(reinterpret_cast<std::uint64_t>(f));
      legalFunctions.insert(reinterpret_cast<std::uint64_t>(f));
    }

    globalAddresses.insert(std::make_pair(f, addr));
  }

#ifndef WINDOWS
  int *errno_addr = getErrnoLocation(state);
  MemoryObject *errnoObj =
      addExternalObject(state, (void *)errno_addr, sizeof *errno_addr, false);
  // Copy values from and to program space explicitly
  errnoObj->isUserSpecified = true;
#endif

  // Disabled, we don't want to promote use of live externals.
#ifdef HAVE_CTYPE_EXTERNALS
#ifndef WINDOWS
#ifndef DARWIN
  /* from /usr/include/ctype.h:
       These point into arrays of 384, so they can be indexed by any `unsigned
       char' value [0,255]; by EOF (-1); or by any `signed char' value
       [-128,-1).  ISO C requires that the ctype functions work for `unsigned */
  const uint16_t **addr = __ctype_b_loc();
  addExternalObject(state, const_cast<uint16_t*>(*addr-128),
                    384 * sizeof **addr, true);
  addExternalObject(state, addr, sizeof(*addr), true);

  const int32_t **lower_addr = __ctype_tolower_loc();
  addExternalObject(state, const_cast<int32_t*>(*lower_addr-128),
                    384 * sizeof **lower_addr, true);
  addExternalObject(state, lower_addr, sizeof(*lower_addr), true);

  const int32_t **upper_addr = __ctype_toupper_loc();
  addExternalObject(state, const_cast<int32_t*>(*upper_addr-128),
                    384 * sizeof **upper_addr, true);
  addExternalObject(state, upper_addr, sizeof(*upper_addr), true);
#endif
#endif
#endif

  // allocate and initialize globals, done in two passes since we may
  // need address of a global in order to initialize some other one.

  // allocate memory objects for all globals
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    const GlobalVariable *v = &*i;
    size_t globalObjectAlignment = getAllocationAlignment(v);
    if (i->isDeclaration()) {
      // FIXME: We have no general way of handling unknown external
      // symbols. If we really cared about making external stuff work
      // better we could support user definition, or use the EXE style
      // hack where we check the object file information.

      Type *ty = i->getType()->getElementType();
      uint64_t size = 0;
      if (ty->isSized()) {
	size = kmodule->targetData->getTypeStoreSize(ty);
      } else {
        klee_warning("Type for %.*s is not sized", (int)i->getName().size(),
			i->getName().data());
      }

      // XXX - DWD - hardcode some things until we decide how to fix.
#ifndef WINDOWS
      if (i->getName() == "_ZTVN10__cxxabiv117__class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv120__si_class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv121__vmi_class_type_infoE") {
        size = 0x2C;
      }
#endif

      if (size == 0) {
        klee_warning("Unable to find size for global variable: %.*s (use will result in out of bounds access)",
			(int)i->getName().size(), i->getName().data());
      }

      MemoryObject *mo = memory->allocate(size, /*isLocal=*/false,
                                          /*isGlobal=*/true, /*allocSite=*/v,
                                          /*alignment=*/globalObjectAlignment,
                                          /*isInPOSIX*/false);
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(v, mo));
      globalAddresses.insert(std::make_pair(v, mo->getBaseExpr()));

      // Program already running = object already initialized.  Read
      // concrete value and write it to our copy.
      if (size) {
        void *addr;
        if (i->getName() == "__dso_handle") {
          addr = &__dso_handle; // wtf ?
        } else {
          addr = externalDispatcher->resolveSymbol(i->getName());
        }
        if (!addr)
          klee_error("unable to load symbol(%s) while initializing globals.",
                     i->getName().data());

        for (unsigned offset=0; offset<mo->size; offset++)
          os->write8(offset, ((unsigned char*)addr)[offset], Expr::FLAG_INITIALIZATION, nullptr);
      }
    } else {
      Type *ty = i->getType()->getElementType();
      uint64_t size = kmodule->targetData->getTypeStoreSize(ty);
      MemoryObject *mo = memory->allocate(size, /*isLocal=*/false,
                                          /*isGlobal=*/true, /*allocSite=*/v,
                                          /*alignment=*/globalObjectAlignment,
                                          /*isInPOSIX*/false);
      if (!mo)
        llvm::report_fatal_error("out of memory");
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(v, mo));
      globalAddresses.insert(std::make_pair(v, mo->getBaseExpr()));

      if (!i->hasInitializer())
          os->initializeToRandom();
    }
  }

  // link aliases to their definitions (if bound)
  for (auto i = m->alias_begin(), ie = m->alias_end(); i != ie; ++i) {
    // Map the alias to its aliasee's address. This works because we have
    // addresses for everything, even undefined functions.

    // Alias may refer to other alias, not necessarily known at this point.
    // Thus, resolve to real alias directly.
    const GlobalAlias *alias = &*i;
    while (const auto *ga = dyn_cast<GlobalAlias>(alias->getAliasee())) {
      assert(ga != alias && "alias pointing to itself");
      alias = ga;
    }

    globalAddresses.insert(std::make_pair(&*i, evalConstant(alias->getAliasee())));
  }

  // once all objects are allocated, do the actual initialization
  // remember constant objects to initialise their counter part for external
  // calls
  std::vector<ObjectState *> constantObjects;
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->hasInitializer()) {
      const GlobalVariable *v = &*i;
      MemoryObject *mo = globalObjects.find(v)->second;
      const ObjectState *os = state.addressSpace.findObject(mo);
      assert(os);
      ObjectState *wos = state.addressSpace.getWriteable(mo, os);

      initializeGlobalObject(state, wos, i->getInitializer(), 0);
      if (i->isConstant())
        constantObjects.emplace_back(wos);
    }
  }

  // initialise constant memory that is potentially used with external calls
  if (!constantObjects.empty()) {
    // initialise the actual memory with constant values
    state.addressSpace.copyOutConcretes();

    // mark constant objects as read-only
    for (auto obj : constantObjects)
      obj->setReadOnly(true);
  }
}

void Executor::branch(ExecutionState &state,
                      const std::vector< ref<Expr> > &conditions,
                      std::vector<ExecutionState*> &result) {
  TimerStatIncrementer timer(stats::branchTime);
  unsigned N = conditions.size();
  assert(N);
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
    seedMap.find(&state);
  bool isSeeding = (it != seedMap.end());

  if (MaxForks!=~0u && stats::forks >= MaxForks) {
    unsigned next = theRNG.getInt32() % N;
    for (unsigned i=0; i<N; ++i) {
      if (i == next) {
        result.push_back(&state);
      } else {
        result.push_back(NULL);
      }
    }
  } else {
    stats::forks += N-1;

    // XXX do proper balance or keep random?
    // NOTE: I guess they only have a binary tree to keep tracking branched ExecutionState
    // To avoid a deep tree, you can't simply let all successors branched (forked) from the same root state.
    // Here the current approach is to randomly select the original state or forked new states (note that forked new states are identical to the original one).
    // Proper balance might mean creating a balanced binary tree when it has to branch many states from one state.
    result.push_back(&state);
    for (unsigned i=1; i<N; ++i) {
      ExecutionState *es = result[theRNG.getInt32() % i];
      ExecutionState *ns = es->branch();
      addedStates.push_back(ns);
      result.push_back(ns);
      processTree->attach(es->ptreeNode, ns, es);
    }
  }

  if (isSeeding) {
    // If necessary redistribute seeds to match conditions, killing
    // states if necessary due to OnlyReplaySeeds (inefficient but
    // simple).

    std::vector<SeedInfo> seeds = it->second;
    seedMap.erase(it);

    // Assume each seed only satisfies one condition (necessarily true
    // when conditions are mutually exclusive and their conjunction is
    // a tautology).
    for (std::vector<SeedInfo>::iterator siit = seeds.begin(),
           siie = seeds.end(); siit != siie; ++siit) {
      unsigned i;
      for (i=0; i<N; ++i) {
        ref<ConstantExpr> res;
        bool success =
          solver->getValue(state, siit->assignment.evaluate(conditions[i]),
                           res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue())
          break;
      }

      // If we didn't find a satisfying condition randomly pick one
      // (the seed will be patched).
      if (i==N)
        i = theRNG.getInt32() % N;

      // Extra check in case we're replaying seeds with a max-fork
      if (result[i])
        seedMap[result[i]].push_back(*siit);
    }

    if (OnlyReplaySeeds) {
      for (unsigned i=0; i<N; ++i) {
        if (result[i] && !seedMap.count(result[i])) {
          terminateState(*result[i]);
          result[i] = NULL;
        }
      }
    }
  }

  for (unsigned i=0; i<N; ++i)
    if (result[i])
      addConstraint(*result[i], conditions[i]);
}

Executor::StatePair
Executor::fork(ExecutionState &current, ref<Expr> condition, bool isInternal) {
  TimerStatIncrementer timer(stats::forkTime);
  Solver::Validity res;
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
    seedMap.find(&current);
  bool isSeeding = it != seedMap.end();

  // When (!isSeeding), the condition is non-constant, and states already forked
  // exceed configured threshold (forked too many || solver costs too many time, etc.):
  // the following code will pick one possible value (assignment) for the condition,
  // and add that assignment as a constraint to current state.
  if (!isSeeding && !isa<ConstantExpr>(condition) &&
      (MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
       MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.) &&
      statsTracker->elapsed() > time::seconds(60)) {
    StatisticManager &sm = *theStatisticManager;
    CallPathNode *cpn = current.stack().back().callPathNode;
    if ((MaxStaticForkPct<1. &&
         sm.getIndexedValue(stats::forks, sm.getIndex()) >
         stats::forks*MaxStaticForkPct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::forks) >
                 stats::forks*MaxStaticCPForkPct)) ||
        (MaxStaticSolvePct<1 &&
         sm.getIndexedValue(stats::solverTime, sm.getIndex()) >
         stats::solverTime*MaxStaticSolvePct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::solverTime) >
                 stats::solverTime*MaxStaticCPSolvePct))) {
      ref<ConstantExpr> value;
      bool success = solver->getValue(current, condition, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      addConstraint(current, EqExpr::create(value, condition));
      condition = value;
    }
  }

  if (CallSolver || !current.shouldRecord() || isInternal) {
    time::Span timeout = coreSolverTimeout;
    time::Span fork_queryCost_begin = current.queryCost;
    if (isSeeding)
      timeout *= static_cast<unsigned>(it->second.size());
    solver->setTimeout(timeout);
    bool success = solver->evaluate(current, condition, res);
    solver->setTimeout(time::Span());
    current.fork_queryCost += current.queryCost - fork_queryCost_begin;
    if (!success) {
      current.pc() = current.prevPC();
      terminateStateEarly(current, "Query timed out (fork).");
      return StatePair(0, 0);
    }
  }
  else {
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
      if (CE->isTrue()) {
        res = Solver::True;
      }
      else {
        res = Solver::False;
      }
    }
    else {
      res = Solver::Unknown;
    }
  }

  ref<Expr> new_constraint;
  if (!isSeeding) {
    // replaying, read recorded branch condition
    if (replayPath && !isInternal) {
      if (res==Solver::True) { // Concrete branch
        if (current.shouldRecord()) {
          AssertNextBranchTaken(current, true);
        }
      } else if (res==Solver::False) { // Concrete branch
        if (current.shouldRecord()) {
          AssertNextBranchTaken(current, false);
        }
      } else {
        // in replay mode, symbolic branch.
        // add constraints according to recorded replayPath
        assert(current.isInUserMain && "We assumed that during replay, uClibc doesn't need recorded path, wrong!");
        assert(!current.isInPOSIX() && "We assumed that no constraints will be added inside POSIX runtime, wrong!");
        getNextBranchConstraint(current, condition, new_constraint, res);
      }
    } else if (res==Solver::Unknown) {
      assert(!replayKTest && "in replay mode, only one branch can be true.");

      if ((MaxMemoryInhibit && atMemoryLimit) ||
          current.forkDisabled ||
          inhibitForking ||
          (MaxForks!=~0u && stats::forks >= MaxForks)) {
        // When (!isSeeding):
        // Do not fork later, randomly choose a bool value for this unknown fork here.
        if (MaxMemoryInhibit && atMemoryLimit)
          klee_warning_once(0, "skipping fork (memory cap exceeded)");
        else if (current.forkDisabled)
          klee_warning_once(0, "skipping fork (fork disabled on current path)");
        else if (inhibitForking)
          klee_warning_once(0, "skipping fork (fork disabled globally)");
        else
          klee_warning_once(0, "skipping fork (max-forks reached)");

        getConstraintFromBool(condition, new_constraint, res, theRNG.getBool());
      }
    }
  }

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.
  //
  // When (isSeeding), do not fork later if current state's SeedInfo will not
  // cover both true branch and false branch.
  if (isSeeding &&
      (current.forkDisabled || OnlyReplaySeeds) &&
      res == Solver::Unknown) {
    bool trueSeed=false, falseSeed=false;
    // Is seed extension still ok here?
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> res;
      bool success =
        solver->getValue(current, siit->assignment.evaluate(static_cast<const ref<Expr>>(condition)), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res->isTrue()) {
        trueSeed = true;
      } else {
        falseSeed = true;
      }
      if (trueSeed && falseSeed)
        break;
    }
    if (!(trueSeed && falseSeed)) {
      assert(trueSeed || falseSeed);
      getConstraintFromBool(condition, new_constraint, res, trueSeed);
    }
  }
  if (!new_constraint.isNull() && current.isInPOSIX()) {
    current.dumpStack();
    klee_error("Adding new constraint within POSIX runtime");
  }

  // XXX - even if the constraint is provable one way or the other we
  // can probably benefit by adding this constraint and allowing it to
  // reduce the other constraints. For example, if we do a binary
  // search on a particular value, and then see a comparison against
  // the value it has been fixed at, we should take this as a nice
  // hint to just use the single constraint instead of all the binary
  // search ones. If that makes sense.
  if (res == Solver::True || res == Solver::False) {
    if (!isInternal && current.shouldRecord()) {
        record1BitAtFork(current, res);
        ++current.nbranches_rec;
        dumpStateAtFork(current, new_constraint);
    }
    // dump first, then add new constraint
    if (!new_constraint.isNull()) {
      bool valid_constraint = addConstraint(current, new_constraint);
      if (!valid_constraint) {
        terminateStateOnError(current, "add a invalid constraint", Abort);
      }
    }
    if (res == Solver::True) {
      return StatePair(&current, 0);
    }
    else {
      return StatePair(0, &current);
    }
  } else {
    // res is still Solver::Unknown in this branch, which means current state
    // should fork here.
    ExecutionState *falseState, *trueState = &current;
    if (replayPath) {
      klee_warning("ExecutionState forks in replay mode:");
      current.dumpStack();
    }

    ++stats::forks;
    
    falseState = trueState->branch();
    addedStates.push_back(falseState);

    if (it != seedMap.end()) {
      std::vector<SeedInfo> seeds = it->second;
      it->second.clear();
      std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
      std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
      for (std::vector<SeedInfo>::iterator siit = seeds.begin(),
             siie = seeds.end(); siit != siie; ++siit) {
        ref<ConstantExpr> res;
        bool success =
          solver->getValue(current, siit->assignment.evaluate(static_cast<const ref<Expr>>(condition)), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue()) {
          trueSeeds.push_back(*siit);
        } else {
          falseSeeds.push_back(*siit);
        }
      }

      bool swapInfo = false;
      if (trueSeeds.empty()) {
        if (&current == trueState) swapInfo = true;
        seedMap.erase(trueState);
      }
      if (falseSeeds.empty()) {
        if (&current == falseState) swapInfo = true;
        seedMap.erase(falseState);
      }
      if (swapInfo) {
        std::swap(trueState->coveredNew, falseState->coveredNew);
        std::swap(trueState->coveredLines, falseState->coveredLines);
      }
    }

    processTree->attach(current.ptreeNode, falseState, trueState);
    ref<Expr> true_constraint = condition;
    ref<Expr> false_constraint = Expr::createIsZero(condition);

    if (!isInternal && !current.isInPOSIX()) {
      assert(current.isInUserMain && "We assumed state fork won't happen in uClibc, wrong!");
      record1BitAtFork(*trueState, Solver::True);
      dumpStateAtFork(*trueState, true_constraint);
      record1BitAtFork(*falseState, Solver::False);
      dumpStateAtFork(*falseState, false_constraint);
    }

    if (symPathWriter) {
      falseState->symPathOS = symPathWriter->open(current.symPathOS);
      if (!isInternal) {
        trueState->symPathOS << '1';
        falseState->symPathOS << '0';
      }
    }


    // when current state forks, all dump is done before actually adding constraint
    // Such behaviour should be consistent with no-fork situation.
    if (!addConstraint(*trueState, true_constraint)) {
      trueState = nullptr;
    }
    if (!addConstraint(*falseState, false_constraint)) {
      falseState = nullptr;
    }

    // Kinda gross, do we even really still want this option?
    if (MaxDepth && MaxDepth<=trueState->depth) {
      terminateStateEarly(*trueState, "max-depth exceeded.");
      terminateStateEarly(*falseState, "max-depth exceeded.");
      return StatePair(0, 0);
    }

    return StatePair(trueState, falseState);
  }
}

bool Executor::addConstraint(ExecutionState &state, ref<Expr> condition) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
    if (!CE->isTrue())
      llvm::report_fatal_error("attempt to add invalid constraint");
    return false;
  }

  // Check to see if this constraint violates seeds.
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
    seedMap.find(&state);
  if (it != seedMap.end()) {
    bool warn = false;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
           siie = it->second.end(); siit != siie; ++siit) {
      bool res;
      bool success =
        solver->mustBeFalse(state, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        siit->patchSeed(state, condition, solver);
        warn = true;
      }
    }
    if (warn)
      klee_warning("seeds patched for violating constraint");
  }

  if (oracle_eval) {
    ref<Expr> res = oracle_eval->visit(condition);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(res)) {
      if (!CE->isTrue()) {
        terminateStateOnError(state, "Adding False Constaint", Abort);
        return false;
      }
    }
    else {
      terminateStateOnError(state,
          "NonConstant Expr returned by OracleEvaluator", Abort);
      return false;
    }
  }
  bool valid = state.addConstraint(condition);
  if (ivcEnabled)
    doImpliedValueConcretization(state, condition,
                                 ConstantExpr::alloc(1, Expr::Bool));
  return valid;
}

const Cell& Executor::eval(KInstruction *ki, unsigned index,
                           ExecutionState &state) const {
  assert(index < ki->inst->getNumOperands());
  int vnumber = ki->operands[index];

  assert(vnumber != -1 &&
         "Invalid operand to eval(), not a value or constant!");

  // Determine if this is a constant or not.
  if (vnumber < 0) {
    unsigned index = -vnumber - 2;
    return kmodule->constantTable[index];
  } else {
    unsigned index = vnumber;
    StackFrame &sf = state.stack().back();
    return sf.locals[index];
  }
}

void Executor::bindLocal(KInstruction *target, ExecutionState &state,
                         ref<Expr> value) {
  getDestCell(state, target).value = value;
  // I mark LLVM functions from POSIX and LIBC with special function
  // attributes. Then we only bind a kinst to a symbolic expression in either of
  // the following scenarios:
  //   1) the symbolic expression has not been bound to any kinst
  //   2) the kinst is from the target program (not from POSIX nor LIBC) and it
  //   has lower frequency (less recording overhead)
  // NOTE: Since kinst tracked in this way is no longer guaranteed to be the
  // latest instruction bind this symbolic value to a llvm register, I should
  // never use Expr.kinst to locate a llvm register.
  if (!value->kinst || (value->kinst->frequency > target->frequency &&
                        state.isInTargetProgram())) {
    value->kinst = target;
    value->flags |= Expr::Expr::FLAG_INSTRUCTION_ROOT;
  }
}

void Executor::bindArgument(KFunction *kf, unsigned index,
                            ExecutionState &state, ref<Expr> value) {
  getArgumentCell(state, kf, index).value = value;
}

ref<Expr> Executor::toUnique(const ExecutionState &state,
                             ref<Expr> &e) {
  ref<Expr> result = e;

  if (!isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool isTrue = false;
    e = optimizer.optimizeExpr(e, true);
    solver->setTimeout(coreSolverTimeout);
    if (solver->getValue(state, e, value)) {
      ref<Expr> cond = EqExpr::create(e, value);
      cond = optimizer.optimizeExpr(cond, false);
      if (solver->mustBeTrue(state, cond, isTrue) && isTrue)
        result = value;
    }
    solver->setTimeout(time::Span());
  }

  return result;
}


/* Concretize the given expression, and return a possible constant value.
   'reason' is just a documentation string stating the reason for concretization. */
ref<klee::ConstantExpr>
Executor::toConstant(ExecutionState &state,
                     ref<Expr> e,
                     const char *reason) {
  e = state.constraints.simplifyExpr(e);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE;

  ref<ConstantExpr> value;
  bool success = solver->getValue(state, e, value);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;

  std::string str;
  llvm::raw_string_ostream os(str);
  os << "silently concretizing (reason: " << reason << ") expression " << e
     << " to value " << value << " (" << (*(state.pc())).info->file << ":"
     << (*(state.pc())).info->line << ")";

  if (AllExternalWarnings)
    klee_warning("%s", os.str().c_str());
  else
    klee_warning_once(reason, "%s", os.str().c_str());

  addConstraint(state, EqExpr::create(e, value));

  return value;
}

void Executor::executeGetValue(ExecutionState &state,
                               ref<Expr> e,
                               KInstruction *target) {
  e = state.constraints.simplifyExpr(e);
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
    seedMap.find(&state);
  if (it==seedMap.end() || isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    e = optimizer.optimizeExpr(e, true);
    bool success = solver->getValue(state, e, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    bindLocal(target, state, value);
  } else {
    std::set< ref<Expr> > values;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
           siie = it->second.end(); siit != siie; ++siit) {
      ref<Expr> cond = siit->assignment.evaluate(e);
      cond = optimizer.optimizeExpr(cond, true);
      ref<ConstantExpr> value;
      bool success = solver->getValue(state, cond, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      values.insert(value);
    }

    std::vector< ref<Expr> > conditions;
    for (std::set< ref<Expr> >::iterator vit = values.begin(),
           vie = values.end(); vit != vie; ++vit)
      conditions.push_back(EqExpr::create(e, *vit));

    std::vector<ExecutionState*> branches;
    branch(state, conditions, branches);

    std::vector<ExecutionState*>::iterator bit = branches.begin();
    for (std::set< ref<Expr> >::iterator vit = values.begin(),
           vie = values.end(); vit != vie; ++vit) {
      ExecutionState *es = *bit;
      if (es)
        bindLocal(target, *es, *vit);
      ++bit;
    }
  }
}

void Executor::printDebugInstructions(ExecutionState &state) {
  // check do not print
  if (DebugPrintInstructions.getBits() == 0)
	  return;

  llvm::raw_ostream *stream = 0;
  if (DebugPrintInstructions.isSet(STDERR_ALL) ||
      DebugPrintInstructions.isSet(STDERR_SRC) ||
      DebugPrintInstructions.isSet(STDERR_COMPACT))
    stream = &llvm::errs();
  else
    stream = &debugLogBuffer;

  if (!DebugPrintInstructions.isSet(STDERR_COMPACT) &&
      !DebugPrintInstructions.isSet(FILE_COMPACT)) {
    (*stream) << "     " << state.pc()->getSourceLocation() << ":";
  }

  (*stream) << state.pc()->info->assemblyLine;

  if (DebugPrintInstructions.isSet(STDERR_ALL) ||
      DebugPrintInstructions.isSet(FILE_ALL))
    (*stream) << ":" << *(state.pc()->inst);
  (*stream) << "\n";

  if (DebugPrintInstructions.isSet(FILE_ALL) ||
      DebugPrintInstructions.isSet(FILE_COMPACT) ||
      DebugPrintInstructions.isSet(FILE_SRC)) {
    debugLogBuffer.flush();
    (*debugInstFile) << debugLogBuffer.str();
    debugBufferString = "";
  }
}

void Executor::stepInstruction(ExecutionState &state) {
  printDebugInstructions(state);
  if (statsTracker)
    statsTracker->stepInstruction(state);

  ++stats::instructions;
  ++state.steppedInstructions;
  state.prevPC() = state.pc();
  ++state.pc();

  if (stats::instructions == MaxInstructions)
    haltExecution = true;
}

static inline const llvm::fltSemantics *fpWidthToSemantics(unsigned width) {
  switch (width) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(4, 0)
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle();
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble();
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended();
#else
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle;
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble;
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended;
#endif
  default:
    return 0;
  }
}

void Executor::executeCall(ExecutionState &state,
                           KInstruction *ki,
                           Function *f,
                           std::vector< ref<Expr> > &arguments) {
  if (ki) {
    Instruction *I = ki->inst;
    if (I && isa<DbgInfoIntrinsic>(I))
      return;
  }
  if (ki && f && f->isDeclaration()) {
    Instruction *I = ki->inst;
    switch(f->getIntrinsicID()) {
    case Intrinsic::not_intrinsic:
      // state may be destroyed by this call, cannot touch
      callExternalFunction(state, ki, f, arguments);
      break;
    case Intrinsic::fabs: {
      ref<ConstantExpr> arg =
          toConstant(state, eval(ki, 0, state).value, "floating point");
      if (!fpWidthToSemantics(arg->getWidth()))
        return terminateStateOnExecError(
            state, "Unsupported intrinsic llvm.fabs call");

      llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()),
                        arg->getAPValue());
      Res = llvm::abs(Res);

      bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
      break;
    }
    // va_arg is handled by caller and intrinsic lowering, see comment for
    // ExecutionState::varargs
    case Intrinsic::vastart:  {
      StackFrame &sf = state.stack().back();

      // varargs can be zero if no varargs were provided
      if (!sf.varargs)
        return;

      // FIXME: This is really specific to the architecture, not the pointer
      // size. This happens to work for x86-32 and x86-64, however.
      Expr::Width WordSize = Context::get().getPointerWidth();
      if (WordSize == Expr::Int32) {
        executeMemoryOperation(state, true, arguments[0],
                               sf.varargs->getBaseExpr(), ki);
      } else {
        assert(WordSize == Expr::Int64 && "Unknown word size!");

        // x86-64 has quite complicated calling convention. However,
        // instead of implementing it, we can do a simple hack: just
        // make a function believe that all varargs are on stack.
        executeMemoryOperation(state, true, arguments[0],
                               ConstantExpr::create(48, 32), ki); // gp_offset
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0],
                                               ConstantExpr::create(4, 64)),
                               ConstantExpr::create(304, 32), ki); // fp_offset
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0],
                                               ConstantExpr::create(8, 64)),
                               sf.varargs->getBaseExpr(), ki); // overflow_arg_area
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0],
                                               ConstantExpr::create(16, 64)),
                               ConstantExpr::create(0, 64), ki); // reg_save_area
      }
      break;
    }
    case Intrinsic::vaend:
      // va_end is a noop for the interpreter.
      //
      // FIXME: We should validate that the target didn't do something bad
      // with va_end, however (like call it twice).
      break;

    case Intrinsic::vacopy:
      // va_copy should have been lowered.
      //
      // FIXME: It would be nice to check for errors in the usage of this as
      // well.
    default:
      klee_error("unknown intrinsic: %s", f->getName().data());
    }

    if (InvokeInst *ii = dyn_cast<InvokeInst>(I))
      transferToBasicBlock(ii->getNormalDest(), I->getParent(), state);
  } else {
    // Check if maximum stack size was reached.
    // We currently only count the number of stack frames
    if (RuntimeMaxStackFrames && state.stack().size() > RuntimeMaxStackFrames) {
      terminateStateEarly(state, "Maximum stack size reached.");
      klee_warning("Maximum stack size reached.");
      return;
    }

    // FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
    // guess. This just done to avoid having to pass KInstIterator everywhere
    // instead of the actual instruction, since we can't make a KInstIterator
    // from just an instruction (unlike LLVM).
    KFunction *kf = kmodule->functionMap[f];

    state.pushFrame(state.prevPC(), kf);
    state.pc() = kf->instructions;

    if (statsTracker)
      statsTracker->framePushed(state, &state.stack()[state.stack().size()-2]);

     // TODO: support "byval" parameter attribute
     // TODO: support zeroext, signext, sret attributes

    unsigned callingArgs = arguments.size();
    unsigned funcArgs = f->arg_size();
    if (!f->isVarArg()) {
      if (callingArgs > funcArgs) {
        klee_warning_once(f, "calling %s with extra arguments.",
                          f->getName().data());
      } else if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments",
                              User);
        return;
      }
    } else {
      Expr::Width WordSize = Context::get().getPointerWidth();

      if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments",
                              User);
        return;
      }

      StackFrame &sf = state.stack().back();
      unsigned size = 0;
      bool requires16ByteAlignment = false;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work for x86-32 and x86-64, however.
        if (WordSize == Expr::Int32) {
          size += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          Expr::Width argWidth = arguments[i]->getWidth();
          // AMD64-ABI 3.5.7p5: Step 7. Align l->overflow_arg_area upwards to a
          // 16 byte boundary if alignment needed by type exceeds 8 byte
          // boundary.
          //
          // Alignment requirements for scalar types is the same as their size
          if (argWidth > Expr::Int64) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
             size = llvm::alignTo(size, 16);
#else
             size = llvm::RoundUpToAlignment(size, 16);
#endif
             requires16ByteAlignment = true;
          }
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
          size += llvm::alignTo(argWidth, WordSize) / 8;
#else
          size += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
#endif
        }
      }

      MemoryObject *mo = sf.varargs =
          memory->allocate(size, /*isLocal=*/true, /*isGlobal=*/false, state.prevPC()->inst,
                           (requires16ByteAlignment ? 16 : 8),
                           /*isInPOSIX*/(state.isInPOSIX() || !state.isInUserMain));
      if (!mo && size) {
        terminateStateOnExecError(state, "out of memory (varargs)");
        return;
      }

      if (mo) {
        if ((WordSize == Expr::Int64) && (mo->address & 15) &&
            requires16ByteAlignment) {
          // Both 64bit Linux/Glibc and 64bit MacOSX should align to 16 bytes.
          klee_warning_once(
              0, "While allocating varargs: malloc did not align to 16 bytes.");
        }

        ObjectState *os = bindObjectInState(state, mo, true);
        unsigned offset = 0;
        for (unsigned i = funcArgs; i < callingArgs; i++) {
          // FIXME: This is really specific to the architecture, not the pointer
          // size. This happens to work for x86-32 and x86-64, however.
          if (WordSize == Expr::Int32) {
            os->write(offset, arguments[i], Expr::FLAG_INSTRUCTION_ROOT, ki);
            offset += Expr::getMinBytesForWidth(arguments[i]->getWidth());
          } else {
            assert(WordSize == Expr::Int64 && "Unknown word size!");

            Expr::Width argWidth = arguments[i]->getWidth();
            if (argWidth > Expr::Int64) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
              offset = llvm::alignTo(offset, 16);
#else
              offset = llvm::RoundUpToAlignment(offset, 16);
#endif
            }
            os->write(offset, arguments[i], Expr::FLAG_INSTRUCTION_ROOT, ki);
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 9)
            offset += llvm::alignTo(argWidth, WordSize) / 8;
#else
            offset += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
#endif
          }
        }
      }
    }

    unsigned numFormals = f->arg_size();
    for (unsigned i=0; i<numFormals; ++i)
      bindArgument(kf, i, state, arguments[i]);
    if (kf->function->hasFnAttribute("InPOSIX")) {
      bool hasSymbolicArgs = false;
      std::vector<ref<Expr>> symbolicArgs;
      for (unsigned i=0; i<numFormals; ++i) {
        if (arguments[i].get() && !isa<ConstantExpr>(arguments[i])) {
          symbolicArgs.push_back(arguments[i]);
          hasSymbolicArgs = true;
        }
      }
      if (!AllowSymbolicPOSIXCall && hasSymbolicArgs) {
        std::string sbuf;
        llvm::raw_string_ostream sos(sbuf);
        state.dumpStack(sos);
        klee_message("Calling POSIX Runtime with symbolic args:\n%s\n", sos.str().c_str());
        std::string file_path = interpreterHandler->getOutputFilename("symbolicPOSIX.kquery");
        debugDumpConstraintsEval(state, state.constraints, symbolicArgs, file_path.c_str());
        terminateStateOnError(state, "symbolic args in the POSIX", Abort);
      }
    }
  }
}

void Executor::transferToBasicBlock(const BasicBlock *dst, BasicBlock *src,
                                    ExecutionState &state) {
  // Note that in general phi nodes can reuse phi values from the same
  // block but the incoming value is the eval() result *before* the
  // execution of any phi nodes. this is pathological and doesn't
  // really seem to occur, but just in case we run the PhiCleanerPass
  // which makes sure this cannot happen and so it is safe to just
  // eval things in order. The PhiCleanerPass also makes sure that all
  // incoming blocks have the same order for each PHINode so we only
  // have to compute the index once.
  //
  // With that done we simply set an index in the state so that PHI
  // instructions know which argument to eval, set the pc, and continue.

  // XXX this lookup has to go ?
  KFunction *kf = state.stack().back().kf;
  unsigned entry = kf->basicBlockEntry[dst];
  state.pc() = &kf->instructions[entry];
  if (state.pc()->inst->getOpcode() == Instruction::PHI) {
    PHINode *first = static_cast<PHINode*>(state.pc()->inst);
    state.incomingBBIndex() = first->getBasicBlockIndex(src);
  }
}

/// Compute the true target of a function call, resolving LLVM aliases
/// and bitcasts.
Function* Executor::getTargetFunction(Value *calledVal, ExecutionState &state) {
  SmallPtrSet<const GlobalValue*, 3> Visited;

  Constant *c = dyn_cast<Constant>(calledVal);
  if (!c)
    return 0;

  while (true) {
    if (GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      if (!Visited.insert(gv).second)
        return 0;

      if (Function *f = dyn_cast<Function>(gv))
        return f;
      else if (GlobalAlias *ga = dyn_cast<GlobalAlias>(gv))
        c = ga->getAliasee();
      else
        return 0;
    } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
      if (ce->getOpcode()==Instruction::BitCast)
        c = ce->getOperand(0);
      else
        return 0;
    } else
      return 0;
  }
}

void Executor::executeInstruction(ExecutionState &state, KInstruction *ki) {
  Instruction *i = ki->inst;
  ++ki->frequency;
  switch (i->getOpcode()) {
    // Control flow
  case Instruction::Ret: {
    ReturnInst *ri = cast<ReturnInst>(i);
    KInstIterator kcaller = state.stack().back().caller;
    Instruction *caller = kcaller ? kcaller->inst : 0;
    bool isVoidReturn = (ri->getNumOperands() == 0);
    ref<Expr> result = ConstantExpr::alloc(0, Expr::Bool);

    if (!isVoidReturn) {
      result = eval(ki, 0, state).value;
    }

    if (state.stack().size() <= 1) {
      assert(!caller && "caller set on initial stack frame");
      if (state.threads.size() == 1) {
        // main exit
        terminateStateOnExit(state);
      } else {
        // Invoke pthread_exit()
        Function *f = kmodule->module->getFunction("pthread_exit");
        std::vector<ref<Expr>> arguments;
        arguments.push_back(result);
        executeCall(state, ki, f, arguments);
      }
    } else {
      state.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
        transferToBasicBlock(ii->getNormalDest(), caller->getParent(), state);
      } else {
        state.pc() = kcaller;
        ++state.pc();
      }

      if (!isVoidReturn) {
        Type *t = caller->getType();
        if (t != Type::getVoidTy(i->getContext())) {
          // may need to do coercion due to bitcasts
          Expr::Width from = result->getWidth();
          Expr::Width to = getWidthForLLVMType(t);

          if (from != to) {
            CallSite cs = (isa<InvokeInst>(caller) ? CallSite(cast<InvokeInst>(caller)) :
                           CallSite(cast<CallInst>(caller)));

            // XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
            bool isSExt = cs.hasRetAttr(llvm::Attribute::SExt);
#else
            bool isSExt = cs.paramHasAttr(0, llvm::Attribute::SExt);
#endif
            if (isSExt) {
              result = SExtExpr::create(result, to);
            } else {
              result = ZExtExpr::create(result, to);
            }
          }

          bindLocal(kcaller, state, result);
        }
      } else {
        // We check that the return value has no users instead of
        // checking the type, since C defaults to returning int for
        // undeclared functions.
        if (!caller->use_empty()) {
          terminateStateOnExecError(state, "return void when caller expected a result");
        }
      }
    }
    break;
  }
  case Instruction::Br: {
    TimerStatIncrementer timer(stats::brTime);
    BranchInst *bi = cast<BranchInst>(i);
    if (bi->isUnconditional()) {
      transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), state);
    } else {
      // FIXME: Find a way that we don't have this hidden dependency.
      assert(bi->getCondition() == bi->getOperand(0) &&
             "Wrong operand index!");
      ref<Expr> cond = eval(ki, 0, state).value;

      cond = optimizer.optimizeExpr(cond, false);

      if (isa<ConstantExpr>(cond))
        ++stats::concreteBr;
      else
        ++stats::symbolicBr;

      Executor::StatePair branches = fork(state, cond, false);

      // NOTE: There is a hidden dependency here, markBranchVisited
      // requires that we still be in the context of the branch
      // instruction (it reuses its statistic id). Should be cleaned
      // up with convenient instruction specific data.
      if (statsTracker && state.stack().back().kf->trackCoverage)
        statsTracker->markBranchVisited(branches.first, branches.second);

      if (branches.first)
        transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), *branches.first);
      if (branches.second)
        transferToBasicBlock(bi->getSuccessor(1), bi->getParent(), *branches.second);
    }
    break;
  }
  case Instruction::IndirectBr: {
    TimerStatIncrementer timer(stats::indirectBrTime);
    // implements indirect branch to a label within the current function
    const auto bi = cast<IndirectBrInst>(i);
    BasicBlock *parentbb = bi->getParent();
    auto address = eval(ki, 0, state).value;

    if (isa<ConstantExpr>(address))
      ++stats::concreteIndirectBr;
    else
      ++stats::symbolicIndirectBr;

    address = toUnique(state, address);

    // parse the IndirectBr instruction, order unique destination BasicBlocks deterministicly
    const auto numDestinations = bi->getNumDestinations();
    // bbindexMap map each feasible succeeding basicblock to a unique index
    std::map<const BasicBlock*, PathEntry::indirectbrIndex_t> bbindexMap;
    // BBindex2bb map each possible unique index (not necessarily feasible) to
    // the corresponding succeeding basicblock pointer (NULL if the successor is unfeasible)
    std::vector<const BasicBlock *> BBindex2bb;
    BBindex2bb.reserve(numDestinations);
    // index2exp map each possible unique index (not necessarily feasible) to
    // the corresponding constraint expression (NULL if the successor is unfeasible)
    std::vector<ref<Expr>> index2exp;
    index2exp.reserve(numDestinations);

    ref<Expr> errorCase = ConstantExpr::alloc(1, Expr::Bool);
    // FIXME: the 5 here seems like an arbitrary value
    SmallPtrSet<BasicBlock *, 5> destinations;
    // collect and check destinations from label list
    PathEntry::indirectbrIndex_t bbindex = 0;
    for (unsigned k = 0; k < numDestinations; ++k) {
      // filter duplicates
      const auto d = bi->getDestination(k);
      if (destinations.count(d)) continue;
      destinations.insert(d);

      // create address expression
      const auto PE = Expr::createPointer(reinterpret_cast<std::uint64_t>(d));
      ref<Expr> e = EqExpr::create(address, PE);

      // exclude address from errorCase
      errorCase = AndExpr::create(errorCase, Expr::createIsZero(e));

      // check feasibility
      bool result;
      bool success __attribute__ ((unused)) = solver->mayBeTrue(state, e, result);
      assert(success && "FIXME: Unhandled solver failure");
      if (result) {
        bbindexMap[d] = bbindex;
        BBindex2bb.push_back(d);
        index2exp.push_back(e);
      }
      else {
        BBindex2bb.push_back(NULL);
        index2exp.push_back(NULL);
      }
      ++bbindex;
    }
    assert((BBindex2bb.size() == bbindex) && (index2exp.size() == bbindex) && "bb or expr size mismatch");
    // check errorCase feasibility
    bool isErrorCaseFeasible;
    bool success __attribute__ ((unused)) = solver->mayBeTrue(state, errorCase, isErrorCaseFeasible);
    assert(success && "FIXME: Unhandled solver failure");

    // concrete address
    if (const auto CE = dyn_cast<ConstantExpr>(address.get())) {
      const auto bb_address = (BasicBlock *) CE->getZExtValue(Context::get().getPointerWidth());
      auto bbindex_find_it = bbindexMap.find(bb_address);
      assert((bbindex_find_it != bbindexMap.end()) &&
          "Can't find this concrete basicblock address, it may never exist or it is unfeasible");
      if (state.shouldRecord()) { // need to consider record/replay
        PathEntry pe;
        if (replayPath) {
          // replaying, check
          getNextPathEntry(state, pe);
          assert((pe.t == PathEntry::INDIRECTBR) &&
              "When replaying Instruction::IndirectBr concrete address, wrong PathEntry Type");
          assert((pe.body.indirectbrIndex == bbindex_find_it->second) &&
              "When replaying Instruction::IndirectBr, recorded index mismatch");
        }
        else {
          pe.t = PathEntry::INDIRECTBR;
          pe.body.indirectbrIndex = bbindex_find_it->second;
        }
        dumpStateAtBranch(state, pe, CE);
      }
      transferToBasicBlock(bb_address, parentbb, state);
      break;
    }

    // symbolic address
    std::vector<ExecutionState *> branches;
    if (state.shouldRecord() && replayPath) {
      PathEntry pe;
      getNextPathEntry(state, pe);
      assert((pe.t == PathEntry::INDIRECTBR) &&
          "When replaying Instruction::IndirectBr symbolic address, wrong PathEntry Type");
      PathEntry::indirectbrIndex_t index = pe.body.indirectbrIndex;
      assert((index < bbindex) && (BBindex2bb[index]) &&
          "When replaying Instruction::IndirectBr symbolic address, recorded index is invalid");
      branch(state, std::vector<ref<Expr>>{index2exp[index]}, branches);
      assert((branches.size() > 0) && (branches[0] != NULL));
      dumpStateAtBranch(state, pe, index2exp[index]);
      transferToBasicBlock(BBindex2bb[index], parentbb, *branches[0]);
    }
    else {
      std::vector<ref<Expr>> expressions;
      expressions.reserve(numDestinations + 1);
      for (auto e: index2exp) {
        if (!e.isNull())
          expressions.push_back(e);
      }
      if (isErrorCaseFeasible) {
        expressions.push_back(errorCase);
      }
      // fork every branch, include the ErrorCase
      branch(state, expressions, branches);

      // terminate error state
      if (isErrorCaseFeasible) {
        terminateStateOnExecError(*branches.back(), "indirectbr: illegal label address");
        branches.pop_back();
      }
      // dump PathEntry
      PathEntry pe;
      pe.t = PathEntry::INDIRECTBR;
      auto exp_it = expressions.begin();
      auto state_it = branches.begin();
      // iterate all feasible succeeding basicblocks by index, because we need to fill the index to PathEntry
      for (std::vector<const BasicBlock*>::size_type bbidx=0;
          (bbidx < BBindex2bb.size()) && (exp_it != expressions.end()); ++bbidx) {
        if (BBindex2bb[bbidx]) {
          pe.body.indirectbrIndex = bbidx;
          dumpStateAtBranch(**state_it, pe, *exp_it);
          ++exp_it;
          ++state_it;
        }
      }
      // branch states to resp. target blocks
      state_it = branches.begin();
      assert(bbindexMap.size() == branches.size());
      // iterate all succeeding basicblock again since `branches` is corresponding to feasible successors only.
      for (auto bbp: BBindex2bb) {
        if (bbp) { // BasicBlock * != NULL
          if (*state_it) { // ExecutionState * != NULL
            transferToBasicBlock(bbp, parentbb, **state_it);
          }
          ++state_it;
        }
      }
    }

    break;
  }
  case Instruction::Switch: {
    TimerStatIncrementer timer(stats::switchTime);
    SwitchInst *si = cast<SwitchInst>(i);
    ref<Expr> cond = eval(ki, 0, state).value;
    BasicBlock *parentbb = si->getParent();

    if (isa<ConstantExpr>(cond))
      ++stats::concreteSwitch;
    else
      ++stats::symbolicSwitch;
    //**** parse the switch instruction ****//

    // We use CaseIt->getSuccessorIndex as the unique case expression index
    // it maps default case, case_begin()..case_end() to
    // 0, 1 ... si->getNumCases()
    // Note that getNumCases() == getNumSuccesors() - 1
    // Note that even though the function name contains Successor,
    //   a distinct case value (expression) still has a unique SuccessorIndex.
    // Though, SwitchIndex::getSuccessor may return the same basicblock for
    //   distinct SuccessorIndex (multiple cases are mapped to the same
    //   successive BB).

    // map basicblocks (regardless of feasibility) to corresponding unique index
    std::map<const BasicBlock*, PathEntry::switchIndex_t> bbindexMap;
    // map unique index to corresponding basicblocks (regardless of feasibility), no NULL here
    std::vector<const BasicBlock *> BBindex2bb;

    // Iterate through all non-default cases and order them by expressions
    PathEntry::switchIndex_t bbindex = 0;
    for (auto i : si->cases()) {
      BasicBlock *caseSuccessor = i.getCaseSuccessor();
      std::pair<std::map<const BasicBlock*, PathEntry::switchIndex_t>::iterator, bool>
        insert_res = bbindexMap.insert(std::make_pair(
              caseSuccessor, bbindex));
      if (insert_res.second) { // first time see a BB
        ++bbindex;
        BBindex2bb.push_back(caseSuccessor);
      }
    }
    { // handle default destination separately
      BasicBlock *defaultDest = si->getDefaultDest();
      std::pair<std::map<const BasicBlock*, PathEntry::switchIndex_t>::iterator, bool>
        insert_res = bbindexMap.insert(std::make_pair(
              defaultDest, bbindex));
      if (insert_res.second) {
        ++bbindex;
        BBindex2bb.push_back(defaultDest);
      }
    }
    assert(BBindex2bb.size() == bbindex);

    if (state.shouldRecord() && replayPath) {
      ; // replaying, do not try to simplify cond
    }
    else {
      // concretize the condition Expr to a unique ConstExpr if possbile.
      // possible means the Expr provably only has a single value.
      cond = toUnique(state, cond);
    }

    // concrete switch condition
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      // Somewhat gross to create these all the time, but fine till we
      // switch to an internal rep.
      llvm::IntegerType *Ty = cast<IntegerType>(si->getCondition()->getType());
      ConstantInt *ci = ConstantInt::get(Ty, CE->getZExtValue());
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
      SwitchInst::CaseIt caseit = si->findCaseValue(ci);
      BasicBlock* succbb = caseit->getCaseSuccessor();
      PathEntry::switchIndex_t exp_idx = caseit->getSuccessorIndex();
#else
#error "Haven't tried SwitchInst::CaseIt in llvm < 5.0"
      BasicBlock *succbb = si->findCaseValue(ci).getCaseSuccessor();
#endif
      if (state.shouldRecord()) { // need to consider record/replay
        PathEntry pe;
        if (replayPath) { // replaying
          getNextPathEntry(state, pe);
          assert((pe.t == PathEntry::SWITCH_EXPIDX) && "When replaying Instruction::Switch concrete condition, wrong PathEntry Type");
          assert((pe.body.switchIndex == exp_idx) && "When replaying Instruction::Switch concrete condition, recorded index mismatch");
        }
        else { // not replaying
          pe.t = PathEntry::SWITCH_EXPIDX;
          pe.body.switchIndex = exp_idx;
        }
        dumpStateAtBranch(state, pe, CE);
      }
      transferToBasicBlock(succbb, parentbb, state);
    } else {
      // Handle possible different (symbolic) branch targets

      // We have the following assumptions:
      // - each case value is mutual exclusive to all other values, the
      //   default case represents value excluding all case values.
      // - order of case branches is based on the order of the expressions of
      //   the case values, still default is handled last

      // constraints of each case (default case included)
      std::vector<ref<Expr>> cases_constraints(si->getNumSuccessors(), ref<Expr>());
      // default constraint, initialized to true
      // it will be the conjunction of cond != each case value
      SwitchInst::CaseIt defaultit = si->case_default();
      ref<Expr> &defaultConstraint = cases_constraints[defaultit->getSuccessorIndex()];
      defaultConstraint = ConstantExpr::alloc(1, Expr::Bool);

      // note that SwitchInst::cases() will not iterate the default case
      for (auto caseit: si->cases()) {
        ref<Expr> value = evalConstant(caseit.getCaseValue());
        ref<Expr> match = EqExpr::create(cond, value);
        match = optimizer.optimizeExpr(match, false);
        cases_constraints[caseit.getSuccessorIndex()] = match;
        defaultConstraint = AndExpr::create(defaultConstraint, Expr::createIsZero(match));
      }

      for (auto i: cases_constraints) {
        assert(!i.isNull() && "cases_constraints uninitialized");
      }

      // constraints of all derived states, they may come from
      //   each case expression or the constraint of each possible succ BB
      std::vector<ref<Expr>> conditions;
      // used to store the forked state(s) returned by Executor::branch
      std::vector<ExecutionState*> branches;
      if (state.shouldRecord() && replayPath) {
        // replay
        PathEntry pe;
        getNextPathEntry(state, pe);
        if (pe.t == PathEntry::SWITCH_EXPIDX) {
          // replay a concrete switch decision, the cond should equal
          //   the corresponding case value
          PathEntry::switchIndex_t index = pe.body.switchIndex;
          assert(index < si->getNumSuccessors() && "invalid recorded EXPIDX");
          conditions.push_back(cases_constraints[index]);
          branch(state, conditions, branches);
          dumpStateAtBranch(state, pe, conditions[0]);
          transferToBasicBlock(si->getSuccessor(index), parentbb, *(branches[0]));
        }
        else if (pe.t == PathEntry::SWITCH_BBIDX) {
          // replay a symbolic switch decision, the cond could equal any
          //   case value (disjunction of equations)
          //   having the corresponding successor basicblock
          PathEntry::switchIndex_t index = pe.body.switchIndex;
          assert(index < BBindex2bb.size() &&  "Invalid recorded BBIDX");
          const BasicBlock *targetBB = BBindex2bb[index];
          // init this disjunction form to false
          ref<Expr> new_constraint = ConstantExpr::alloc(0, Expr::Bool);
          // "default" is also taken cared in this loop.
          // Even though a case value (case1) could have the same successor BB
          // as the default case, the generated condition is also valid:
          //   (cond == case1) || (cond != case1 && cond != case2 && ...)
          for (unsigned int succ_idx = 0;
              succ_idx < si->getNumSuccessors(); ++succ_idx) {
            SwitchInst::CaseIt caseit =
              SwitchInst::CaseIt::fromSuccessorIndex(si, succ_idx);
            if (caseit->getCaseSuccessor() == targetBB) {
              new_constraint = OrExpr::create(
                  new_constraint,
                  cases_constraints[caseit->getSuccessorIndex()]
              );
            }
          }
          conditions.push_back(new_constraint);
          branch(state, conditions, branches);
          dumpStateAtBranch(state, pe, conditions[0]);
          transferToBasicBlock(targetBB, parentbb, *(branches[0]));
        }
        else {
          klee_error("When replaying Instruction::Switch symbolic condition, wrong PathEntry type: %d", pe.t);
          terminateStateOnError(state, "Wrong PathEntry type", ReplayPath);
          break;
        }
      }
      else {
        // require symbolic execution, i.e. fork for each possible successive BB

        // tracking the constraints expression associated with each feasible basicblock
        std::map<const BasicBlock *, ref<Expr> > branchTargets;
        for (unsigned int succ_idx = 0;
            succ_idx < si->getNumSuccessors(); ++succ_idx) {
          SwitchInst::CaseIt caseit =
            SwitchInst::CaseIt::fromSuccessorIndex(si, succ_idx);
          bool result;
          ref<Expr> &match = cases_constraints[caseit->getSuccessorIndex()];
          bool success = solver->mayBeTrue(state, match, result);
          assert(success && "FIXME: Unhandled solver failure");
          (void) success;
          if (result) {
            const BasicBlock *caseSuccessor = caseit->getCaseSuccessor();
            // Handle the case that a basic block might be the target of multiple
            // switch cases.
            // Currently we generate a disjunctive form of all switch-case
            // values with the same target basic block. We spare us forking too
            // many times but we generate more complex condition expressions
            // TODO Add option to allow to choose between those behaviors
            std::pair<std::map<const BasicBlock *, ref<Expr> >::iterator, bool> insret =
              branchTargets.insert(std::make_pair(
                    caseSuccessor, ConstantExpr::alloc(0, Expr::Bool)));
            insret.first->second = OrExpr::create(insret.first->second, match);
          }
        }
        // call branch to fork on all possbile targets
        // note that iterator of branchTargets consists of:
        //   (target BB*, target constraint)
        for (auto t: branchTargets) {
          conditions.push_back(t.second);
        }
        branch(state, conditions, branches);
        PathEntry pe;
        pe.t = PathEntry::SWITCH_BBIDX;
        auto target = branchTargets.begin();
        auto forked_state = branches.begin();
        for (; (target != branchTargets.end()) && (forked_state != branches.end())
             ; target++, forked_state++) {
          if (*forked_state) { // Executor::branch returned valid state
            auto find_it = bbindexMap.find(target->first);
            assert(find_it != bbindexMap.end() && "invalid target BB*");
            pe.body.switchIndex = find_it->second;
            dumpStateAtBranch(**forked_state, pe, target->second);
            transferToBasicBlock(target->first, parentbb, **forked_state);
          }
        }
      }
    }
    break;
  }
  case Instruction::Unreachable:
    // Note that this is not necessarily an internal bug, llvm will
    // generate unreachable instructions in cases where it knows the
    // program will crash. So it is effectively a SEGV or internal
    // error.
    terminateStateOnExecError(state, "reached \"unreachable\" instruction");
    break;

  case Instruction::Invoke:
  case Instruction::Call: {
    TimerStatIncrementer time(stats::callTime);
    // Ignore debug intrinsic calls
    if (isa<DbgInfoIntrinsic>(i))
      break;
    CallSite cs(i);

    unsigned numArgs = cs.arg_size();
    Value *fp = cs.getCalledValue();

    if (llvm::InlineAsm *AI = dyn_cast<llvm::InlineAsm>(fp)) {
      if (AI->getAsmString() == "ptwrite $0") {
        llvm::Instruction *recI = dyn_cast<llvm::Instruction>(i->getOperand(0));
        assert(recI != nullptr);
        KInstruction *recKI = kmodule->getKInstruction(recI);
        assert(recKI != nullptr);

        tryLoadDataRecording(state, recKI);
        tryStoreDataRecording(state, recKI);
        break;
      }

      terminateStateOnExecError(state, "inline assembly is unsupported");
      break;
    }

    Function *f = getTargetFunction(fp, state);

    // evaluate arguments
    std::vector< ref<Expr> > arguments;
    arguments.reserve(numArgs);

    for (unsigned j=0; j<numArgs; ++j) {
      arguments.push_back(eval(ki, j+1, state).value);
      /*
      std::string BufferString;
      llvm::raw_string_ostream ExprWriter(BufferString);
      arguments[j].get()->dump();
      */
    }

    if (f) {
      const FunctionType *fType =
        dyn_cast<FunctionType>(cast<PointerType>(f->getType())->getElementType());
      const FunctionType *fpType =
        dyn_cast<FunctionType>(cast<PointerType>(fp->getType())->getElementType());

      // special case the call with a bitcast case
      if (fType != fpType) {
        assert(fType && fpType && "unable to get function type");

        // XXX check result coercion

        // XXX this really needs thought and validation
        unsigned i=0;
        for (std::vector< ref<Expr> >::iterator
               ai = arguments.begin(), ie = arguments.end();
             ai != ie; ++ai) {
          Expr::Width to, from = (*ai)->getWidth();

          if (i<fType->getNumParams()) {
            to = getWidthForLLVMType(fType->getParamType(i));

            if (from != to) {
              // XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
              bool isSExt = cs.paramHasAttr(i, llvm::Attribute::SExt);
#else
              bool isSExt = cs.paramHasAttr(i+1, llvm::Attribute::SExt);
#endif
              if (isSExt) {
                arguments[i] = SExtExpr::create(arguments[i], to);
              } else {
                arguments[i] = ZExtExpr::create(arguments[i], to);
              }
            }
          }

          i++;
        }
      }

      ++stats::concreteCall;

      executeCall(state, ki, f, arguments);
    } else {
      ref<Expr> v = eval(ki, 0, state).value;

      if (isa<ConstantExpr>(v))
        ++stats::concreteCall;
      else
        ++stats::symbolicCall;

      ExecutionState *free = &state;
      bool hasInvalid = false, first = true;

      /* XXX This is wasteful, no need to do a full evaluate since we
         have already got a value. But in the end the caches should
         handle it for us, albeit with some overhead. */
      do {
        v = optimizer.optimizeExpr(v, true);
        ref<ConstantExpr> value;
        bool success = solver->getValue(*free, v, value);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        StatePair res = fork(*free, EqExpr::create(v, value), true);
        if (res.first) {
          uint64_t addr = value->getZExtValue();
          if (legalFunctions.count(addr)) {
            f = (Function*) addr;

            // Don't give warning on unique resolution
            if (res.second || !first)
              klee_warning_once(reinterpret_cast<void*>(addr),
                                "resolved symbolic function pointer to: %s",
                                f->getName().data());

            executeCall(*res.first, ki, f, arguments);
          } else {
            if (!hasInvalid) {
              terminateStateOnExecError(state, "invalid function pointer");
              hasInvalid = true;
            }
          }
        }

        first = false;
        free = res.second;
      } while (free);
    }
    break;
  }
  case Instruction::PHI: {
    ref<Expr> result = eval(ki, state.incomingBBIndex(), state).value;
    bindLocal(ki, state, result);
    break;
  }

    // Special instructions
  case Instruction::Select: {
    // NOTE: It is not required that operands 1 and 2 be of scalar type.
    ref<Expr> cond = eval(ki, 0, state).value;

    if (isa<ConstantExpr>(cond))
      ++stats::concreteSelect;
    else
      ++stats::symbolicSelect;

    ref<Expr> tExpr = eval(ki, 1, state).value;
    ref<Expr> fExpr = eval(ki, 2, state).value;
    ref<Expr> result = SelectExpr::create(cond, tExpr, fExpr);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::VAArg:
    terminateStateOnExecError(state, "unexpected VAArg instruction");
    break;

    // Arithmetic / logical

  case Instruction::Add: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, AddExpr::create(left, right));
    break;
  }

  case Instruction::Sub: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, SubExpr::create(left, right));
    break;
  }

  case Instruction::Mul: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, MulExpr::create(left, right));
    break;
  }

  case Instruction::UDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = UDivExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::SDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SDivExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::URem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = URemExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::SRem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SRemExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::And: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AndExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Or: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = OrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Xor: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = XorExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Shl: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ShlExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::LShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = LShrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::AShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AShrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

    // Compare

  case Instruction::ICmp: {
    CmpInst *ci = cast<CmpInst>(i);
    ICmpInst *ii = cast<ICmpInst>(ci);

    switch(ii->getPredicate()) {
    case ICmpInst::ICMP_EQ: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = EqExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_NE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = NeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_UGT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UgtExpr::create(left, right);
      bindLocal(ki, state,result);
      break;
    }

    case ICmpInst::ICMP_UGE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UgeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UltExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UleExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SgtExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SgeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SltExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SleExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    default:
      terminateStateOnExecError(state, "invalid ICmp predicate");
    }
    break;
  }

    // Memory instructions...
  case Instruction::Alloca: {
    TimerStatIncrementer timer(stats::allocaTime);
    AllocaInst *ai = cast<AllocaInst>(i);
    unsigned elementSize =
      kmodule->targetData->getTypeStoreSize(ai->getAllocatedType());
    ref<Expr> size = Expr::createPointer(elementSize);
    if (ai->isArrayAllocation()) {
      ref<Expr> count = eval(ki, 0, state).value;
      count = Expr::createZExtToPointerWidth(count);
      size = MulExpr::create(size, count);
    }
    executeAlloc(state, size, true, ki);
    break;
  }

  case Instruction::Load: {
    ref<Expr> base = eval(ki, 0, state).value;
    executeMemoryOperation(state, false, base, 0, ki);
    break;
  }
  case Instruction::Store: {
    ref<Expr> base = eval(ki, 1, state).value;
    ref<Expr> value = eval(ki, 0, state).value;
    executeMemoryOperation(state, true, base, value, ki);
    break;
  }

  case Instruction::GetElementPtr: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);
    ref<Expr> base = eval(ki, 0, state).value;

    for (std::vector< std::pair<unsigned, uint64_t> >::iterator
           it = kgepi->indices.begin(), ie = kgepi->indices.end();
         it != ie; ++it) {
      uint64_t elementSize = it->second;
      ref<Expr> index = eval(ki, it->first, state).value;
      base = AddExpr::create(base,
                             MulExpr::create(Expr::createSExtToPointerWidth(index),
                                             Expr::createPointer(elementSize)));
    }
    if (kgepi->offset)
      base = AddExpr::create(base,
                             Expr::createPointer(kgepi->offset));
    bindLocal(ki, state, base);
    break;
  }

    // Conversion
  case Instruction::Trunc: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ExtractExpr::create(eval(ki, 0, state).value,
                                           0,
                                           getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ZExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ZExtExpr::create(eval(ki, 0, state).value,
                                        getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }
  case Instruction::SExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = SExtExpr::create(eval(ki, 0, state).value,
                                        getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::IntToPtr: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width pType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    bindLocal(ki, state, ZExtExpr::create(arg, pType));
    break;
  }
  case Instruction::PtrToInt: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width iType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    bindLocal(ki, state, ZExtExpr::create(arg, iType));
    break;
  }

  case Instruction::BitCast: {
    ref<Expr> result = eval(ki, 0, state).value;
    bindLocal(ki, state, result);
    break;
  }

    // Floating point instructions

  case Instruction::FAdd: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FAdd operation");

    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.add(APFloat(*fpWidthToSemantics(right->getWidth()),right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FSub: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FSub operation");
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.subtract(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FMul: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FMul operation");

    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.multiply(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FDiv: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FDiv operation");

    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.divide(APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FRem: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FRem operation");
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    Res.mod(
        APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()));
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FPTrunc: {
    FPTruncInst *fi = cast<FPTruncInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > arg->getWidth())
      return terminateStateOnExecError(state, "Unsupported FPTrunc operation");

    llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, state, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPExt: {
    FPExtInst *fi = cast<FPExtInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || arg->getWidth() > resultType)
      return terminateStateOnExecError(state, "Unsupported FPExt operation");
    llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, state, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPToUI: {
    FPToUIInst *fi = cast<FPToUIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToUI operation");

    llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
    uint64_t value = 0;
    bool isExact = true;
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
    auto valueRef = makeMutableArrayRef(value);
#else
    uint64_t *valueRef = &value;
#endif
    Arg.convertToInteger(valueRef, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::FPToSI: {
    FPToSIInst *fi = cast<FPToSIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToSI operation");
    llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());

    uint64_t value = 0;
    bool isExact = true;
#if LLVM_VERSION_CODE >= LLVM_VERSION(5, 0)
    auto valueRef = makeMutableArrayRef(value);
#else
    uint64_t *valueRef = &value;
#endif
    Arg.convertToInteger(valueRef, resultType, true,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::UIToFP: {
    UIToFPInst *fi = cast<UIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported UIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), false,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, state, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::SIToFP: {
    SIToFPInst *fi = cast<SIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported SIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), true,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, state, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::FCmp: {
    FCmpInst *fi = cast<FCmpInst>(i);
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FCmp operation");

    APFloat LHS(*fpWidthToSemantics(left->getWidth()),left->getAPValue());
    APFloat RHS(*fpWidthToSemantics(right->getWidth()),right->getAPValue());
    APFloat::cmpResult CmpRes = LHS.compare(RHS);

    bool Result = false;
    switch( fi->getPredicate() ) {
      // Predicates which only care about whether or not the operands are NaNs.
    case FCmpInst::FCMP_ORD:
      Result = (CmpRes != APFloat::cmpUnordered);
      break;

    case FCmpInst::FCMP_UNO:
      Result = (CmpRes == APFloat::cmpUnordered);
      break;

      // Ordered comparisons return false if either operand is NaN.  Unordered
      // comparisons return true if either operand is NaN.
    case FCmpInst::FCMP_UEQ:
      Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpEqual);
      break;
    case FCmpInst::FCMP_OEQ:
      Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpEqual);
      break;

    case FCmpInst::FCMP_UGT:
      Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpGreaterThan);
      break;
    case FCmpInst::FCMP_OGT:
      Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpGreaterThan);
      break;

    case FCmpInst::FCMP_UGE:
      Result = (CmpRes == APFloat::cmpUnordered || (CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual));
      break;
    case FCmpInst::FCMP_OGE:
      Result = (CmpRes != APFloat::cmpUnordered && (CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual));
      break;

    case FCmpInst::FCMP_ULT:
      Result = (CmpRes == APFloat::cmpUnordered || CmpRes == APFloat::cmpLessThan);
      break;
    case FCmpInst::FCMP_OLT:
      Result = (CmpRes != APFloat::cmpUnordered && CmpRes == APFloat::cmpLessThan);
      break;

    case FCmpInst::FCMP_ULE:
      Result = (CmpRes == APFloat::cmpUnordered || (CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual));
      break;
    case FCmpInst::FCMP_OLE:
      Result = (CmpRes != APFloat::cmpUnordered && (CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual));
      break;

    case FCmpInst::FCMP_UNE:
      Result = (CmpRes == APFloat::cmpUnordered || CmpRes != APFloat::cmpEqual);
      break;
    case FCmpInst::FCMP_ONE:
      Result = (CmpRes != APFloat::cmpUnordered && CmpRes != APFloat::cmpEqual);
      break;

    default:
      assert(0 && "Invalid FCMP predicate!");
      break;
    case FCmpInst::FCMP_FALSE:
      Result = false;
      break;
    case FCmpInst::FCMP_TRUE:
      Result = true;
      break;
    }

    bindLocal(ki, state, ConstantExpr::alloc(Result, Expr::Bool));
    break;
  }
  case Instruction::InsertValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;
    ref<Expr> val = eval(ki, 1, state).value;

    ref<Expr> l = NULL, r = NULL;
    unsigned lOffset = kgepi->offset*8, rOffset = kgepi->offset*8 + val->getWidth();

    if (lOffset > 0)
      l = ExtractExpr::create(agg, 0, lOffset);
    if (rOffset < agg->getWidth())
      r = ExtractExpr::create(agg, rOffset, agg->getWidth() - rOffset);

    ref<Expr> result;
    if (!l.isNull() && !r.isNull())
      result = ConcatExpr::create(r, ConcatExpr::create(val, l));
    else if (!l.isNull())
      result = ConcatExpr::create(val, l);
    else if (!r.isNull())
      result = ConcatExpr::create(r, val);
    else
      result = val;

    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ExtractValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;

    ref<Expr> result = ExtractExpr::create(agg, kgepi->offset*8, getWidthForLLVMType(i->getType()));

    bindLocal(ki, state, result);
    break;
  }
  case Instruction::Fence: {
    // Ignore for now
    break;
  }
  case Instruction::InsertElement: {
    InsertElementInst *iei = cast<InsertElementInst>(i);
    ref<Expr> vec = eval(ki, 0, state).value;
    ref<Expr> newElt = eval(ki, 1, state).value;
    ref<Expr> idx = eval(ki, 2, state).value;

    ConstantExpr *cIdx = dyn_cast<ConstantExpr>(idx);
    if (cIdx == NULL) {
      terminateStateOnError(
          state, "InsertElement, support for symbolic index not implemented",
          Unhandled);
      return;
    }
    uint64_t iIdx = cIdx->getZExtValue();
    const llvm::VectorType *vt = iei->getType();
    unsigned EltBits = getWidthForLLVMType(vt->getElementType());

    if (iIdx >= vt->getNumElements()) {
      // Out of bounds write
      terminateStateOnError(state, "Out of bounds write when inserting element",
                            BadVectorAccess);
      return;
    }

    const unsigned elementCount = vt->getNumElements();
    llvm::SmallVector<ref<Expr>, 8> elems;
    elems.reserve(elementCount);
    for (unsigned i = elementCount; i != 0; --i) {
      auto of = i - 1;
      unsigned bitOffset = EltBits * of;
      elems.push_back(
          of == iIdx ? newElt : ExtractExpr::create(vec, bitOffset, EltBits));
    }

    assert(Context::get().isLittleEndian() && "FIXME:Broken for big endian");
    ref<Expr> Result = ConcatExpr::createN(elementCount, elems.data());
    bindLocal(ki, state, Result);
    break;
  }
  case Instruction::ExtractElement: {
    ExtractElementInst *eei = cast<ExtractElementInst>(i);
    ref<Expr> vec = eval(ki, 0, state).value;
    ref<Expr> idx = eval(ki, 1, state).value;

    ConstantExpr *cIdx = dyn_cast<ConstantExpr>(idx);
    if (cIdx == NULL) {
      terminateStateOnError(
          state, "ExtractElement, support for symbolic index not implemented",
          Unhandled);
      return;
    }
    uint64_t iIdx = cIdx->getZExtValue();
    const llvm::VectorType *vt = eei->getVectorOperandType();
    unsigned EltBits = getWidthForLLVMType(vt->getElementType());

    if (iIdx >= vt->getNumElements()) {
      // Out of bounds read
      terminateStateOnError(state, "Out of bounds read when extracting element",
                            BadVectorAccess);
      return;
    }

    unsigned bitOffset = EltBits * iIdx;
    ref<Expr> Result = ExtractExpr::create(vec, bitOffset, EltBits);
    bindLocal(ki, state, Result);
    break;
  }
  case Instruction::ShuffleVector:
    // Should never happen due to Scalarizer pass removing ShuffleVector
    // instructions.
    terminateStateOnExecError(state, "Unexpected ShuffleVector instruction");
    break;
  case Instruction::AtomicRMW:
    terminateStateOnExecError(state, "Unexpected Atomic instruction, should be "
                                     "lowered by LowerAtomicInstructionPass");
    break;
  case Instruction::AtomicCmpXchg:
    terminateStateOnExecError(state,
                              "Unexpected AtomicCmpXchg instruction, should be "
                              "lowered by LowerAtomicInstructionPass");
    break;
  // Other instructions...
  // Unhandled
  default:
    terminateStateOnExecError(state, "illegal instruction");
    break;
  }
}

void Executor::updateStates(ExecutionState *current) {
  if (searcher) {
    searcher->update(current, addedStates, removedStates);
  }

  states.insert(addedStates.begin(), addedStates.end());
  addedStates.clear();

  for (std::vector<ExecutionState *>::iterator it = removedStates.begin(),
                                               ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    std::set<ExecutionState*>::iterator it2 = states.find(es);
    assert(it2!=states.end());
    states.erase(it2);
    std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 =
      seedMap.find(es);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    processTree->remove(es->ptreeNode);
    delete es;
  }
  removedStates.clear();
}

template <typename TypeIt>
void Executor::computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie) {
  ref<ConstantExpr> constantOffset =
    ConstantExpr::alloc(0, Context::get().getPointerWidth());
  uint64_t index = 1;
  for (TypeIt ii = ib; ii != ie; ++ii) {
    if (StructType *st = dyn_cast<StructType>(*ii)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(st);
      const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());
      uint64_t addend = sl->getElementOffset((unsigned) ci->getZExtValue());
      constantOffset = constantOffset->Add(ConstantExpr::alloc(addend,
                                                               Context::get().getPointerWidth()));
    } else if (const auto set = dyn_cast<SequentialType>(*ii)) {
      uint64_t elementSize =
        kmodule->targetData->getTypeStoreSize(set->getElementType());
      Value *operand = ii.getOperand();
      if (Constant *c = dyn_cast<Constant>(operand)) {
        ref<ConstantExpr> index =
          evalConstant(c)->SExt(Context::get().getPointerWidth());
        ref<ConstantExpr> addend =
          index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
#if LLVM_VERSION_CODE >= LLVM_VERSION(4, 0)
    } else if (const auto ptr = dyn_cast<PointerType>(*ii)) {
      auto elementSize =
        kmodule->targetData->getTypeStoreSize(ptr->getElementType());
      auto operand = ii.getOperand();
      if (auto c = dyn_cast<Constant>(operand)) {
        auto index = evalConstant(c)->SExt(Context::get().getPointerWidth());
        auto addend = index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
#endif
    } else
      assert("invalid type" && 0);
    index++;
  }
  kgepi->offset = constantOffset->getZExtValue();
}

void Executor::bindInstructionConstants(KInstruction *KI) {
  KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(KI);

  if (GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(KI->inst)) {
    computeOffsets(kgepi, gep_type_begin(gepi), gep_type_end(gepi));
  } else if (InsertValueInst *ivi = dyn_cast<InsertValueInst>(KI->inst)) {
    computeOffsets(kgepi, iv_type_begin(ivi), iv_type_end(ivi));
    assert(kgepi->indices.empty() && "InsertValue constant offset expected");
  } else if (ExtractValueInst *evi = dyn_cast<ExtractValueInst>(KI->inst)) {
    computeOffsets(kgepi, ev_type_begin(evi), ev_type_end(evi));
    assert(kgepi->indices.empty() && "ExtractValue constant offset expected");
  }
}

void Executor::bindModuleConstants() {
  for (auto &kfp : kmodule->functions) {
    KFunction *kf = kfp.get();
    for (unsigned i=0; i<kf->numInstructions; ++i)
      bindInstructionConstants(kf->instructions[i]);
  }

  kmodule->constantTable =
      std::unique_ptr<Cell[]>(new Cell[kmodule->constants.size()]);
  for (unsigned i=0; i<kmodule->constants.size(); ++i) {
    Cell &c = kmodule->constantTable[i];
    c.value = evalConstant(kmodule->constants[i]);
  }
}

void Executor::checkMemoryUsage() {
  if (!MaxMemory)
    return;
  if ((stats::instructions & 0xFFFF) == 0) {
    // We need to avoid calling GetTotalMallocUsage() often because it
    // is O(elts on freelist). This is really bad since we start
    // to pummel the freelist once we hit the memory cap.
    unsigned mbs = (util::GetTotalMallocUsage() >> 20) +
                   (memory->getUsedDeterministicSize() >> 20);

    if (mbs > MaxMemory) {
      if (mbs > MaxMemory + 100) {
        // just guess at how many to kill
        unsigned numStates = states.size();
        unsigned toKill = std::max(1U, numStates - numStates * MaxMemory / mbs);
        klee_warning("killing %d states (over memory cap)", toKill);
        std::vector<ExecutionState *> arr(states.begin(), states.end());
        for (unsigned i = 0, N = arr.size(); N && i < toKill; ++i, --N) {
          unsigned idx = rand() % N;
          // Make two pulls to try and not hit a state that
          // covered new code.
          if (arr[idx]->coveredNew)
            idx = rand() % N;

          std::swap(arr[idx], arr[N - 1]);
          terminateStateEarly(*arr[N - 1], "Memory limit exceeded.");
        }
      }
      atMemoryLimit = true;
    } else {
      atMemoryLimit = false;
    }
  }
}

void Executor::doDumpStates() {
  if (!DumpStatesOnHalt || states.empty())
    return;

  printInfo(llvm::errs());
  klee_message("halting execution, dumping remaining states");
  for (const auto &state : states)
    terminateStateEarly(*state, "Execution halting.");
  updateStates(nullptr);
}

void Executor::run(ExecutionState &initialState) {
  bindModuleConstants();

  // Delay init till now so that ticks don't accrue during optimization and such.
  //timers.reset();

  states.insert(&initialState);

  if (usingSeeds) {
    std::vector<SeedInfo> &v = seedMap[&initialState];

    for (std::vector<KTest*>::const_iterator it = usingSeeds->begin(),
           ie = usingSeeds->end(); it != ie; ++it)
      v.push_back(SeedInfo(*it));

    int lastNumSeeds = usingSeeds->size()+10;
    time::Point lastTime, startTime = lastTime = time::getWallTime();
    ExecutionState *lastState = 0;
    while (!seedMap.empty()) {
      if (haltExecution) {
        doDumpStates();
        return;
      }

      std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it =
        seedMap.upper_bound(lastState);
      if (it == seedMap.end())
        it = seedMap.begin();
      lastState = it->first;
      ExecutionState &state = *lastState;
      KInstruction *ki = state.pc();
      stepInstruction(state);

      executeInstruction(state, ki);
      //timers.invoke();
      if (::dumpStates) dumpStates();
      if (::dumpPTree) dumpPTree();
      updateStates(&state);

      if ((stats::instructions % 1000) == 0) {
        int numSeeds = 0, numStates = 0;
        for (std::map<ExecutionState*, std::vector<SeedInfo> >::iterator
               it = seedMap.begin(), ie = seedMap.end();
             it != ie; ++it) {
          numSeeds += it->second.size();
          numStates++;
        }
        const auto time = time::getWallTime();
        const time::Span seedTime(SeedTime);
        if (seedTime && time > startTime + seedTime) {
          klee_warning("seed time expired, %d seeds remain over %d states",
                       numSeeds, numStates);
          break;
        } else if (numSeeds<=lastNumSeeds-10 ||
                   time - lastTime >= time::seconds(10)) {
          lastTime = time;
          lastNumSeeds = numSeeds;
          klee_message("%d seeds remaining over: %d states",
                       numSeeds, numStates);
        }
      }
    }

    klee_message("seeding done (%d states remain)", (int) states.size());

    if (OnlySeed) {
      doDumpStates();
      return;
    }
  }

  searcher = constructUserSearcher(*this);

  std::vector<ExecutionState *> newStates(states.begin(), states.end());
  searcher->update(0, newStates, std::vector<ExecutionState *>());

  while (!states.empty() && !haltExecution) {
    if (info_requested) {
      info_requested = false;
      printInfo(llvm::errs());
    }
    ExecutionState &state = searcher->selectState();
    KInstruction *ki = state.pc();
    stepInstruction(state);

    executeInstruction(state, ki);
    // Each instruction takes one unit of time
    state.stateTime++;
    //timers.invoke();
    if (::dumpStates) dumpStates();
    if (::dumpPTree) dumpPTree();

    //checkMemoryUsage();

    updateStates(&state);
  }

  delete searcher;
  searcher = 0;

  doDumpStates();
}

std::string Executor::getAddressInfo(ExecutionState &state,
                                     ref<Expr> address) const{
  std::string Str;
  llvm::raw_string_ostream info(Str);
  info << "\taddress: " << address << "\n";
  // hack: I do not care detailed AddressInfo for now and I want to get rid of
  //   the expensive solver call bellow.
  // So just return the address itself here.
  return info.str();
  uint64_t example;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
    example = CE->getZExtValue();
  } else {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, address, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    example = value->getZExtValue();
    info << "\texample: " << example << "\n";
    std::pair< ref<Expr>, ref<Expr> > res = solver->getRange(state, address);
    info << "\trange: [" << res.first << ", " << res.second <<"]\n";
  }

  MemoryObject hack((unsigned) example);
  MemoryMap::iterator lower = state.addressSpace.objects.upper_bound(&hack);
  info << "\tnext: ";
  if (lower==state.addressSpace.objects.end()) {
    info << "none\n";
  } else {
    const MemoryObject *mo = lower->first;
    std::string alloc_info;
    mo->getAllocInfo(alloc_info);
    info << "object at " << mo->address
         << " of size " << mo->size << "\n"
         << "\t\t" << alloc_info << "\n";
  }
  if (lower!=state.addressSpace.objects.begin()) {
    --lower;
    info << "\tprev: ";
    if (lower==state.addressSpace.objects.end()) {
      info << "none\n";
    } else {
      const MemoryObject *mo = lower->first;
      std::string alloc_info;
      mo->getAllocInfo(alloc_info);
      info << "object at " << mo->address
           << " of size " << mo->size << "\n"
           << "\t\t" << alloc_info << "\n";
    }
  }

  return info.str();
}


void Executor::terminateState(ExecutionState &state) {
  if (replayKTest && state.replayPosition!=replayKTest->numObjects) {
    klee_warning_once(replayKTest,
                      "replay did not consume all objects in test input.");
  }

  interpreterHandler->incPathsExplored();

  std::vector<ExecutionState *>::iterator it =
      std::find(addedStates.begin(), addedStates.end(), &state);
  if (it==addedStates.end()) {
    state.pc() = state.prevPC();

    removedStates.push_back(&state);
  } else {
    // never reached searcher, just delete immediately
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it3 =
      seedMap.find(&state);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    addedStates.erase(it);
    processTree->remove(state.ptreeNode);
    delete &state;
  }
}

void Executor::terminateStateEarly(ExecutionState &state,
                                   const Twine &message) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, (message + "\n").str().c_str(),
                                        "early");
  terminateState(state);
}

void Executor::terminateStateOnExit(ExecutionState &state) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, 0, 0);
  terminateState(state);
}

const InstructionInfo & Executor::getLastNonKleeInternalInstruction(const ExecutionState &state,
    Instruction ** lastInstruction) {
  // unroll the stack of the applications state and find
  // the last instruction which is not inside a KLEE internal function
  ExecutionState::stack_ty::const_reverse_iterator it = state.stack().rbegin(),
      itE = state.stack().rend();

  // don't check beyond the outermost function (i.e. main())
  itE--;

  const InstructionInfo * ii = 0;
  if (kmodule->internalFunctions.count(it->kf->function) == 0){
    ii =  state.prevPC()->info;
    *lastInstruction = state.prevPC()->inst;
    //  Cannot return yet because even though
    //  it->function is not an internal function it might of
    //  been called from an internal function.
  }

  // Wind up the stack and check if we are in a KLEE internal function.
  // We visit the entire stack because we want to return a CallInstruction
  // that was not reached via any KLEE internal functions.
  for (;it != itE; ++it) {
    // check calling instruction and if it is contained in a KLEE internal function
    const Function * f = (*it->caller).inst->getParent()->getParent();
    if (kmodule->internalFunctions.count(f)){
      ii = 0;
      continue;
    }
    if (!ii){
      ii = (*it->caller).info;
      *lastInstruction = (*it->caller).inst;
    }
  }

  if (!ii) {
    // something went wrong, play safe and return the current instruction info
    *lastInstruction = state.prevPC()->inst;
    return *state.prevPC()->info;
  }
  return *ii;
}

bool Executor::shouldExitOn(enum TerminateReason termReason) {
  std::vector<TerminateReason>::iterator s = ExitOnErrorType.begin();
  std::vector<TerminateReason>::iterator e = ExitOnErrorType.end();

  for (; s != e; ++s)
    if (termReason == *s)
      return true;

  return false;
}

void Executor::terminateStateOnError(ExecutionState &state,
                                     const llvm::Twine &messaget,
                                     enum TerminateReason termReason,
                                     const char *suffix,
                                     const llvm::Twine &info) {
  std::string message = messaget.str();
  static std::set< std::pair<Instruction*, std::string> > emittedErrors;
  Instruction * lastInst;
  const InstructionInfo &ii = getLastNonKleeInternalInstruction(state, &lastInst);

  if (EmitAllErrors ||
      emittedErrors.insert(std::make_pair(lastInst, message)).second) {
    if (ii.file != "") {
      klee_message("ERROR: %s:%d: %s", ii.file.c_str(), ii.line, message.c_str());
    } else {
      klee_message("ERROR: (location information missing) %s", message.c_str());
    }
    if (!EmitAllErrors)
      klee_message("NOTE: now ignoring this error at this location");

    std::string MsgString;
    llvm::raw_string_ostream msg(MsgString);
    msg << "Error: " << message << "\n";
    if (ii.file != "") {
      msg << "File: " << ii.file << "\n";
      msg << "Line: " << ii.line << "\n";
      msg << "assembly.ll line: " << ii.assemblyLine << "\n";
    }

    printInfo(llvm::errs());

    std::string info_str = info.str();
    if (info_str != "")
      msg << "Info: \n" << info_str;

    std::string suffix_buf;
    if (!suffix) {
      suffix_buf = TerminateReasonNames[termReason];
      suffix_buf += ".err";
      suffix = suffix_buf.c_str();
    }

    interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);
  }

  terminateState(state);

  if (shouldExitOn(termReason))
    haltExecution = true;
}

// XXX shoot me
static const char *okExternalsList[] = { "printf",
                                         "fprintf",
                                         "puts",
                                         "getpid" };
static std::set<std::string> okExternals(okExternalsList,
                                         okExternalsList +
                                         (sizeof(okExternalsList)/sizeof(okExternalsList[0])));

void Executor::callExternalFunction(ExecutionState &state,
                                    KInstruction *target,
                                    Function *function,
                                    std::vector< ref<Expr> > &arguments) {
  // check if specialFunctionHandler wants it
  if (specialFunctionHandler->handle(state, function, target, arguments))
    return;

  if (ExternalCalls == ExternalCallPolicy::None
      && !okExternals.count(function->getName())) {
    klee_warning("Disallowed call to external function: %s\n",
               function->getName().str().c_str());
    terminateStateOnError(state, "external calls disallowed", User);
    return;
  }

  // normal external function handling path
  // allocate 128 bits for each argument (+return value) to support fp80's;
  // we could iterate through all the arguments first and determine the exact
  // size we need, but this is faster, and the memory usage isn't significant.
  uint64_t *args = (uint64_t*) alloca(2*sizeof(*args) * (arguments.size() + 1));
  memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
  unsigned wordIndex = 2;
  for (std::vector<ref<Expr> >::iterator ai = arguments.begin(),
       ae = arguments.end(); ai!=ae; ++ai) {
    if (ExternalCalls == ExternalCallPolicy::All) { // don't bother checking uniqueness
      *ai = optimizer.optimizeExpr(*ai, true);
      // NOTE: here comes concolic behaviour (symbolic --assignment-> concrete)
      ref<ConstantExpr> ce;
      bool success = solver->getValue(state, *ai, ce);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      ce->toMemory(&args[wordIndex]);
      ObjectPair op;
      // Checking to see if the argument is a pointer to something
      if (ce->getWidth() == Context::get().getPointerWidth() &&
          state.addressSpace.resolveOne(ce, op)) {
        op.second->flushToConcreteStore(solver, state);
      }
      wordIndex += (ce->getWidth()+63)/64;
    } else {
      ref<Expr> arg = toUnique(state, *ai);
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
        // XXX kick toMemory functions from here
        ce->toMemory(&args[wordIndex]);
        wordIndex += (ce->getWidth()+63)/64;
      } else {
        terminateStateOnExecError(state,
                                  "external call with symbolic argument: " +
                                  function->getName());
        return;
      }
    }
  }

  // Prepare external memory for invoking the function
  state.addressSpace.copyOutConcretes();
#ifndef WINDOWS
  // Update external errno state with local state value
  int *errno_addr = getErrnoLocation(state);
  ObjectPair result;
  bool resolved = state.addressSpace.resolveOne(
      ConstantExpr::create((uint64_t)errno_addr, Expr::Int64), result);
  if (!resolved)
    klee_error("Could not resolve memory object for errno");
  ref<Expr> errValueExpr = result.second->read(0, sizeof(*errno_addr) * 8);
  ConstantExpr *errnoValue = dyn_cast<ConstantExpr>(errValueExpr);
  if (!errnoValue) {
    terminateStateOnExecError(state,
                              "external call with errno value symbolic: " +
                                  function->getName());
    return;
  }

  externalDispatcher->setLastErrno(
      errnoValue->getZExtValue(sizeof(*errno_addr) * 8));
#endif

  if (!SuppressExternalWarnings) {

    std::string TmpStr;
    llvm::raw_string_ostream os(TmpStr);
    os << "calling external: " << function->getName().str() << "(";
    for (unsigned i=0; i<arguments.size(); i++) {
      os << arguments[i];
      if (i != arguments.size()-1)
        os << ", ";
    }
    os << ") at " << state.pc()->getSourceLocation();

    if (AllExternalWarnings)
      klee_warning("%s", os.str().c_str());
    else
      klee_warning_once(function, "%s", os.str().c_str());
  }

  bool success = externalDispatcher->executeCall(function, target->inst, args);
  if (!success) {
    terminateStateOnError(state, "failed external call: " + function->getName(),
                          External);
    return;
  }

  if (!state.addressSpace.copyInConcretes()) {
    terminateStateOnError(state, "external modified read-only object",
                          External);
    return;
  }

#ifndef WINDOWS
  // Update errno memory object with the errno value from the call
  int error = externalDispatcher->getLastErrno();
  state.addressSpace.copyInConcrete(result.first, result.second,
                                    (uint64_t)&error);
#endif

  Type *resultType = target->inst->getType();
  if (resultType != Type::getVoidTy(function->getContext())) {
    ref<Expr> e = ConstantExpr::fromMemory((void*) args,
                                           getWidthForLLVMType(resultType));
    bindLocal(target, state, e);
  }
}

/***/

ref<Expr> Executor::replaceReadWithSymbolic(ExecutionState &state,
                                            ref<Expr> e) {
  unsigned n = interpreterOpts.MakeConcreteSymbolic;
  if (!n || replayKTest || replayPath)
    return e;

  // right now, we don't replace symbolics (is there any reason to?)
  if (!isa<ConstantExpr>(e))
    return e;

  if (n != 1 && random() % n)
    return e;

  // create a new fresh location, assert it is equal to concrete value in e
  // and return it.

  static unsigned id;
  const Array *array =
      arrayCache.CreateArray("rrws_arr" + llvm::utostr(++id),
                             Expr::getMinBytesForWidth(e->getWidth()));
  ref<Expr> res = Expr::createTempRead(array, e->getWidth());
  ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
  llvm::errs() << "Making symbolic: " << eq << "\n";
  state.addConstraint(eq);
  return res;
}

ObjectState *Executor::bindObjectInState(ExecutionState &state,
                                         const MemoryObject *mo,
                                         bool isLocal,
                                         const Array *array) {
  ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
  state.addressSpace.bindObject(mo, os);

  // Its possible that multiple bindings of the same mo in the state
  // will put multiple copies on this list, but it doesn't really
  // matter because all we use this list for is to unbind the object
  // on function return.
  if (isLocal)
    state.stack().back().allocas.push_back(mo);

  return os;
}

void Executor::executeAlloc(ExecutionState &state,
                            ref<Expr> size,
                            bool isLocal,
                            KInstruction *target,
                            bool zeroMemory,
                            const ObjectState *reallocFrom,
                            size_t allocationAlignment) {
  TimerStatIncrementer timer(stats::executeAllocTime);
  size = toUnique(state, size);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
    const llvm::Value *allocSite = state.prevPC()->inst;
    if (allocationAlignment == 0) {
      allocationAlignment = getAllocationAlignment(allocSite);
    }
    MemoryObject *mo =
        memory->allocate(CE->getZExtValue(), isLocal, /*isGlobal=*/false,
                         allocSite, allocationAlignment,
                         /*isInPOSIX*/(state.isInPOSIX() || !state.isInUserMain));
    if (!mo) {
      bindLocal(target, state,
                ConstantExpr::alloc(0, Context::get().getPointerWidth()));
    } else {
      ObjectState *os = bindObjectInState(state, mo, isLocal);
      if (zeroMemory) {
        os->initializeToZero();
      } else {
        os->initializeToRandom();
      }
      bindLocal(target, state, mo->getBaseExpr());

      if (reallocFrom) {
        unsigned count = std::min(reallocFrom->size, os->size);
        for (unsigned i=0; i<count; i++)
          os->write(i, reallocFrom->read8(i), Expr::FLAG_INSTRUCTION_ROOT, target);
        state.addressSpace.unbindObject(reallocFrom->getObject());
      }
    }
  } else if (!AllowSymbolicMalloc) {
    // we need to stop replaying and dump the symbolic "size" so that ptwrite
    // can be instrumented and help us concretize this in the next iteration.
    std::string sbuf;
    llvm::raw_string_ostream sos(sbuf);
    state.dumpStack(sos);
    std::vector<ref<Expr>> symbolicEvals;
    symbolicEvals.push_back(size);
    klee_message("Calling malloc with symbolic size:\n%s\n", sos.str().c_str());
    std::string file_path = interpreterHandler->getOutputFilename("symbolicMalloc.kquery");
    debugDumpConstraintsEval(state, state.constraints, symbolicEvals, file_path.c_str());
    terminateStateOnError(state, "calling malloc with symbolic size", Abort);
  } else {
    // XXX For now we just pick a size. Ideally we would support
    // symbolic sizes fully but even if we don't it would be better to
    // "smartly" pick a value, for example we could fork and pick the
    // min and max values and perhaps some intermediate (reasonable
    // value).
    //
    // It would also be nice to recognize the case when size has
    // exactly two values and just fork (but we need to get rid of
    // return argument first). This shows up in pcre when llvm
    // collapses the size expression with a select.

    size = optimizer.optimizeExpr(size, true);

    ref<ConstantExpr> example;
    bool success = solver->getValue(state, size, example);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;

    // Try and start with a small example.
    Expr::Width W = example->getWidth();
    while (example->Ugt(ConstantExpr::alloc(128, W))->isTrue()) {
      ref<ConstantExpr> try_smaller = example->LShr(ConstantExpr::alloc(1, W));
      bool res;
      bool success = solver->mayBeTrue(state, EqExpr::create(try_smaller, size), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (!res)
        break;
      example = try_smaller;
    }

    StatePair fixedSize = fork(state, EqExpr::create(example, size), true);

    if (fixedSize.second) {
      // Check for exactly two values
      ref<ConstantExpr> example2;
      bool success = solver->getValue(*fixedSize.second, size, example2);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      bool res;
      success = solver->mustBeTrue(*fixedSize.second,
                                   EqExpr::create(example2, size),
                                   res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        executeAlloc(*fixedSize.second, example2, isLocal,
                     target, zeroMemory, reallocFrom);
      } else {
        // See if a *really* big value is possible. If so assume
        // malloc will fail for it, so lets fork and return 0.
        StatePair hugeSize =
          fork(*fixedSize.second,
               UltExpr::create(ConstantExpr::alloc(1U<<31, W), size),
               true);
        if (hugeSize.first) {
          klee_message("NOTE: found huge malloc, returning 0");
          bindLocal(target, *hugeSize.first,
                    ConstantExpr::alloc(0, Context::get().getPointerWidth()));
        }

        if (hugeSize.second) {

          std::string Str;
          llvm::raw_string_ostream info(Str);
          ExprPPrinter::printOne(info, "  size expr", size);
          info << "  concretization : " << example << "\n";
          info << "  unbound example: " << example2 << "\n";
          terminateStateOnError(*hugeSize.second, "concretized symbolic size",
                                Model, NULL, info.str());
        }
      }
    }

    if (fixedSize.first) // can be zero when fork fails
      executeAlloc(*fixedSize.first, example, isLocal,
                   target, zeroMemory, reallocFrom);
  }
}

void Executor::executeFree(ExecutionState &state,
                           ref<Expr> address,
                           KInstruction *target) {
  address = optimizer.optimizeExpr(address, true);
  StatePair zeroPointer = fork(state, Expr::createIsZero(address), true);
  if (zeroPointer.first) {
    if (target)
      bindLocal(target, *zeroPointer.first, Expr::createPointer(0));
  }
  if (zeroPointer.second) { // address != 0
    //typedef std::vector< std::pair<
    //          std::pair<const MemoryObject*, const ObjectState*>,
    //          ExecutionState*>
    //        > ExactResolutionList;
    ExactResolutionList rl;
    resolveExact(*zeroPointer.second, address, rl, "free");

    for (Executor::ExactResolutionList::iterator it = rl.begin(),
           ie = rl.end(); it != ie; ++it) {
      const MemoryObject *mo = it->first.first;
      if (mo->isLocal) {
        terminateStateOnError(*it->second, "free of alloca", Free, NULL,
                              getAddressInfo(*it->second, address));
      } else if (mo->isGlobal) {
        terminateStateOnError(*it->second, "free of global", Free, NULL,
                              getAddressInfo(*it->second, address));
      } else {
        it->second->addressSpace.unbindObject(mo);
        if (target)
          bindLocal(target, *it->second, Expr::createPointer(0));
      }
    }
  }
}

void Executor::executeMallocUsableSize(ExecutionState &state,
                           ref<Expr> address,
                           KInstruction *target) {
  address = optimizer.optimizeExpr(address, true);
  StatePair zeroPointer = fork(state, Expr::createIsZero(address), true);
  if (zeroPointer.first) {
    terminateStateOnError(state, "call usable_size on zero address", Unhandled);
  }
  if (zeroPointer.second) { // address != 0
    //typedef std::vector< std::pair<
    //          std::pair<const MemoryObject*, const ObjectState*>,
    //          ExecutionState*>
    //        > ExactResolutionList;
    ExactResolutionList rl;
    resolveExact(*zeroPointer.second, address, rl, "usable_size");
    if (rl.size() != 1) {
    terminateStateOnError(state, "wrong number of resolved obj", Unhandled, NULL, getAddressInfo(state, address));
    }
    Executor::ExactResolutionList::iterator it = rl.begin();
    const MemoryObject *mo = it->first.first;
    if (mo->isLocal) {
      terminateStateOnError(*it->second, "usable_size of alloca", Free, NULL,
          getAddressInfo(*it->second, address));
    } else if (mo->isGlobal) {
      terminateStateOnError(*it->second, "usable_size of global", Free, NULL,
          getAddressInfo(*it->second, address));
    } else {
      bindLocal(target, state, ConstantExpr::create(mo->size, Expr::Int64));
      return;
    }
  }
  bindLocal(target, state, ConstantExpr::create(0, Expr::Int64));
}

void Executor::resolveExact(ExecutionState &state,
                            ref<Expr> p,
                            ExactResolutionList &results,
                            const std::string &name) {
  p = optimizer.optimizeExpr(p, true);
  // XXX we may want to be capping this?
  ResolutionList rl;
  state.addressSpace.resolve(state, solver, p, rl);

  ExecutionState *unbound = &state;
  for (ResolutionList::iterator it = rl.begin(), ie = rl.end();
       it != ie; ++it) {
    ref<Expr> inBounds = EqExpr::create(p, it->first->getBaseExpr());

    StatePair branches = fork(*unbound, inBounds, true);

    if (branches.first)
      results.push_back(std::make_pair(*it, branches.first));

    unbound = branches.second;
    if (!unbound) // Fork failure
      break;
  }

  if (unbound) {
    terminateStateOnError(*unbound, "memory error: invalid pointer: " + name,
                          Ptr, NULL, getAddressInfo(*unbound, p));
  }
}

void Executor::executeMemoryOperation(ExecutionState &state,
                                      bool isWrite,
                                      ref<Expr> address,
                                      ref<Expr> value /* undef if read */,
                                      KInstruction *target /* undef if write */) {
  TimerStatIncrementer timerS1(stats::executeMemopTimeS1);
  Expr::Width type = (isWrite ? value->getWidth() :
                     getWidthForLLVMType(target->inst->getType()));
  unsigned bytes = Expr::getMinBytesForWidth(type);

  if (SimplifySymIndices) {
    if (!isa<ConstantExpr>(address))
      address = state.constraints.simplifyExpr(address);
    if (isWrite && !isa<ConstantExpr>(value))
      value = state.constraints.simplifyExpr(value);
  }

  address = optimizer.optimizeExpr(address, true);

  // fast path: single in-bounds resolution
  ObjectPair op;
  bool success;
  solver->setTimeout(coreSolverTimeout);
  if (!state.addressSpace.resolveOne(state, solver, address, op, success)) {
    address = toConstant(state, address, "resolveOne failure");
    success = state.addressSpace.resolveOne(cast<ConstantExpr>(address), op);
  }
  solver->setTimeout(time::Span());
  timerS1.check();
  if (success) {
    TimerStatIncrementer timerOOBCheck(stats::executeMemopOOBCheck);
    const MemoryObject *mo = op.first;

    if (MaxSymArraySize && mo->size >= MaxSymArraySize) {
      address = toConstant(state, address, "max-sym-array-size");
    }

    ref<Expr> offset = mo->getOffsetExpr(address);

    bool inBounds;
    // I used to say:
    // *********************************************
    // It is totally OK to disable solver call here.
    // Since we only symbolic execute instructions before the crash, thus no
    //   out of bound memory access should happen.
    //   Otherwise, the program is supposed to crash earlier.
    // *********************************************
    // But it is no longer true because out of bound access does not necessarily
    // cause segfault immediately
    if (DoOutofBoundaryCheck) {
      ref<Expr> check = mo->getBoundsCheckOffset(offset, bytes);
      check = optimizer.optimizeExpr(check, true);

      solver->setTimeout(coreSolverTimeout);
      bool success = solver->mustBeTrue(state, check, inBounds);
      solver->setTimeout(time::Span());
      if (!success) {
        state.pc() = state.prevPC();
        terminateStateEarly(state, "Query timed out (bounds check).");
        return;
      }
    }
    else {
      inBounds = true;
    }
    timerOOBCheck.check();

    if (inBounds) {
      TimerStatIncrementer timerInBounds(stats::executeMemopTimeInBounds);
      const ObjectState *os = op.second;
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(state, "memory error: object read only",
                                ReadOnly);
        } else {
          ObjectState *wos = state.addressSpace.getWriteable(mo, os);
          wos->write(offset, value, Expr::FLAG_INSTRUCTION_ROOT, target);
        }
      } else {
        ref<Expr> result = os->read(offset, type);

        if (interpreterOpts.MakeConcreteSymbolic)
          result = replaceReadWithSymbolic(state, result);

        bindLocal(target, state, result);
      }

      return;
    }
  }
  TimerStatIncrementer timerErrHandl(stats::executeMemopTimeErrHandl);
  klee_warning("Out of bound memory access, forking in Memory Model, address kinst: %s", address->getKInstUniqueID().c_str());

  // we are on an error path (no resolution, multiple resolution, one
  // resolution with out of bounds)

  address = optimizer.optimizeExpr(address, true);
  ResolutionList rl;
  solver->setTimeout(coreSolverTimeout);
  bool incomplete = state.addressSpace.resolve(state, solver, address, rl,
                                               0, coreSolverTimeout);
  solver->setTimeout(time::Span());

  // XXX there is some query wasteage here. who cares?
  ExecutionState *unbound = &state;

  for (ResolutionList::iterator i = rl.begin(), ie = rl.end(); i != ie; ++i) {
    const MemoryObject *mo = i->first;
    const ObjectState *os = i->second;
    ref<Expr> inBounds = mo->getBoundsCheckPointer(address, bytes);

    // TODO: I feel like the fork here is unnecessary.
    // Considering the symbolic address can already be mapped to multiple
    //   memory objects, it of course will be "out of bound" when you only consider
    //   one possible mapping. (unless the objects overlap or
    //   the addressSpace.resolve is not accurate.
    // I am considering only forking one more ExecutionState per iteration,
    //   which represents the in bound case.
    StatePair branches = fork(*unbound, inBounds, true);
    ExecutionState *bound = branches.first;

    // bound can be 0 on failure or overlapped
    if (bound) {
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(*bound, "memory error: object read only",
                                ReadOnly);
        } else {
          ObjectState *wos = bound->addressSpace.getWriteable(mo, os);
          wos->write(mo->getOffsetExpr(address), value,
                    Expr::FLAG_INSTRUCTION_ROOT, target);
        }
      } else {
        ref<Expr> result = os->read(mo->getOffsetExpr(address), type);
        bindLocal(target, *bound, result);
      }
    }

    unbound = branches.second;
    if (!unbound)
      break;
  }

  // XXX should we distinguish out of bounds and overlapped cases?
  if (unbound) {
    if (incomplete) {
      terminateStateEarly(*unbound, "Query timed out (resolve).");
    } else {
      terminateStateOnError(*unbound, "memory error: out of bound pointer", Ptr,
                            NULL, getAddressInfo(*unbound, address));
    }
  }
}

void Executor::executeMakeSymbolic(ExecutionState &state,
                                   const MemoryObject *mo,
                                   const std::string &name) {
  // Create a new object state for the memory object (instead of a copy).
  if (!replayKTest) {
    // Find a unique name for this array.  First try the original name,
    // or if that fails try adding a unique identifier.
    unsigned id = 0;
    std::string uniqueName = name;
    while (!state.arrayNames.insert(uniqueName).second) {
      uniqueName = name + "_" + llvm::utostr(++id);
    }
    const Array *array = arrayCache.CreateArray(uniqueName, mo->size);
    bindObjectInState(state, mo, false, array);
    state.addSymbolic(mo, array);

    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it =
      seedMap.find(&state);
    if (it!=seedMap.end()) { // In seed mode we need to add this as a
                             // binding.
      for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
             siie = it->second.end(); siit != siie; ++siit) {
        SeedInfo &si = *siit;
        KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

        if (!obj) {
          if (ZeroSeedExtension) {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values = std::vector<unsigned char>(mo->size, '\0');
          } else if (!AllowSeedExtension) {
            terminateStateOnError(state, "ran out of inputs during seeding",
                                  User);
            break;
          }
        } else {
          if (obj->numBytes != mo->size &&
              ((!(AllowSeedExtension || ZeroSeedExtension)
                && obj->numBytes < mo->size) ||
               (!AllowSeedTruncation && obj->numBytes > mo->size))) {
        	    std::stringstream msg;
        	    msg << "replace size mismatch: "
        		<< mo->name << "[" << mo->size << "]"
        		<< " vs " << obj->name << "[" << obj->numBytes << "]"
        		<< " in test\n";

            terminateStateOnError(state, msg.str(), User);
            break;
          } else {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values.insert(values.begin(), obj->bytes,
                          obj->bytes + std::min(obj->numBytes, mo->size));
            if (ZeroSeedExtension) {
              for (unsigned i=obj->numBytes; i<mo->size; ++i)
                values.push_back('\0');
            }
          }
        }
      }
    }
  } else {
    ObjectState *os = bindObjectInState(state, mo, false);
    if (state.replayPosition >= replayKTest->numObjects) {
      terminateStateOnError(state, "replay count mismatch", User);
    } else {
      KTestObject *obj = &replayKTest->objects[state.replayPosition++];
      if (obj->numBytes != mo->size) {
        terminateStateOnError(state, "replay size mismatch", User);
      } else {
        for (unsigned i=0; i<mo->size; i++)
          os->write8(i, obj->bytes[i], Expr::FLAG_INITIALIZATION, nullptr);
      }
    }
  }
}

/***/

void Executor::runFunctionAsMain(Function *f,
				 int argc,
				 char **argv,
				 char **envp) {
  std::vector<ref<Expr> > arguments;

  // force deterministic initialization of memory objects
  srand(1);
  srandom(1);

  MemoryObject *argvMO = 0;

  // In order to make uclibc happy and be closer to what the system is
  // doing we lay out the environments at the end of the argv array
  // (both are terminated by a null). There is also a final terminating
  // null that uclibc seems to expect, possibly the ELF header?

  int envc;
  for (envc=0; envp[envc]; ++envc) ;

  unsigned NumPtrBytes = Context::get().getPointerWidth() / 8;
  KFunction *kf = kmodule->functionMap[f];
  assert(kf);
  Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
  if (ai!=ae) {
    arguments.push_back(ConstantExpr::alloc(argc, Expr::Int32));
    if (++ai!=ae) {
      Instruction *first = &*(f->begin()->begin());
      argvMO =
          memory->allocate((argc + 1 + envc + 1 + 1) * NumPtrBytes,
                           /*isLocal=*/false, /*isGlobal=*/true,
                           /*allocSite=*/first, /*alignment=*/8,
                           /*isInPOSIX*/false);

      if (!argvMO)
        klee_error("Could not allocate memory for function arguments");

      arguments.push_back(argvMO->getBaseExpr());

      if (++ai!=ae) {
        uint64_t envp_start = argvMO->address + (argc+1)*NumPtrBytes;
        arguments.push_back(Expr::createPointer(envp_start));

        if (++ai!=ae)
          klee_error("invalid main function (expect 0-3 arguments)");
      }
    }
  }

  ExecutionState *state = new ExecutionState(kmodule->functionMap[f]);

  if (pathWriter)
    state->pathOS = pathWriter->open();
  if (pathDataRecWriter)
    state->pathDataRecOS = pathDataRecWriter->open();
  if (symPathWriter)
    state->symPathOS = symPathWriter->open();
  if (stackPathWriter)
    state->stackPathOS = stackPathWriter->open();
  if (consPathWriter)
    state->consPathOS = consPathWriter->open();
  if (statsPathWriter)
    state->statsPathOS = statsPathWriter->open();

  if (statsTracker)
    statsTracker->framePushed(*state, 0);

  assert(arguments.size() == f->arg_size() && "wrong number of arguments");
  for (unsigned i = 0, e = f->arg_size(); i != e; ++i)
    bindArgument(kf, i, *state, arguments[i]);

  if (argvMO) {
    ObjectState *argvOS = bindObjectInState(*state, argvMO, false);

    for (int i=0; i<argc+1+envc+1+1; i++) {
      if (i==argc || i>=argc+1+envc) {
        // Write NULL pointer
        argvOS->write(i * NumPtrBytes, Expr::createPointer(0), Expr::FLAG_INITIALIZATION, nullptr);
      } else {
        char *s = i<argc ? argv[i] : envp[i-(argc+1)];
        int j, len = strlen(s);

        MemoryObject *arg =
            memory->allocate(len + 1, /*isLocal=*/false, /*isGlobal=*/true,
                             /*allocSite=*/state->pc()->inst, /*alignment=*/8,
                             /*isInPOSIX*/false);
        if (!arg)
          klee_error("Could not allocate memory for function arguments");
        ObjectState *os = bindObjectInState(*state, arg, false);
        for (j=0; j<len+1; j++)
          os->write8(j, s[j], Expr::FLAG_INITIALIZATION, nullptr);

        // Write pointer to newly allocated and initialised argv/envp c-string
        argvOS->write(i * NumPtrBytes, arg->getBaseExpr(), Expr::FLAG_INITIALIZATION, nullptr);
      }
    }
  }

  initializeGlobals(*state);

  processTree = std::make_unique<PTree>(state);
  run(*state);
  processTree = nullptr;

  // hack to clear memory objects
  delete memory;
  memory = new MemoryManager(NULL);

  globalObjects.clear();
  globalAddresses.clear();

  if (statsTracker)
    statsTracker->done();
  kmodule->saveCntToMDNode();
}

unsigned Executor::getPathStreamID(const ExecutionState &state) {
  assert(pathWriter);
  return state.pathOS.getID();
}

unsigned Executor::getPathDataRecStreamID(const ExecutionState &state) {
  assert(pathDataRecWriter);
  return state.pathDataRecOS.getID();
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state) {
  assert(symPathWriter);
  return state.symPathOS.getID();
}

unsigned Executor::getStackPathStreamID(const ExecutionState &state) {
  assert(stackPathWriter);
  return state.stackPathOS.getID();
}

unsigned Executor::getConsPathStreamID(const ExecutionState &state) {
  assert(consPathWriter);
  return state.consPathOS.getID();
}

unsigned Executor::getStatsPathStreamID(const ExecutionState &state) {
  assert(statsPathWriter);
  return state.statsPathOS.getID();
}

void Executor::getConstraintLog(const ExecutionState &state, std::string &res,
                                Interpreter::LogType logFormat) {

  switch (logFormat) {
  case STP: {
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    char *log = solver->getConstraintLog(query);
    res = std::string(log);
    free(log);
  } break;

  case KQUERY: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    
    const ref<Expr>* evalExprsBegin = 0;
    const ref<Expr>* evalExprsEnd = 0;

    const Array* const *evalArraysBegin = 0;
    const Array* const *evalArraysEnd = 0;
    
    std::vector<const Array*> objects;
    for (unsigned i = 0; i != state.symbolics.size(); ++i)
      objects.push_back(state.symbolics[i].second);

    if (!objects.empty()) {
        evalArraysBegin = &(objects[0]);
        evalArraysEnd = &(objects[0]) + objects.size();
    }
    
    ExprPPrinter::printQuery(info, state.constraints.getAllConstraints(), ConstantExpr::alloc(false, Expr::Bool),
                        evalExprsBegin, evalExprsEnd,
                        evalArraysBegin, evalArraysEnd);
    res = info.str();
  } break;

  case SMTLIB2: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprSMTLIBPrinter printer;
    printer.setOutput(info);
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    printer.setQuery(query);
    printer.generateOutput();
    res = info.str();
  } break;

  default:
    klee_warning("Executor::getConstraintLog() : Log format not supported!");
  }
}

bool Executor::getSymbolicSolution(const ExecutionState &state,
                                   std::vector<
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res) {
  solver->setTimeout(coreSolverTimeout);

  ExecutionState tmp(state);

  // Go through each byte in every test case and attempt to restrict
  // it to the constraints contained in cexPreferences.  (Note:
  // usually this means trying to make it an ASCII character (0-127)
  // and therefore human readable. It is also possible to customize
  // the preferred constraints.  See test/Features/PreferCex.c for
  // an example) While this process can be very expensive, it can
  // also make understanding individual test cases much easier.
  for (unsigned i = 0; i != state.symbolics.size(); ++i) {
    const MemoryObject *mo = state.symbolics[i].first;
    std::vector< ref<Expr> >::const_iterator pi =
      mo->cexPreferences.begin(), pie = mo->cexPreferences.end();
    for (; pi != pie; ++pi) {
      bool mustBeTrue;
      // Attempt to bound byte to constraints held in cexPreferences
      bool success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi),
					mustBeTrue);
      // If it isn't possible to constrain this particular byte in the desired
      // way (normally this would mean that the byte can't be constrained to
      // be between 0 and 127 without making the entire constraint list UNSAT)
      // then just continue on to the next byte.
      if (!success) break;
      // If the particular constraint operated on in this iteration through
      // the loop isn't implied then add it to the list of constraints.
      if (!mustBeTrue) tmp.addConstraint(*pi);
    }
    if (pi!=pie) break;
  }

  std::vector< std::vector<unsigned char> > values;
  std::vector<const Array*> objects;
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    objects.push_back(state.symbolics[i].second);
  bool success = solver->getInitialValues(tmp, objects, values);
  solver->setTimeout(time::Span());
  if (!success) {
    klee_warning("unable to compute initial values (invalid constraints?)!");
    ExprPPrinter::printQuery(llvm::errs(), state.constraints.getAllConstraints(),
                             ConstantExpr::alloc(0, Expr::Bool));
    return false;
  }

  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    res.push_back(std::make_pair(state.symbolics[i].first->name, values[i]));
  return true;
}

void Executor::getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res) {
  res = state.coveredLines;
}

void Executor::doImpliedValueConcretization(ExecutionState &state,
                                            ref<Expr> e,
                                            ref<ConstantExpr> value) {
  abort(); // FIXME: Broken until we sort out how to do the write back.

  if (DebugCheckForImpliedValues)
    ImpliedValue::checkForImpliedValues(solver->solver, e, value);

  ImpliedValueList results;
  ImpliedValue::getImpliedValues(e, value, results);
  for (ImpliedValueList::iterator it = results.begin(), ie = results.end();
       it != ie; ++it) {
    ReadExpr *re = it->first.get();

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(re->index)) {
      // FIXME: This is the sole remaining usage of the Array object
      // variable. Kill me.
      const MemoryObject *mo = 0; //re->updates.root->object;
      const ObjectState *os = state.addressSpace.findObject(mo);

      if (!os) {
        // object has been free'd, no need to concretize (although as
        // in other cases we would like to concretize the outstanding
        // reads, but we have no facility for that yet)
      } else {
        assert(!os->readOnly &&
               "not possible? read only object with static read?");
        ObjectState *wos = state.addressSpace.getWriteable(mo, os);
        wos->write(CE, it->second, 0, nullptr);
      }
    }
  }
}

Expr::Width Executor::getWidthForLLVMType(llvm::Type *type) const {
  return kmodule->targetData->getTypeSizeInBits(type);
}

size_t Executor::getAllocationAlignment(const llvm::Value *allocSite) const {
  // FIXME: 8 was the previous default. We shouldn't hard code this
  // and should fetch the default from elsewhere.
  const size_t forcedAlignment = 8;
  size_t alignment = 0;
  llvm::Type *type = NULL;
  std::string allocationSiteName(allocSite->getName().str());
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(allocSite)) {
    alignment = GV->getAlignment();
    if (const GlobalVariable *globalVar = dyn_cast<GlobalVariable>(GV)) {
      // All GlobalVariables's have pointer type
      llvm::PointerType *ptrType =
          dyn_cast<llvm::PointerType>(globalVar->getType());
      assert(ptrType && "globalVar's type is not a pointer");
      type = ptrType->getElementType();
    } else {
      type = GV->getType();
    }
  } else if (const AllocaInst *AI = dyn_cast<AllocaInst>(allocSite)) {
    alignment = AI->getAlignment();
    type = AI->getAllocatedType();
  } else if (isa<InvokeInst>(allocSite) || isa<CallInst>(allocSite)) {
    // FIXME: Model the semantics of the call to use the right alignment
    llvm::Value *allocSiteNonConst = const_cast<llvm::Value *>(allocSite);
    const CallSite cs = (isa<InvokeInst>(allocSiteNonConst)
                             ? CallSite(cast<InvokeInst>(allocSiteNonConst))
                             : CallSite(cast<CallInst>(allocSiteNonConst)));
    llvm::Function *fn =
        klee::getDirectCallTarget(cs, /*moduleIsFullyLinked=*/true);
    if (fn)
      allocationSiteName = fn->getName().str();

    klee_warning_once(fn != NULL ? fn : allocSite,
                      "Alignment of memory from call \"%s\" is not "
                      "modelled. Using alignment of %zu.",
                      allocationSiteName.c_str(), forcedAlignment);
    alignment = forcedAlignment;
  } else {
    llvm_unreachable("Unhandled allocation site");
  }

  if (alignment == 0) {
    assert(type != NULL);
    // No specified alignment. Get the alignment for the type.
    if (type->isSized()) {
      alignment = kmodule->targetData->getPrefTypeAlignment(type);
    } else {
      klee_warning_once(allocSite, "Cannot determine memory alignment for "
                                   "\"%s\". Using alignment of %zu.",
                        allocationSiteName.c_str(), forcedAlignment);
      alignment = forcedAlignment;
    }
  }

  if (alignment < sizeof(void*)) {
    // force alignment to >= minimum alignment unit (sizeof(void*))
    alignment = sizeof(void*);
  }
  else if (!bits64::isPowerOfTwo(alignment)) {
    // Otherwise, we require alignment be a power of 2
    klee_warning_once(allocSite, "Alignment of %zu requested for %s but this "
                                 "not supported. Using alignment of %zu",
                      alignment, allocSite->getName().str().c_str(),
                      forcedAlignment);
    alignment = forcedAlignment;
  }
  assert(bits64::isPowerOfTwo(alignment) &&
         "Returned alignment must be a power of two");
  assert(alignment >= sizeof(void*) &&
         "Alignment should be a multiple of pointer size");
  return alignment;
}

/*** Runtime options ***/
void Executor::prepareForEarlyExit() {
  if (statsTracker) {
    // Make sure stats get flushed out
    statsTracker->done();
  }
}

void Executor::printInfo(llvm::raw_ostream &os) {
  static unsigned int cnt = 0;
  std::string message_buf;
  std::time_t walltime = std::time(nullptr);
  llvm::raw_string_ostream msg_oss(message_buf);
  llvm::raw_ostream &infoStream = interpreterHandler->getInfoStream();
  msg_oss << "********************************* Info " << cnt << "***********************\n";
  msg_oss << "Wall Time: " << std::asctime(std::localtime(&walltime)) << '\n';
  msg_oss << "Total Instructions: " << stats::instructions.getValue() << '\n';
  unsigned int i=0;
  for (auto s: states) {
    msg_oss << "================ ExecutionState: " << i << '\n'
       << "  ReplayPosition: " << (replayPath?std::to_string(s->replayPosition):"N/A")
         << " / " << (replayPath?std::to_string(replayPath->size()):"N/A") << '\n'
       << "  Stack:\n";
    s->dumpStack(msg_oss);
    char filenamebuf[128];
    std::snprintf(filenamebuf, 128, "constraints_cnt%03u_state%03u.kquery", cnt, i);
    std::string constraints_per_state_filename =
        interpreterHandler->getOutputFilename(filenamebuf);
    debugDumpConstraints(*s, s->constraints, ref<Expr>(0),
        constraints_per_state_filename.c_str());
    ++i;
  }
  msg_oss << "=============== Statistics =============\n";
  dumpStatisticsToLLVMrawos(msg_oss);
  infoStream << msg_oss.str();
  infoStream.flush();
  os << msg_oss.str();
  ++cnt;
}

/// Returns the errno location in memory
int *Executor::getErrnoLocation(const ExecutionState &state) const {
#if !defined(__APPLE__) && !defined(__FreeBSD__)
  /* From /usr/include/errno.h: it [errno] is a per-thread variable. */
  return __errno_location();
#else
  return __error();
#endif
}

void Executor::dumpStateAtBranch(ExecutionState &current, PathEntry pe, ref<Expr> new_constraint) {
  ++current.nbranches_rec;
  if (pathWriter) {
    current.pathOS << pe;
  }
  if (stackPathWriter) {
    current.dumpStackPathOS();
  }
  if (consPathWriter && !dyn_cast<ConstantExpr>(new_constraint)) {
    std::string BufferString;
    llvm::raw_string_ostream ExprWriter(BufferString);
    new_constraint.get()->print(ExprWriter);
    current.dumpConsPathOS(ExprWriter.str());
  }
  if (statsPathWriter) {
    current.dumpStatsPathOS();
  }
}

// dump execution state
// \param[in] new_constraint: could be Null but mustn't be ConstantExpr
void Executor::dumpStateAtFork(ExecutionState &current, ref<Expr> new_constraint) {
  if (stackPathWriter) {
    current.dumpStackPathOS();
  }
  if (consPathWriter && !new_constraint.isNull()) {
    assert(!dyn_cast<ConstantExpr>(new_constraint));
    std::string BufferString;
    llvm::raw_string_ostream ExprWriter(BufferString);
    new_constraint.get()->print(ExprWriter);
    current.dumpConsPathOS(ExprWriter.str());
  }
  if (statsPathWriter) {
    current.dumpStatsPathOS();
  }
}

void Executor::record1BitAtFork(ExecutionState &current, Solver::Validity solvalid) {
  assert((solvalid == Solver::True || solvalid == Solver::False) && "Don't support dumping Unknown fork");
  if (pathWriter) {
    PathEntry pe;
    pe.t = PathEntry::FORK;
    pe.body.br = ((solvalid == Solver::True)? true: false);
    current.pathOS << pe;
  }
}

void Executor::dumpPTree() {
  if (!::dumpPTree) return;

  char name[32];
  snprintf(name, sizeof(name),"ptree%08d.dot", (int) stats::instructions);
  auto os = interpreterHandler->openOutputFile(name);
  if (os) {
    processTree->dump(*os);
  }

  ::dumpPTree = 0;
}

void Executor::dumpStates() {
  if (!::dumpStates) return;

  auto os = interpreterHandler->openOutputFile("states.txt");

  if (os) {
    for (ExecutionState *es : states) {
      *os << "(" << es << ",";
      *os << "[";
      auto next = es->stack().begin();
      ++next;
      for (auto sfIt = es->stack().begin(), sf_ie = es->stack().end();
           sfIt != sf_ie; ++sfIt) {
        *os << "('" << sfIt->kf->function->getName().str() << "',";
        if (next == es->stack().end()) {
          *os << es->prevPC()->info->line << "), ";
        } else {
          *os << next->caller->info->line << "), ";
          ++next;
        }
      }
      *os << "], ";

      StackFrame &sf = es->stack().back();
      uint64_t md2u = computeMinDistToUncovered(es->pc(),
                                                sf.minDistToUncoveredOnReturn);
      uint64_t icnt = theStatisticManager->getIndexedValue(stats::instructions,
                                                           es->pc()->info->id);
      uint64_t cpicnt = sf.callPathNode->statistics.getValue(stats::instructions);

      *os << "{";
      *os << "'depth' : " << es->depth << ", ";
      *os << "'queryCost' : " << es->queryCost << ", ";
      *os << "'coveredNew' : " << es->coveredNew << ", ";
      *os << "'instsSinceCovNew' : " << es->instsSinceCovNew << ", ";
      *os << "'md2u' : " << md2u << ", ";
      *os << "'icnt' : " << icnt << ", ";
      *os << "'CPicnt' : " << cpicnt << ", ";
      *os << "}";
      *os << ")\n";
    }
  }

  ::dumpStates = 0;
}

///

Interpreter *Interpreter::create(LLVMContext &ctx, const InterpreterOptions &opts,
                                 InterpreterHandler *ih) {
  return new Executor(ctx, opts, ih);
}

/*
void Executor::writeStackKQueries(std::string& buf) {
  solver->writeStackKQueries(buf);
};
*/
void Executor::AssertNextBranchTaken(ExecutionState &state, bool br) {
  PathEntry pe;
  getNextPathEntry(state, pe);
  bool recorded_br;
  if (pe.t == PathEntry::FORK) {
    recorded_br = pe.body.br;
  }
  else {
    klee_error("Wrong PathEntry_t during asserting next branch");
  }
  if (br != recorded_br) {
    std::string constraints;
    getConstraintLog(state, constraints, Interpreter::KQUERY);
    auto f = interpreterHandler->openOutputFile("debugKQuery");
    if (f) {
      *f << constraints;
    }
    klee_message("replay: %d/%lu runtime: %d recorded: %d, stack:\n", state.replayPosition-1, replayPath->size(), br, recorded_br);
    state.dumpStack(llvm::errs());
    terminateStateOnError(state, "hit invalid branch in replay path mode", ReplayPath);
  }
}

void Executor::getNextBranchConstraint(ExecutionState &state, ref<Expr> condition,
    ref<Expr> &new_constraint, Solver::Validity &res) {
  PathEntry pe;
  getNextPathEntry(state, pe);
  if (pe.t == PathEntry::FORK) {
    getConstraintFromBool(condition, new_constraint, res, pe.body.br);
  }
  else {
    klee_error("Wrong recorded branch type");
  }
}

/*
 * try to load data for KInstruction KI from recorded data (do nothing if we are
 * not replaying)
 * 
 * If KI was a symbolic value during replay and now we load a concrete value for
 *   it, then we say this is an effective DataRec.
 * If KI was a LoadInstruction and it read a symbolic value during the replay,
 *   then we not only load recorded data to the corresponding register, but also
 *   do symbolic memory access to overwrite memory to be concrete.
 *   We assume the additional memory access introduced will be negligible to all
 *   other symbolic memory access.
 * If KI was already a concrete value but we load a different concrete value, a
 *   warning will be display.
 */
bool Executor::tryLoadDataRecording(ExecutionState &state, KInstruction *KI) {
  if (replayPath && replayDataRecEntries) {
    std::string uniqID = getKInstUniqueID(KI);
    PathEntry pe;
    DataRecEntry dre;
    getNextPathEntry(state, pe);
    getNextDataRecEntry(state, dre);
    assert((pe.t == PathEntry::DATAREC) && "When try loading DataRecording, PathEntry Type mismatches");
    assert((pe.body.drec.IDlen = uniqID.size()) && "When try loading DataRecording, uniqID length mismatches");
    ref<Expr> replayedValue = getDestCell(state, KI).value;
    ref<ConstantExpr> loadedValue = ConstantExpr::alloc(dre.data, pe.body.drec.width);
    if (!isa<ConstantExpr>(replayedValue)) {
      ++stats::dataRecLoadedEffective;
      klee_message("Effective dataRecLoaded at %u", state.replayDataRecEntriesPosition-1);
    }
    concretizeKInst(state, KI, loadedValue);
    return true;
  }
  return false;
}

/*
 * Use a given constant value to concretize the result of a given KInstruction.
 * Will add constraint (Eq loadedValue replayedValue) to the given state
 * Will symbolically write memory if the loadedValue is for a LoadInst
 * In case of recording < 64B data by ptwrite, there needs to be a explicit type
 * cast, this function will also recursively concertize the expression before
 * the type cast.
 * \param[in] state The ExecutionState in which the register associated with KI
 * will be concretized
 * \param[in] KI The KInstruction you recorded and want to concretize now
 * \param[in] loadedValue the recorded value from trace
 */
void Executor::concretizeKInst(ExecutionState &state, KInstruction *KI,
    ref<ConstantExpr> loadedValue) {
  ref<Expr> replayedValue = getDestCell(state, KI).value;
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(replayedValue)) {
      if (loadedValue->getZExtValue() != CE->getZExtValue()) {
        klee_warning("Loaded ConstantExpr %lu != Replayed %lu",
            loadedValue->getZExtValue(), CE->getZExtValue());
      }
    }
    else {
      if (replayedValue->getWidth() != loadedValue->getWidth()) {
        klee_warning("Width mismatch: Loaded ConstantExpr %u != Replayed %u",
            loadedValue->getWidth(), replayedValue->getWidth());
      }
      if (KI->inst->getOpcode() == Instruction::Load) {
        ref<Expr> base = eval(KI, 0, state).value;
        executeMemoryOperation(state, true, base, loadedValue, KI);
      }
      if (CastInst *ci = dyn_cast<CastInst>(KI->inst)) {
        // Further concretization opportunity:
        // e.g. if we can concretize (ZExt w64 (Read w8 xxx))
        // Then we can also concretize the inner ReadExpr
        // Note that we do not add constraints for the CastExpr but add
        // constraints to the expression inside instead.
        if (CastExpr *castE = dyn_cast<CastExpr>(replayedValue)) {
          klee_message("Further CastExpr concretization base on %s",
                       ci->getName().str().c_str());
          Instruction *innerI = dyn_cast<llvm::Instruction>(ci->getOperand(0));
          KInstruction *innerKInst = kmodule->getKInstruction(innerI);
          ref<Expr> innerExpr = castE->src;
          assert(innerI != nullptr && innerKInst != nullptr);
          assert(innerI->getType() == ci->getSrcTy());
          assert(getWidthForLLVMType(ci->getDestTy()) == castE->getWidth());
          assert(getWidthForLLVMType(innerI->getType()) ==
                 innerExpr->getWidth());
          concretizeKInst(state, innerKInst,
                          loadedValue->Extract(0, innerExpr->getWidth()));
        }
      } else {
        // we avoid adding multiple constraints in case of CastExpr
        // we only constrain the inner expression
        // here we add a new constraint: replayedValue == loadedValue
        addConstraint(state, EqExpr::create(replayedValue, loadedValue));
      }
      bindLocal(KI, state, loadedValue);
    }
}

/*
 * try to record intermediate data from KInstruction KI (do nothing if path
 * recording is disabled)
 */
bool Executor::tryStoreDataRecording(ExecutionState &state, KInstruction *KI) {
  if (pathWriter) {
    std::string uniqID = getKInstUniqueID(KI);
    PathEntry pe;
    ref<Expr> e = getDestCell(state, KI).value;
    ConstantExpr *CE = dyn_cast<ConstantExpr>(e);
    assert(CE && "should only record concrete values");
    pe.t = PathEntry::DATAREC;
    pe.body.drec.IDlen = uniqID.size();
    pe.body.drec.width = CE->getWidth();
    state.pathOS << pe;
    DataRecEntry dre;
    dre.instUniqueID = uniqID;
    dre.data = CE->getZExtValue();
    state.pathDataRecOS << dre;
    return true;
  }
  return false;
}

/* Multi-threading related function */
void Executor::bindArgumentToPthreadCreate(KFunction *kf, unsigned index,
                                           StackFrame &sf, ref<Expr> value) {
  getArgumentCell(sf, kf, index).value = value;
}

bool Executor::schedule(ExecutionState &state, bool yield) {
  thread_uid_t beforeSchedule = state.crtThread().tuid;
  int enabledCount = 0;
  for (ExecutionState::threads_ty::value_type &tit: state.threads) {
    if (tit.second.enabled) {
      ++enabledCount;
    }
  }
  if (enabledCount == 0) {
    terminateStateOnError(state, "******* hang (possible deadlock?)", User);
    return false;
  }

  // non preemption and preemption (yield or not) are currently unified
  // find the first enabled thread after current thread
  // TODO: cloud9 emulate all possible scheduling here by forking. But I think
  // deterministic scheduling is suffice for my use case.
  ExecutionState::threads_ty::iterator it = state.crtThreadIt;
  do {
    it = state.nextThread(it);
  } while (!it->second.enabled);
  state.scheduleNext(it);
  thread_uid_t afterSchedule = state.crtThread().tuid;
  if (pathWriter) {
    PathEntry pe;
    pe.t = PathEntry::SCHEDULE;
    pe.body.tgtid = afterSchedule.first;
    state.pathOS << pe;
  }
  if (replayPath) {
    PathEntry pe;
    getNextPathEntry(state, pe);
    assert(pe.t == PathEntry::SCHEDULE && "Wrong PathEntry_t during schedule");
    if (pe.body.tgtid != afterSchedule.first) {
      klee_message("Ambiguous scheduling, why?");
    }
  }
  if (DebugScheduling) {
    klee_message("Context Swtich: from %lu to %lu", beforeSchedule.first,
        afterSchedule.first);
  }
  return true;
}

void Executor::executeThreadCreate(ExecutionState &state, thread_id_t tid,
                                   ref<Expr> start_function, ref<Expr> arg) {
  klee_message("Creating thread %lu", tid);
  if (ConstantExpr *CE_f = dyn_cast<ConstantExpr>(start_function)) {
    Function *f = reinterpret_cast<Function *>(CE_f->getZExtValue());
    auto find_it = kmodule->functionMap.find(f);
    if (find_it != kmodule->functionMap.end()) {
      KFunction *kf = find_it->second;
      Thread &t = state.createThread(tid, kf);
      bindArgumentToPthreadCreate(kf, 0, t.stack.back(), arg);
      if (statsTracker)
        statsTracker->framePushed(state, &t.stack.back());
      return;
    }
  }
  // error path
  terminateStateOnError(
      state, "klee_thread_create cannot locate the start_function", User);
}
void Executor::executeThreadExit(ExecutionState &state) {
  if (state.threads.size() == 1) {
    terminateStateOnExit(state);
    return;
  }
  assert(state.threads.size() > 1);
  ExecutionState::threads_ty::iterator thrIt = state.crtThreadIt;
  thrIt->second.enabled = false;

  if (!schedule(state, false))
    return;
  state.terminateThread(thrIt);
}
/* MISC */
static void (*dummy_include_debug_helper)(llvm::raw_ostream &) __attribute__((unused)) = printDebugLibVersion;
