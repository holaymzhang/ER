//===-- IndependentSolver.cpp ---------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "independent-solver"
#include "klee/Solver/Solver.h"

#include "klee/Expr/Assignment.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Internal/Support/Debug.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include "klee/Internal/Support/IndependentElementSet.h"
#include "klee/Solver/SolverStats.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/Solver/SolverImpl.h"

#include "llvm/Support/raw_ostream.h"

#include <list>
#include <map>
#include <ostream>
#include <vector>

#undef INDEPENDENT_DEBUG
#undef INDEPENDENT_DEBUG_DUMPCONSTRAINTS

#ifdef INDEPENDENT_DEBUG
#include "klee/Expr/ExprDebugHelper.h"
#endif

using namespace klee;
using llvm::errs;

// Breaks down a constraint into all of it's individual pieces, returning a
// list of IndependentElementSets or the independent factors.
//
// Caller takes ownership of returned std::list.
static std::list<IndependentElementSet>*
getAllIndependentConstraintsSets(const Query &query) {
  std::list<IndependentElementSet> *result= new std::list<IndependentElementSet>();
  ConstantExpr *CE = dyn_cast<ConstantExpr>(query.expr);
  IndependentElementSet current;
  if (CE) {
    assert(CE && CE->isFalse() && "the expr should always be false and "
                                  "therefore not included in factors");
  } else {
    ref<Expr> neg = Expr::createIsZero(query.expr);
    current = IndependentElementSet(neg);
  }

  for (ConstraintManager::factor_iterator it = query.constraintMgr.factor_begin(),
                ie = query.constraintMgr.factor_end(); it != ie; ++it) {
    if (current.intersects(*(*it))) {
      current.add(*(*it));
    } else {
      result->push_back(*(*it));
    }
  }
  
  return result;
}

static 
IndependentElementSet getIndependentConstraints(const Query& query,
                                                std::vector< ref<Expr> > &result) {
  
  IndependentElementSet eltsClosure(query.expr);

  for (ConstraintManager::factor_iterator it = query.constraintMgr.factor_begin(),
                ie = query.constraintMgr.factor_end(); it != ie; ++it) {
    if (eltsClosure.intersects(*(*it))) {
      // The eltsClosure represents the IndependentElementSet associated with
      // the query expr.
      // At a high level, you just take eltsClosure and try every existing factors.
      // If it has intersection with any factor, associated expressions should
      // be put in result vector.
      //
      // Note that factors managed by ConstraintManager should be exclusive.
      // So there will not exists two factors f1 f2, that eltsClosure.add(f1)
      // hides expressions in f2.
      // The if condition commented out bellow is redundant.
      //if (eltsClosure.add(*(*it))) {
        for (auto eb = (*it)->exprs.begin(), ee =  (*it)->exprs.end(); eb != ee; eb++) {
           result.push_back(*eb);
        //}
      }
    }
  }

  // **********************************************************

  KLEE_DEBUG(
    std::set< ref<Expr> > reqset(result.begin(), result.end());
    errs() << "--\n";
    errs() << "Q: " << query.expr << "\n";
    errs() << "\telts: " << IndependentElementSet(query.expr) << "\n";
    int i = 0;
    for (ConstraintManager::const_iterator it = query.constraints.begin(),
      ie = query.constraints.end(); it != ie; ++it) {
      errs() << "C" << i++ << ": " << *it;
      errs() << " " << (reqset.count(*it) ? "(required)" : "(independent)") << "\n";
      errs() << "\telts: " << IndependentElementSet(*it) << "\n";
    }
    errs() << "elts closure: " << eltsClosure << "\n";
 );

  stats::independentConstraints += result.size();
  stats::independentAllConstraints += query.constraints.size();
  return eltsClosure;
}


