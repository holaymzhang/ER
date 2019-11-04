#include "ExprInPlaceTransformation.h"
using namespace klee;
static Expr *under_processing_expr = (Expr*)(0x1);
static const UpdateNode *under_processing_un = (const UpdateNode*)(0x1);

ExprInPlaceTransformer::ExprInPlaceTransformer(ConstraintManager &_cm,
    std::vector<ref<Expr>> &constraints): cm(_cm) {
  constraints.clear();
  for (const ref<Expr> &e: cm) {
    visitDFS(e.get());
    constraints.push_back(popKidExpr());
  }
}
// TODO UpdateNode* memory leak
void ExprInPlaceTransformer::visitDFS(Expr *e) {
  expr_worklist.push_back(e);
  while (!expr_worklist.empty()) {
    WorkListEntry &entry = expr_worklist.back();
    if (entry.isExpr()) {
      visitExpr(entry.e);
    }
    else if (entry.isUNode()) {
      visitUNode(entry.un);
    }
  }
}

void ExprInPlaceTransformer::visitExpr(Expr *e) {
  if (isa<ConstantExpr>(e)) {
    // ConstantExpr has no kids and should be omitted
    expr_kidstack.push_back((Expr*)nullptr);
    expr_worklist.pop_back();
    return;
  }
  // NonConstantExpr
  auto expr_find = visited_expr.find(e);
  if (expr_find == visited_expr.end()) {
    // 1st visit, no cached replacement result.
    // mark it as visited (under processing)
    visited_expr[e] = under_processing_expr;
    for (unsigned i=0; i < e->getNumKids(); ++i) {
      expr_worklist.push_back(e->getKid(i).get());
    }
    // need to handle updatelist in ReadExpr separately
    if (ReadExpr *RE = dyn_cast<ReadExpr>(e)) {
      expr_worklist.push_back(RE->updates.head);
    }
  }
  else if (expr_find->second == under_processing_expr) {
    // visited, no cached replacement, should pop from the worklist, rebuild itself then push to kidstack
    // kidstack looks like [ Nth_kid, ..., 1st_kid ]
    // note that update node
    //
    // Given there is no cycle, `under_processing_expr` flag works.
    ref<Expr> kids[8];
    unsigned int N = e->getNumKids();
    // Here we use std::set because different kids may be simplified to the
    // same Expr* in the end.
    std::set<Expr*> nonnull_kids;
    for (unsigned int i=0; i < N; ++i) {
        WorkListEntry &we = expr_kidstack.back();
        if (!we.isExpr()) {
          assert(0 && "rebuildInPlace expects Expr*");
        }
        kids[i] = we.e;
        if (we.e) {
          nonnull_kids.insert(we.e);
        }
        expr_kidstack.pop_back();
    }
    if (ReadExpr *RE = dyn_cast<ReadExpr>(e)) {
      // in-place generate the replacement of a ReadExpr
      // note: ReadExpr should never be omitted
      const UpdateNode *new_un = popKidUNode();
      RE->resetUpdateNode(new_un);
      if (new_un != 0 || nonnull_kids.size() != 0) {
        // Do not omit the index of last-level-read.
        // since ReadExpr only have one kid
        // (updatelist was historically not considered as kid)
        // this branch means we need rebuildInPlace (will only overwrite
        //   the index) only if:
        // 1. non-null updatelist (not last-level-read)
        // OR
        // 2. non-const index (index will not be omitted anyway)
        RE->rebuildInPlace(kids);
      }
      expr_kidstack.push_back(RE);
      visited_expr[RE] = RE;
    }
    else {
      // in-place generate the replacement of a non-ReadExpr
      Expr *replaced_expr;
      if (nonnull_kids.size() == 0) {
        // can be omitted to null
        replaced_expr = nullptr;
      }
      else if (nonnull_kids.size() == 1) {
        // can be omitted to its only dependence
        replaced_expr = *(nonnull_kids.begin());
      }
      else {
        // cannot be omitted, just rebuildInPlace itself
        e->rebuildInPlace(kids);
        replaced_expr = e;
      }
      expr_kidstack.push_back(replaced_expr);
      visited_expr[e] = replaced_expr;
    }
    expr_worklist.pop_back();
  } else {
    // visited, cached replacement, should pop reuse cached replacement
    expr_kidstack.push_back(expr_find->second);
    expr_worklist.pop_back();
  }
}

void ExprInPlaceTransformer::visitUNode(const UpdateNode *un) {
  if (!un) {
    // null UNode should be shortcutted here.
    expr_kidstack.push_back((const UpdateNode*)nullptr);
    expr_worklist.pop_back();
    return;
  }
  auto un_find = visited_un.find(un);
  if (un_find == visited_un.end()) {
    visited_un[un] = under_processing_un;
    expr_worklist.push_back(un->index.get());
    expr_worklist.push_back(un->value.get());
    expr_worklist.push_back(un->next);
  }
  else if (un_find->second == under_processing_un) {
    // kidstack from back to front: index, value, next
    Expr *index = popKidExpr();
    Expr *value = popKidExpr();
    const UpdateNode *next = popKidUNode();
    if (index != un->index.get() ||
        value != un->value.get() ||
        next != un->next) {
      // this UNode need to be changed.
      if (index == nullptr && value == nullptr) {
        // concrete UNode, need to be omitted
        visited_un[un] = next;
        expr_kidstack.push_back(next);
        un->dec();
      }
      else {
        // symbolic UNode, need new replacement
        UpdateNode *new_un =
          new UpdateNode(next, index, value, un->flags, un->kinst);
        new_un->resetRefCount(*un);
        visited_un[un] = new_un;
        expr_kidstack.push_back(new_un);
        // UpdateNode lifecycle management is complex. Don't touch it now.
        // TODO: avoid UpdateNode memory leak
        //delete un;
        if (next) {
          next->dec();
        }
        un->dec();
      }
    }
    else {
      // nothing changed, do nothing
      expr_kidstack.push_back(un);
    }
    expr_worklist.pop_back();
  }
  else {
    expr_kidstack.push_back(un_find->second);
    expr_worklist.pop_back();
  }
}