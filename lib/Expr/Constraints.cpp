//===-- Constraints.cpp ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Expr/Constraints.h"

#include "klee/Expr/ExprPPrinter.h"
#include "klee/Expr/ExprVisitor.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/OptionCategories.h"
#include "klee/Solver/SolverCmdLine.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/CommandLine.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include "klee/Expr/ExprHashMap.h"
#include "klee/util/RefHashMap.h"

using namespace klee;

namespace {
llvm::cl::opt<bool> RewriteEqualities(
    "rewrite-equalities",
    llvm::cl::desc("Rewrite existing constraints when an equality with a "
                   "constant is added (default=true)"),
    llvm::cl::init(true),
    llvm::cl::cat(SolvingCat));
}

/**
 * ExprReplaceVisitor can find and replace occurrence of a single expression.
 */
class ExprReplaceVisitorBase : public ExprVisitor {

protected:
  typedef ConstraintManager::UNMap_ty UNMap_ty;
  // This is for replacement deduplication in a ConstraintManager level
  // All optimized UpdateNodes are mapped to a unique UpdateNode according to
  // their **content** (including the following chain of UpdateNodes)
  UNMap_ty &replacedUN;
  // This is a replacement cache only lives inside this ExprVisitor
  UNMap_ty &visitedUN;

public:
  ExprReplaceVisitorBase(UNMap_ty &_replacedUN, UNMap_ty &_visitedUN)
      : ExprVisitor(true), replacedUN(_replacedUN), visitedUN(_visitedUN) {}
  ref<UpdateNode> visitUpdateNode(const ref<UpdateNode> &un) override {
    // TODO: In recursive mode, should I repeatedly search visited Exprs, so
    // that chains of replacement can resolved quicker.
    auto find_it = visitedUN.find(un);
    if (find_it != visitedUN.end()) {
      return find_it->second;
    }
    ref<UpdateNode> next;
    if (!un->next.isNull()) {
      next = visitUpdateNode(un->next);
    }
    const ref<Expr> index = visit(un->index);
    const ref<Expr> value = visit(un->value);
    if (index != un->index || value != un->value ||
        next.get() != un->next.get()) {
      ref<UpdateNode> newUN =
          new UpdateNode(next, index, value, un->flags, un->kinst);
      UNMap_ty::iterator replacedUN_it;
      bool unseen;
      std::tie(replacedUN_it, unseen) =
          replacedUN.insert(std::make_pair(newUN, newUN));
      if (!unseen) {
        newUN = replacedUN_it->second;
      }
      visitedUN[un] = newUN;
      return newUN;
    }
    visitedUN[un] = un;
    return un;
  }
};

class ExprReplaceVisitor : public ExprReplaceVisitorBase {
private:
  ref<Expr> src, dst;

public:
  ExprReplaceVisitor(UNMap_ty &_replaceUN, UNMap_ty &_visitedUN, ref<Expr> _src, ref<Expr> _dst)
      : ExprReplaceVisitorBase(_replaceUN, _visitedUN), src(_src), dst(_dst) {}

  Action visitExpr(const Expr &e) {
    if (e == *src.get()) {
      return Action::changeTo(dst);
    } else {
      return Action::doChildren();
    }
  }

  Action visitExprPost(const Expr &e) {
    if (e == *src.get()) {
      return Action::changeTo(dst);
    } else {
      return Action::doChildren();
    }
  }
};

/**
 * ExprReplaceVisitor2 can find and replace multiple expressions.
 * The replacement mapping is passed in as a std::map
 */
class ExprReplaceVisitor2 : public ExprReplaceVisitorBase {
private:
  const std::map< ref<Expr>, ref<Expr> > &replacements;

public:
  ExprReplaceVisitor2(UNMap_ty &_replaceUN, UNMap_ty &_visitedUN,
                      const std::map<ref<Expr>, ref<Expr>> &_replacements)
      : ExprReplaceVisitorBase(_replaceUN, _visitedUN), replacements(_replacements) {}