// Extracts which arrays are referenced from a particular independent set.  Examines both
// the actual known array accesses arr[1] plus the undetermined accesses arr[x].
static
void calculateArrayReferences(const IndependentElementSet & ie,
                              std::set<const Array *> &returnSet){
  for(std::map<const Array*, DenseSet<unsigned> >::const_iterator it = ie.elements.begin();
      it != ie.elements.end(); it ++){
    returnSet.insert(it->first);
  }
  for(std::set<const Array *>::iterator it = ie.wholeObjects.begin();
      it != ie.wholeObjects.end(); it ++){
    returnSet.insert(*it);
  }
}

class IndependentSolver : public SolverImpl {
private:
  Solver *solver;

public:
  IndependentSolver(Solver *_solver) 
    : solver(_solver) {}
  ~IndependentSolver() { delete solver; }

  bool computeTruth(const Query&, bool &isValid);
  bool computeValidity(const Query&, Solver::Validity &result);
  bool computeValue(const Query&, ref<Expr> &result);
  bool computeInitialValues(const Query& query,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution) {
    switch (UseIndependentSolverType) {
      case IndependentSolverType::PER_FACTOR:
        return computeInitialValuesPerFactor(query, objects, values,
                                             hasSolution);
      case IndependentSolverType::BATCH:
        return computeInitialValuesBatch(query, objects, values, hasSolution);
      default:
        assert(0 && "Unknown Independent Solver Type");
    }
  }
  bool computeInitialValuesPerFactor(const Query& query,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution);
  bool computeInitialValuesBatch(const Query& query,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution);
  SolverRunStatus getOperationStatusCode();
  char *getConstraintLog(const Query&);
  void setCoreSolverTimeout(time::Span timeout);
};
  
bool IndependentSolver::computeValidity(const Query& query,
                                        Solver::Validity &result) {
  TimerStatIncrementer t(stats::independentTime);
  std::vector< ref<Expr> > required;
  IndependentElementSet eltsClosure =
    getIndependentConstraints(query, required);
  return solver->impl->computeValidity(Query(query.constraintMgr, required, query.expr),
                                       result);
}

bool IndependentSolver::computeTruth(const Query& query, bool &isValid) {
  TimerStatIncrementer t(stats::independentTime);
  std::vector< ref<Expr> > required;
  IndependentElementSet eltsClosure = 
    getIndependentConstraints(query, required);
  return solver->impl->computeTruth(Query(query.constraintMgr, required, query.expr),
                                    isValid);
}

bool IndependentSolver::computeValue(const Query& query, ref<Expr> &result) {
  TimerStatIncrementer t(stats::independentTime);
  std::vector< ref<Expr> > required;
  IndependentElementSet eltsClosure = 
    getIndependentConstraints(query, required);
  return solver->impl->computeValue(Query(query.constraintMgr, required, query.expr), result);
}

// Helper function used only for assertions to make sure point created
// during computeInitialValues is in fact correct. The ``retMap`` is used
// in the case ``objects`` doesn't contain all the assignments needed.
bool assertCreatedPointEvaluatesToTrue(
    const Query &query, const std::vector<const Array *> &objects,
    std::vector<std::vector<unsigned char>> &values,
    std::map<const Array *, std::vector<unsigned char>> &retMap) {
  // _allowFreeValues is set to true so that if there are missing bytes in the
  // assigment we will end up with a non ConstantExpr after evaluating the
  // assignment and fail
  Assignment assign = Assignment(objects, values, /*_allowFreeValues=*/true);

  // Add any additional bindings.
  // The semantics of std::map should be to not insert a (key, value)
  // pair if it already exists so we should continue to use the assignment
  // from ``objects`` and ``values``.
  if (retMap.size() > 0)
    assign.bindings.insert(retMap.begin(), retMap.end());

  for (auto const &constraint : query.constraints) {
    ref<Expr> ret = assign.evaluate(constraint);

    if (!isa<ConstantExpr>(ret)) {
      std::string constraint_str;
      std::string evaluated_str;
      llvm::raw_string_ostream constraint_strOS(constraint_str);
      llvm::raw_string_ostream evaluated_strOS(evaluated_str);
      constraint->print(constraint_strOS);
      ret->print(evaluated_strOS);
      klee_warning("assignment evaluation did not result in constant:\n"
          "\tconstraint:%s\n\tevaluated:%s",
          constraint_strOS.str().c_str(), evaluated_strOS.str().c_str());
    }
    else {
      ref<ConstantExpr> evaluatedConstraint = dyn_cast<ConstantExpr>(ret);
      if (evaluatedConstraint->isFalse()) {
        return false;
      }
    }
  }
  ref<Expr> neg = Expr::createIsZero(query.expr);
  ref<Expr> q = assign.evaluate(neg);
  assert(isa<ConstantExpr>(q) &&
         "assignment evaluation did not result in constant");
  return cast<ConstantExpr>(q)->isTrue();
}