  Action visitExprPost(const Expr &e) {
    std::map< ref<Expr>, ref<Expr> >::const_iterator it =
      replacements.find(ref<Expr>(const_cast<Expr*>(&e)));
    if (it!=replacements.end()) {
      return Action::changeTo(it->second);
    } else {
      return Action::doChildren();
    }
  }
};

bool ConstraintManager::rewriteConstraints(ExprVisitor &visitor) {
  bool changed = false;
  if (old.empty()) {
    constraints.swap(old);
    for (ConstraintManager::constraints_ty::iterator 
           it = old.begin(), ie = old.end(); it != ie; ++it) {
      ref<Expr> &ce = *it;
      ref<Expr> e = visitor.visit(ce);

      if (e!=ce) {
        // TODO: maybe I can check if the rewritten expr has the same IndependentSet as previous one.
        addConstraintInternal(e); // enable further reductions
        changed = true;
      } else {
        constraints.push_back(ce);
      }
    }
  } else {
    ConstraintManager::constraints_ty old_temp;
    constraints.swap(old_temp);
    for (ConstraintManager::constraints_ty::iterator 
           it = old_temp.begin(), ie = old_temp.end(); it != ie; ++it) {
      ref<Expr> &ce = *it;
      ref<Expr> e = visitor.visit(ce);

      if (e!=ce) {
        addConstraintInternal(e); // enable further reductions
        changed = true;
      } else {
        constraints.push_back(ce);
      }
    }
  }

  return changed;
}

void ConstraintManager::simplifyForValidConstraint(ref<Expr> e) {
  // XXX 
}

ref<Expr> ConstraintManager::simplifyExpr(ref<Expr> e) const {
  if (isa<ConstantExpr>(e))
    return e;
  return ExprReplaceVisitor2(replacedUN, visitedUN, equalities).visit(e);
}

bool ConstraintManager::addConstraintInternal(ref<Expr> e) {
  if (representative.find(e) != representative.end()) {
    // found a duplicated constraint, nothing changed, directly return
    return false;
  }
  // rewrite any known equalities and split Ands into different conjuncts
  bool changed = false;
  switch (e->getKind()) {
  case Expr::Constant:
    assert(cast<ConstantExpr>(e)->isTrue() && 
           "attempt to add invalid (false) constraint");
    break;

    // split to enable finer grained independence and other optimizations
  case Expr::And: {
    BinaryExpr *be = cast<BinaryExpr>(e);
    changed |= addConstraintInternal(be->left);
    changed |= addConstraintInternal(be->right);
    break;
  }

  case Expr::Eq: {
    if (RewriteEqualities) {
      // XXX: should profile the effects of this and the overhead.
      // traversing the constraints looking for equalities is hardly the
      // slowest thing we do, but it is probably nicer to have a
      // ConstraintSet ADT which efficiently remembers obvious patterns
      // (byte-constant comparison).
      BinaryExpr *be = cast<BinaryExpr>(e);
      if (isa<ConstantExpr>(be->left) && !isa<EqExpr>(be->right)) {
        ExprReplaceVisitor visitor(replacedUN, visitedUN, be->right, be->left);
        visitedUN.clear();
        changed |= rewriteConstraints(visitor);
      }
    }
    constraints.push_back(e);
    addedConstraints.push_back(e);
    break;
  }

  default:
    constraints.push_back(e);
    addedConstraints.push_back(e);
    break;
  }
  return changed;
}