bool IndependentSolver::computeInitialValuesPerFactor(const Query& query,
                                             const std::vector<const Array*> &objects,
                                             std::vector< std::vector<unsigned char> > &values,
                                             bool &hasSolution) {
  TimerStatIncrementer t(stats::independentTime);
  // We assume the query has a solution except proven differently
  // This is important in case we don't have any constraints but
  // we need initial values for requested array objects.
  hasSolution = true;
  // FIXME: When we switch to C++11 this should be a std::unique_ptr so we don't need
  // to remember to manually call delete
  std::list<IndependentElementSet> *factors = getAllIndependentConstraintsSets(query);

  //Used to rearrange all of the answers into the correct order
  std::map<const Array*, std::vector<unsigned char> > retMap;
#ifdef INDEPENDENT_DEBUG
  unsigned int id = 0;
#endif
  for (std::list<IndependentElementSet>::iterator it = factors->begin();
       it != factors->end(); ++it) {
    std::set<const Array*> arraysInFactorSet;
    std::vector<const Array*> arraysInFactor;
    calculateArrayReferences(*it, arraysInFactorSet);
    arraysInFactor.insert(arraysInFactor.end(), arraysInFactorSet.begin(),
                          arraysInFactorSet.end());
    // Going to use this as the "fresh" expression for the Query() invocation below
    assert(it->exprs.size() >= 1 && "No null/empty factors");
    if (arraysInFactor.size() == 0){
      continue;
    }
#ifdef INDEPENDENT_DEBUG
    /* IndependentSolver performance debugging */
    llvm::errs() << "independent set " << id << " / " << factors->size()
                 << " #array: " << arraysInFactor.size()
                 << " #expr: " << it->exprs.size() << ' ';
#ifdef INDEPENDENT_DEBUG_DUMPCONSTRAINTS
    char dumpfilename[128];
    snprintf(dumpfilename, sizeof(dumpfilename), "independentQuery_%05u.kquery",
             id);
    ++id;
    debugDumpConstraintsImpl(it->exprs, arraysInFactor, dumpfilename);
#endif
    const WallTimer solver_timer;
#endif
    std::vector<std::vector<unsigned char> > tempValues;
    if (!solver->impl->computeInitialValues(Query(query.constraintMgr, it->exprs, ConstantExpr::alloc(0, Expr::Bool)),
                                            arraysInFactor, tempValues, hasSolution)){
      values.clear();
      delete factors;
      return false;
    } else if (!hasSolution){
      values.clear();
      delete factors;
      return true;
    } else {
#ifdef INDEPENDENT_DEBUG
      time::Span solver_time = solver_timer.delta();
      const WallTimer result_timer;
#endif
      assert(tempValues.size() == arraysInFactor.size() &&
             "Should be equal number arrays and answers");
      for (unsigned i = 0; i < tempValues.size(); i++){
        if (retMap.count(arraysInFactor[i])){
          // We already have an array with some partially correct answers,
          // so we need to place the answers to the new query into the right
          // spot while avoiding the undetermined values also in the array
          std::vector<unsigned char> * tempPtr = &retMap[arraysInFactor[i]];
          assert(tempPtr->size() == tempValues[i].size() &&
                 "we're talking about the same array here");
          DenseSet<unsigned> * ds = &(it->elements[arraysInFactor[i]]);
          for (std::set<unsigned>::iterator it2 = ds->begin(); it2 != ds->end(); it2++){
            unsigned index = * it2;
            (* tempPtr)[index] = tempValues[i][index];
          }
        } else {
          // Dump all the new values into the array
          retMap[arraysInFactor[i]] = tempValues[i];
        }
      }
#ifdef INDEPENDENT_DEBUG
      time::Span result_time = result_timer.delta();
      llvm::errs() << "solver_time(us): " << solver_time.toMicroseconds()
                   << " result_time(us): " << result_time.toMicroseconds()
                   << "\n";
#endif
    }
  }
  for (std::vector<const Array *>::const_iterator it = objects.begin();
       it != objects.end(); it++){
    const Array * arr = * it;
    if (!retMap.count(arr)){
      // this means we have an array that is somehow related to the
      // constraint, but whose values aren't actually required to
      // satisfy the query.
      std::vector<unsigned char> ret(arr->size);
      values.push_back(ret);
    } else {
      values.push_back(retMap[arr]);
    }
  }
  assert(assertCreatedPointEvaluatesToTrue(query, objects, values, retMap) && "should satisfy the equation");
  delete factors;
  return true;
}

SolverImpl::SolverRunStatus IndependentSolver::getOperationStatusCode() {
  return solver->impl->getOperationStatusCode();      
}

char *IndependentSolver::getConstraintLog(const Query& query) {
  return solver->impl->getConstraintLog(query);
}

void IndependentSolver::setCoreSolverTimeout(time::Span timeout) {
  solver->impl->setCoreSolverTimeout(timeout);
}

Solver *klee::createIndependentSolver(Solver *s) {
  return new Solver(new IndependentSolver(s));
}

bool IndependentSolver::computeInitialValuesBatch(const Query& query,
                                             const std::vector<const Array*> &objects,
                                             std::vector< std::vector<unsigned char> > &values,
                                             bool &hasSolution){
  TimerStatIncrementer t(stats::independentTime);
#ifdef INDEPENDENT_DEBUG
  const WallTimer total_timer;
#endif
  // We assume the query has a solution except proven differently
  // This is important in case we don't have any constraints but
  // we need initial values for requested array objects.
  hasSolution = true;
  // FIXME: When we switch to C++11 this should be a std::unique_ptr so we don't need
  // to remember to manually call delete
  std::list<IndependentElementSet> *factors = getAllIndependentConstraintsSets(query);

  //Used to rearrange all of the answers into the correct order
  std::map<const Array*, std::vector<unsigned char> > retMap;
  typedef std::vector<std::list<IndependentElementSet>::const_iterator>
    IndEleSetPart_t;
  std::vector<IndEleSetPart_t> part_container(1);
  factors->sort([](auto &a, auto &b) {
      return a.exprs.size() < b.exprs.size();
      });
#ifdef INDEPENDENT_DEBUG
  unsigned int id = 0;
#endif
  unsigned int acc_expr_cnt = 0;
  unsigned int acc_factor_cnt = 0;
  for (std::list<IndependentElementSet>::const_iterator it = factors->begin();
       it != factors->end(); ++it) {
    assert(it->exprs.size() >= 1 && "No null/empty factors");
    if (acc_expr_cnt >= ExprNumThreshold) {
#ifdef INDEPENDENT_DEBUG
      llvm::errs() << "id: " << id << " #expr: " << acc_expr_cnt
                   << " #factors: " << acc_factor_cnt << " "
                   << double(acc_expr_cnt) / acc_factor_cnt << '\n';
      ++id;
#endif
      part_container.push_back({});
      acc_expr_cnt = 0;
      acc_factor_cnt = 0;
    }
    part_container.back().push_back(it);
    acc_expr_cnt += it->exprs.size();
    ++acc_factor_cnt;
  }
  if (part_container.back().empty()) {
    part_container.pop_back();
#ifdef INDEPENDENT_DEBUG
  } else {
    llvm::errs() << "id: " << id << " #expr: " << acc_expr_cnt
                 << " #factors: " << acc_factor_cnt << " "
                 << double(acc_expr_cnt) / acc_factor_cnt << '\n';
  }
  id = 0;
#else
  }