void ConstraintManager::updateDelete() {
  // I believe delete only happens when a constraint is rewritten. But I think
  // it is unlikely to break a IndependentSet if you rewrite a constraint.
  // TODO: do we really want to traverse the entire IndependentSet upon delete/rewrite?
  // Get the set for all the IndependentSet that need to be update.
  // key: factors need to update
  // value: set of constraints (expressions) to delete in the corresponding factor
  std::unordered_map<IndependentElementSet*, ExprHashSet > updateList;
  for (auto it = deleteConstraints.begin(); it != deleteConstraints.end(); it++) {
    updateList[representative[*it]].insert(*it);
    // Update representative.
    representative.erase(*it);
  }

  // Update factors.
  for (auto it = updateList.begin(); it != updateList.end(); it++) {
    factors.erase(it->first);
  }

  for (auto it = updateList.begin(); it != updateList.end(); it++) {
    std::vector<IndependentElementSet*> temp;
    for (auto e = it->first->exprs.begin(); e != it->first->exprs.end(); e++) {
      auto find = it->second.find(*e);
      if (find == it->second.end()) {
        temp.push_back(new IndependentElementSet(*e));
      }
    }

    std::vector<IndependentElementSet*> result;

    if (!temp.empty()) {
      result.push_back(temp.back());
      temp.pop_back();
    }

    while (!temp.empty()) {
      IndependentElementSet* current = temp.back();
      temp.pop_back();
      unsigned int i = 0;
      while (i < result.size()) {
        if (current->intersects(*result[i])) {
          current->add(*result[i]);
          IndependentElementSet* victim = result[i];
          result[i] = result.back();
          result.pop_back();
          delete victim;
        } else {
          i++;
        }
      }
      result.push_back(current);
    }

    // Update representative and factors.
    for (auto r = result.begin(); r != result.end(); r++) {
      factors.insert(*r);
      for (auto e = (*r)->exprs.begin(); e != (*r)->exprs.end(); e++) {
          representative[*e] = *r;
      }
    }
  }
}

// Update the representative after adding constraint
void ConstraintManager::updateIndependentSet() {
  // First find if there are removed constraint. If is, update the correspoding set.
  if (!deleteConstraints.empty()) {
    updateDelete();
  }

  while (!addedConstraints.empty()) {
    IndependentElementSet* current = new IndependentElementSet(addedConstraints.back());
    addedConstraints.pop_back();
    // garbage consists of existing factors which are intersected with
    // this new constraint
    std::vector<IndependentElementSet*> garbage;
    for (auto it = factors.begin(); it != factors.end(); it++ ) {
      if (current->intersects(*(*it))) {
        garbage.push_back(*it);
      }
    }

    if (garbage.size() == 1) {
      // special case: newly added constraint falls exactly in one existing factor
      // we can reuse the existing factor
      IndependentElementSet *singleIntersect = garbage.back();
      singleIntersect->add(*current);
      for (auto &it: current->exprs) {
        representative[it] = singleIntersect;
      }
      delete current;
    }
    else {
      for (IndependentElementSet *indepSet: garbage) {
        current->add(*indepSet);
      }
      // Update representative and factors.
      for (auto it = current->exprs.begin(); it != current->exprs.end(); it ++ ) {
        representative[*it] = current;
      }

      for (IndependentElementSet *victim: garbage) {
        factors.erase(victim);
        delete victim;
      }

      factors.insert(current);
    }
  }
}

void ConstraintManager::updateEqualities() {
  for (auto &refE: addedConstraints) {
    bool isConstantEq = false;
    if (const EqExpr *EE = dyn_cast<EqExpr>(refE)) {
      if (isa<ConstantExpr>(EE->left)) {
        equalities[EE->right] = EE->left;
        isConstantEq = true;
      }
    }
    if (!isConstantEq) {
      equalities[refE] = ConstantExpr::alloc(1, Expr::Bool);
    }
  }
  for (auto refE: deleteConstraints) {
    bool isConstantEq = false;
    if (const EqExpr *EE = dyn_cast<EqExpr>(refE)) {
      if (isa<ConstantExpr>(EE->left)) {
        equalities.erase(EE->right);
        isConstantEq = true;
      }
    }
    if (!isConstantEq) {
      equalities.erase(refE);
    }
  }
}