#endif
  for (const IndEleSetPart_t &p : part_container) {
    Constraints_ty constraints;
    std::set<const Array *> arraysInFactorSet;
    std::vector<const Array *> arraysInFactor;
    for (std::list<IndependentElementSet>::const_iterator it : p) {
      calculateArrayReferences(*it, arraysInFactorSet);
      // Going to use this as the "fresh" expression for the Query() invocation
      // below
      assert(it->exprs.size() >= 1 && "No null/empty factors");
      assert(arraysInFactorSet.size() > 0);
      constraints.insert(constraints.end(), it->exprs.begin(), it->exprs.end());
    }
    arraysInFactor.insert(arraysInFactor.end(), arraysInFactorSet.begin(),
                          arraysInFactorSet.end());
#ifdef INDEPENDENT_DEBUG
    /* IndependentSolver performance debugging */
    llvm::errs() << "independent set part" << id
                 << " #array: " << arraysInFactor.size()
                 << " #expr: " << constraints.size() << ' ';
    char dumpfilename[128];
    snprintf(dumpfilename, sizeof(dumpfilename), "independentQuery_%05u.kquery",
             id);
    ++id;
    debugDumpConstraintsImpl(constraints, arraysInFactor, dumpfilename);
    const WallTimer solver_timer;
#endif
    std::vector<std::vector<unsigned char>> tempValues;
    if (!solver->impl->computeInitialValues(
            Query(query.constraintMgr, constraints,
                  ConstantExpr::alloc(0, Expr::Bool)),
            arraysInFactor, tempValues, hasSolution)) {
      values.clear();
      delete factors;
      return false;
    } else if (!hasSolution) {
      values.clear();
      delete factors;
      return true;
    } else {
#ifdef INDEPENDENT_DEBUG
      time::Span solver_time = solver_timer.delta();
      const WallTimer result_timer;
#endif
      assert(tempValues.size() == arraysInFactor.size() &&
             "Should be equal number arrays and answers");
      for (unsigned i = 0; i < tempValues.size(); i++) {
        if (retMap.count(arraysInFactor[i])) {
          // We already have an array with some partially correct answers,
          // so we need to place the answers to the new query into the right
          // spot while avoiding the undetermined values also in the array
          std::vector<unsigned char> *tempPtr = &retMap[arraysInFactor[i]];
          assert(tempPtr->size() == tempValues[i].size() &&
                 "we're talking about the same array here");
          for (std::list<IndependentElementSet>::const_iterator it:p) {
            auto find_it = it->elements.find(arraysInFactor[i]);
            // assert(find_it != it->elements.end() &&
            //       "Array found in IndependentElementSet is now gone");
            if (find_it != it->elements.end()) {
              const DenseSet<unsigned> &ds = find_it->second;
              for (auto index : ds) {
                (*tempPtr)[index] = tempValues[i][index];
              }
            }
          }
        } else {
          // Dump all the new values into the array
          retMap[arraysInFactor[i]] = tempValues[i];
        }
      }
#ifdef INDEPENDENT_DEBUG
      time::Span result_time = result_timer.delta();
      llvm::errs() << "solver_time(us): " << solver_time.toMicroseconds()
                   << " result_time(us): " << result_time.toMicroseconds()
                   << "\n";
#endif
    }
  }
  for (std::vector<const Array *>::const_iterator it = objects.begin();
       it != objects.end(); it++){
    const Array * arr = * it;
    if (!retMap.count(arr)){
      // this means we have an array that is somehow related to the
      // constraint, but whose values aren't actually required to
      // satisfy the query.
      std::vector<unsigned char> ret(arr->size);
      values.push_back(ret);
    } else {
      values.push_back(retMap[arr]);
    }
  }
#ifdef INDEPENDENT_DEBUG
  time::Span total_time = total_timer.delta();
  llvm::errs() << "total time(us): " << total_time.toMicroseconds() << '\n';
#endif
  assert(assertCreatedPointEvaluatesToTrue(query, objects, values, retMap) && "should satisfy the equation");
  delete factors;
  return true;
}