void ConstraintManager::checkConstraintChange() {
  ExprHashSet oldConsSet;
  while (!old.empty()) {
    oldConsSet.insert(old.back());
    old.pop_back();
  }

  for (auto it = constraints.begin(); it != constraints.end(); it++ ) {
    if (!oldConsSet.erase(*it)) {
      addedConstraints.push_back(*it);
    } 
  }

  // The rest element inside oldConsSet should be the deleted element.
  for (auto itr = oldConsSet.begin(); itr != oldConsSet.end(); ++itr) {
    deleteConstraints.push_back(*itr);
  }
}

bool ConstraintManager::addConstraint(ref<Expr> e) {
  if (representative.find(e) != representative.end()) {
    // found a duplicated constraint
    return true;
  }
  // After update the independant, the add and delete vector should be cleaned;
  assert(old.empty() && "old vector is not empty"); 
  assert(deleteConstraints.empty() && "delete Constraints not empty"); 
  assert(addedConstraints.empty() && "add Constraints not empty"); 

  ref<Expr> simplified = simplifyExpr(e);
  if (simplified->isFalse()) {
    return false;
  }
  bool changed = addConstraintInternal(simplified);

  // If the constraints are changed by rewriteConstraints. Check what has been
  // modified; Important clear the old vector after finish running.
  if (changed) {
    addedConstraints.clear();
    checkConstraintChange();
  }
  old.clear();

  updateEqualities();

  // NOTE that updateIndependentSet will destroy addedConstraints and
  // deletedConstraints. Thus I should updateEqualities before this.
  if (UseIndependentSolver) {
    updateIndependentSet();
  }

  addedConstraints.clear();
  deleteConstraints.clear();
  // TODO Check factors are exclusive (sum the number of constraints and compare with representative.size())
  return true;
}

ConstraintManager::ConstraintManager(const std::vector< ref<Expr> > &_constraints) :
  constraints(_constraints) {
    // Need to establish factors and representative
    std::vector<IndependentElementSet*> temp;
    for (auto it = _constraints.begin(); it != _constraints.end(); it ++ ) {
      temp.push_back(new IndependentElementSet(*it));
    }

    std::vector<IndependentElementSet*> result;
    if (!temp.empty()) {
      result.push_back(temp.back());
      temp.pop_back();
    }
    // work like this:
    // assume IndependentSet was setup for each constraint independently,
    // represented by I0, I1, ... In
    // temp initially has: I1...In
    // result initially has: I0
    // then for each IndependentSet Ii in temp, scan the entire result vector,
    // combine any IndependentSet in result intersecting with Ii and put Ii
    // in the result vector.
    //
    // result vector should only contain exclusive IndependentSet all the time.

    while (!temp.empty()) {
      IndependentElementSet* current = temp.back();
      temp.pop_back();
      unsigned int i = 0;
      while (i < result.size()) {
        if (current->intersects(*result[i])) {
          current->add(*result[i]);
          IndependentElementSet* victim = result[i];
          result[i] = result.back();
          result.pop_back();
          delete victim;
        } else {
          i++;
        }
      }
      result.push_back(current);
    }

    for (auto r = result.begin(); r != result.end(); r++) {
      factors.insert(*r);
      for (auto e = (*r)->exprs.begin(); e != (*r)->exprs.end(); e++) {
        representative[*e] = *r;
      }
    }
  }

ConstraintManager::ConstraintManager(const ConstraintManager &cs) : constraints(cs.constraints) {
  // Copy constructor needs to make deep copy of factors and representative
  // Here we assume every IndependentElementSet point in representative also exist in factors.
  for (auto it = cs.factors.begin(); it != cs.factors.end(); it++) {
    IndependentElementSet* candidate = new IndependentElementSet(*(*it));
    factors.insert(candidate);
    for (auto e = candidate->exprs.begin(); e != candidate->exprs.end(); e++) {
      representative[*e] = candidate;
    }
  }
}

// Destructor
ConstraintManager::~ConstraintManager() {
  // Here we assume every IndependentElementSet point in representative also exist in factors.
  for (auto it = factors.begin(); it != factors.end(); it++) {
    delete(*it);
  }
  // TODO Double check your assumption
}
