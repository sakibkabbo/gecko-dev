/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/FoldConstants.h"

#include "mozilla/FloatingPoint.h"

#include "jslibmath.h"
#include "jsnum.h"

#include "frontend/ParseNode.h"
#include "frontend/ParseNodeVisitor.h"
#include "frontend/Parser.h"
#include "js/Conversions.h"
#include "vm/StringType.h"

using namespace js;
using namespace js::frontend;

using JS::GenericNaN;
using JS::ToInt32;
using JS::ToUint32;
using mozilla::IsNaN;
using mozilla::IsNegative;
using mozilla::NegativeInfinity;
using mozilla::PositiveInfinity;

static bool ContainsHoistedDeclaration(JSContext* cx, ParseNode* node,
                                       bool* result);

static bool ListContainsHoistedDeclaration(JSContext* cx, ListNode* list,
                                           bool* result) {
  for (ParseNode* node : list->contents()) {
    if (!ContainsHoistedDeclaration(cx, node, result)) {
      return false;
    }
    if (*result) {
      return true;
    }
  }

  *result = false;
  return true;
}

// Determines whether the given ParseNode contains any declarations whose
// visibility will extend outside the node itself -- that is, whether the
// ParseNode contains any var statements.
//
// THIS IS NOT A GENERAL-PURPOSE FUNCTION.  It is only written to work in the
// specific context of deciding that |node|, as one arm of a ParseNodeKind::If
// controlled by a constant condition, contains a declaration that forbids
// |node| being completely eliminated as dead.
static bool ContainsHoistedDeclaration(JSContext* cx, ParseNode* node,
                                       bool* result) {
  if (!CheckRecursionLimit(cx)) {
    return false;
  }

restart:

  // With a better-typed AST, we would have distinct parse node classes for
  // expressions and for statements and would characterize expressions with
  // ExpressionKind and statements with StatementKind.  Perhaps someday.  In
  // the meantime we must characterize every ParseNodeKind, even the
  // expression/sub-expression ones that, if we handle all statement kinds
  // correctly, we'll never see.
  switch (node->getKind()) {
    // Base case.
    case ParseNodeKind::VarStmt:
      *result = true;
      return true;

    // Non-global lexical declarations are block-scoped (ergo not hoistable).
    case ParseNodeKind::LetDecl:
    case ParseNodeKind::ConstDecl:
      MOZ_ASSERT(node->is<ListNode>());
      *result = false;
      return true;

    // Similarly to the lexical declarations above, classes cannot add hoisted
    // declarations
    case ParseNodeKind::ClassDecl:
      MOZ_ASSERT(node->is<ClassNode>());
      *result = false;
      return true;

    // Function declarations *can* be hoisted declarations.  But in the
    // magical world of the rewritten frontend, the declaration necessitated
    // by a nested function statement, not at body level, doesn't require
    // that we preserve an unreachable function declaration node against
    // dead-code removal.
    case ParseNodeKind::Function:
      MOZ_ASSERT(node->is<CodeNode>());
      *result = false;
      return true;

    case ParseNodeKind::Module:
      *result = false;
      return true;

    // Statements with no sub-components at all.
    case ParseNodeKind::EmptyStmt:
      MOZ_ASSERT(node->is<NullaryNode>());
      *result = false;
      return true;

    case ParseNodeKind::DebuggerStmt:
      MOZ_ASSERT(node->is<DebuggerStatement>());
      *result = false;
      return true;

    // Statements containing only an expression have no declarations.
    case ParseNodeKind::ExpressionStmt:
    case ParseNodeKind::ThrowStmt:
    case ParseNodeKind::ReturnStmt:
      MOZ_ASSERT(node->is<UnaryNode>());
      *result = false;
      return true;

    // These two aren't statements in the spec, but we sometimes insert them
    // in statement lists anyway.
    case ParseNodeKind::InitialYield:
    case ParseNodeKind::YieldStarExpr:
    case ParseNodeKind::YieldExpr:
      MOZ_ASSERT(node->is<UnaryNode>());
      *result = false;
      return true;

    // Other statements with no sub-statement components.
    case ParseNodeKind::BreakStmt:
    case ParseNodeKind::ContinueStmt:
    case ParseNodeKind::ImportDecl:
    case ParseNodeKind::ImportSpecList:
    case ParseNodeKind::ImportSpec:
    case ParseNodeKind::ExportFromStmt:
    case ParseNodeKind::ExportDefaultStmt:
    case ParseNodeKind::ExportSpecList:
    case ParseNodeKind::ExportSpec:
    case ParseNodeKind::ExportStmt:
    case ParseNodeKind::ExportBatchSpecStmt:
    case ParseNodeKind::CallImportExpr:
      *result = false;
      return true;

    // Statements possibly containing hoistable declarations only in the left
    // half, in ParseNode terms -- the loop body in AST terms.
    case ParseNodeKind::DoWhileStmt:
      return ContainsHoistedDeclaration(cx, node->as<BinaryNode>().left(),
                                        result);

    // Statements possibly containing hoistable declarations only in the
    // right half, in ParseNode terms -- the loop body or nested statement
    // (usually a block statement), in AST terms.
    case ParseNodeKind::WhileStmt:
    case ParseNodeKind::WithStmt:
      return ContainsHoistedDeclaration(cx, node->as<BinaryNode>().right(),
                                        result);

    case ParseNodeKind::LabelStmt:
      return ContainsHoistedDeclaration(
          cx, node->as<LabeledStatement>().statement(), result);

    // Statements with more complicated structures.

    // if-statement nodes may have hoisted declarations in their consequent
    // and alternative components.
    case ParseNodeKind::IfStmt: {
      TernaryNode* ifNode = &node->as<TernaryNode>();
      ParseNode* consequent = ifNode->kid2();
      if (!ContainsHoistedDeclaration(cx, consequent, result)) {
        return false;
      }
      if (*result) {
        return true;
      }

      if ((node = ifNode->kid3())) {
        goto restart;
      }

      *result = false;
      return true;
    }

    // try-statements have statements to execute, and one or both of a
    // catch-list and a finally-block.
    case ParseNodeKind::TryStmt: {
      TernaryNode* tryNode = &node->as<TernaryNode>();

      MOZ_ASSERT(tryNode->kid2() || tryNode->kid3(),
                 "must have either catch or finally");

      ParseNode* tryBlock = tryNode->kid1();
      if (!ContainsHoistedDeclaration(cx, tryBlock, result)) {
        return false;
      }
      if (*result) {
        return true;
      }

      if (ParseNode* catchScope = tryNode->kid2()) {
        BinaryNode* catchNode =
            &catchScope->as<LexicalScopeNode>().scopeBody()->as<BinaryNode>();
        MOZ_ASSERT(catchNode->isKind(ParseNodeKind::Catch));

        ParseNode* catchStatements = catchNode->right();
        if (!ContainsHoistedDeclaration(cx, catchStatements, result)) {
          return false;
        }
        if (*result) {
          return true;
        }
      }

      if (ParseNode* finallyBlock = tryNode->kid3()) {
        return ContainsHoistedDeclaration(cx, finallyBlock, result);
      }

      *result = false;
      return true;
    }

    // A switch node's left half is an expression; only its right half (a
    // list of cases/defaults, or a block node) could contain hoisted
    // declarations.
    case ParseNodeKind::SwitchStmt: {
      SwitchStatement* switchNode = &node->as<SwitchStatement>();
      return ContainsHoistedDeclaration(cx, &switchNode->lexicalForCaseList(),
                                        result);
    }

    case ParseNodeKind::Case: {
      CaseClause* caseClause = &node->as<CaseClause>();
      return ContainsHoistedDeclaration(cx, caseClause->statementList(),
                                        result);
    }

    case ParseNodeKind::ForStmt: {
      ForNode* forNode = &node->as<ForNode>();
      TernaryNode* loopHead = forNode->head();
      MOZ_ASSERT(loopHead->isKind(ParseNodeKind::ForHead) ||
                 loopHead->isKind(ParseNodeKind::ForIn) ||
                 loopHead->isKind(ParseNodeKind::ForOf));

      if (loopHead->isKind(ParseNodeKind::ForHead)) {
        // for (init?; cond?; update?), with only init possibly containing
        // a hoisted declaration.  (Note: a lexical-declaration |init| is
        // (at present) hoisted in SpiderMonkey parlance -- but such
        // hoisting doesn't extend outside of this statement, so it is not
        // hoisting in the sense meant by ContainsHoistedDeclaration.)
        ParseNode* init = loopHead->kid1();
        if (init && init->isKind(ParseNodeKind::VarStmt)) {
          *result = true;
          return true;
        }
      } else {
        MOZ_ASSERT(loopHead->isKind(ParseNodeKind::ForIn) ||
                   loopHead->isKind(ParseNodeKind::ForOf));

        // for each? (target in ...), where only target may introduce
        // hoisted declarations.
        //
        //   -- or --
        //
        // for (target of ...), where only target may introduce hoisted
        // declarations.
        //
        // Either way, if |target| contains a declaration, it's |loopHead|'s
        // first kid.
        ParseNode* decl = loopHead->kid1();
        if (decl && decl->isKind(ParseNodeKind::VarStmt)) {
          *result = true;
          return true;
        }
      }

      ParseNode* loopBody = forNode->body();
      return ContainsHoistedDeclaration(cx, loopBody, result);
    }

    case ParseNodeKind::LexicalScope: {
      LexicalScopeNode* scope = &node->as<LexicalScopeNode>();
      ParseNode* expr = scope->scopeBody();

      if (expr->isKind(ParseNodeKind::ForStmt) ||
          expr->isKind(ParseNodeKind::Function)) {
        return ContainsHoistedDeclaration(cx, expr, result);
      }

      MOZ_ASSERT(expr->isKind(ParseNodeKind::StatementList));
      return ListContainsHoistedDeclaration(
          cx, &scope->scopeBody()->as<ListNode>(), result);
    }

    // List nodes with all non-null children.
    case ParseNodeKind::StatementList:
      return ListContainsHoistedDeclaration(cx, &node->as<ListNode>(), result);

    // Grammar sub-components that should never be reached directly by this
    // method, because some parent component should have asserted itself.
    case ParseNodeKind::ObjectPropertyName:
    case ParseNodeKind::ComputedName:
    case ParseNodeKind::Spread:
    case ParseNodeKind::MutateProto:
    case ParseNodeKind::Colon:
    case ParseNodeKind::Shorthand:
    case ParseNodeKind::ConditionalExpr:
    case ParseNodeKind::TypeOfNameExpr:
    case ParseNodeKind::TypeOfExpr:
    case ParseNodeKind::AwaitExpr:
    case ParseNodeKind::VoidExpr:
    case ParseNodeKind::NotExpr:
    case ParseNodeKind::BitNotExpr:
    case ParseNodeKind::DeleteNameExpr:
    case ParseNodeKind::DeletePropExpr:
    case ParseNodeKind::DeleteElemExpr:
    case ParseNodeKind::DeleteExpr:
    case ParseNodeKind::PosExpr:
    case ParseNodeKind::NegExpr:
    case ParseNodeKind::PreIncrementExpr:
    case ParseNodeKind::PostIncrementExpr:
    case ParseNodeKind::PreDecrementExpr:
    case ParseNodeKind::PostDecrementExpr:
    case ParseNodeKind::OrExpr:
    case ParseNodeKind::AndExpr:
    case ParseNodeKind::BitOrExpr:
    case ParseNodeKind::BitXorExpr:
    case ParseNodeKind::BitAndExpr:
    case ParseNodeKind::StrictEqExpr:
    case ParseNodeKind::EqExpr:
    case ParseNodeKind::StrictNeExpr:
    case ParseNodeKind::NeExpr:
    case ParseNodeKind::LtExpr:
    case ParseNodeKind::LeExpr:
    case ParseNodeKind::GtExpr:
    case ParseNodeKind::GeExpr:
    case ParseNodeKind::InstanceOfExpr:
    case ParseNodeKind::InExpr:
    case ParseNodeKind::LshExpr:
    case ParseNodeKind::RshExpr:
    case ParseNodeKind::UrshExpr:
    case ParseNodeKind::AddExpr:
    case ParseNodeKind::SubExpr:
    case ParseNodeKind::MulExpr:
    case ParseNodeKind::DivExpr:
    case ParseNodeKind::ModExpr:
    case ParseNodeKind::PowExpr:
    case ParseNodeKind::AssignExpr:
    case ParseNodeKind::AddAssignExpr:
    case ParseNodeKind::SubAssignExpr:
    case ParseNodeKind::BitOrAssignExpr:
    case ParseNodeKind::BitXorAssignExpr:
    case ParseNodeKind::BitAndAssignExpr:
    case ParseNodeKind::LshAssignExpr:
    case ParseNodeKind::RshAssignExpr:
    case ParseNodeKind::UrshAssignExpr:
    case ParseNodeKind::MulAssignExpr:
    case ParseNodeKind::DivAssignExpr:
    case ParseNodeKind::ModAssignExpr:
    case ParseNodeKind::PowAssignExpr:
    case ParseNodeKind::CommaExpr:
    case ParseNodeKind::ArrayExpr:
    case ParseNodeKind::ObjectExpr:
    case ParseNodeKind::PropertyNameExpr:
    case ParseNodeKind::DotExpr:
    case ParseNodeKind::ElemExpr:
    case ParseNodeKind::Arguments:
    case ParseNodeKind::CallExpr:
    case ParseNodeKind::Name:
    case ParseNodeKind::PrivateName:
    case ParseNodeKind::TemplateStringExpr:
    case ParseNodeKind::TemplateStringListExpr:
    case ParseNodeKind::TaggedTemplateExpr:
    case ParseNodeKind::CallSiteObjExpr:
    case ParseNodeKind::StringExpr:
    case ParseNodeKind::RegExpExpr:
    case ParseNodeKind::TrueExpr:
    case ParseNodeKind::FalseExpr:
    case ParseNodeKind::NullExpr:
    case ParseNodeKind::RawUndefinedExpr:
    case ParseNodeKind::ThisExpr:
    case ParseNodeKind::Elision:
    case ParseNodeKind::NumberExpr:
#ifdef ENABLE_BIGINT
    case ParseNodeKind::BigIntExpr:
#endif
    case ParseNodeKind::NewExpr:
    case ParseNodeKind::Generator:
    case ParseNodeKind::ParamsBody:
    case ParseNodeKind::Catch:
    case ParseNodeKind::ForIn:
    case ParseNodeKind::ForOf:
    case ParseNodeKind::ForHead:
    case ParseNodeKind::ClassMethod:
    case ParseNodeKind::ClassField:
    case ParseNodeKind::ClassMemberList:
    case ParseNodeKind::ClassNames:
    case ParseNodeKind::NewTargetExpr:
    case ParseNodeKind::ImportMetaExpr:
    case ParseNodeKind::PosHolder:
    case ParseNodeKind::SuperCallExpr:
    case ParseNodeKind::SuperBase:
    case ParseNodeKind::SetThis:
      MOZ_CRASH(
          "ContainsHoistedDeclaration should have indicated false on "
          "some parent node without recurring to test this node");

    case ParseNodeKind::PipelineExpr:
      MOZ_ASSERT(node->is<ListNode>());
      *result = false;
      return true;

    case ParseNodeKind::Limit:  // invalid sentinel value
      MOZ_CRASH("unexpected ParseNodeKind::Limit in node");
  }

  MOZ_CRASH("invalid node kind");
}

/*
 * Fold from one constant type to another.
 * XXX handles only strings and numbers for now
 */
static bool FoldType(JSContext* cx, ParseNode* pn, ParseNodeKind kind) {
  if (!pn->isKind(kind)) {
    switch (kind) {
      case ParseNodeKind::NumberExpr:
        if (pn->isKind(ParseNodeKind::StringExpr)) {
          double d;
          if (!StringToNumber(cx, pn->as<NameNode>().atom(), &d)) {
            return false;
          }
          pn->setKind(ParseNodeKind::NumberExpr);
          pn->setOp(JSOP_DOUBLE);
          pn->as<NumericLiteral>().setValue(d);
          pn->as<NumericLiteral>().setDecimalPoint(NoDecimal);
        }
        break;

      case ParseNodeKind::StringExpr:
        if (pn->isKind(ParseNodeKind::NumberExpr)) {
          JSAtom* atom = NumberToAtom(cx, pn->as<NumericLiteral>().value());
          if (!atom) {
            return false;
          }
          pn->setKind(ParseNodeKind::StringExpr);
          pn->setOp(JSOP_STRING);
          pn->as<NameNode>().setAtom(atom);
          pn->as<NameNode>().setInitializer(nullptr);
        }
        break;

      default:
        MOZ_CRASH("Invalid type in constant folding FoldType");
    }
  }
  return true;
}

static bool IsEffectless(ParseNode* node) {
  return node->isKind(ParseNodeKind::TrueExpr) ||
         node->isKind(ParseNodeKind::FalseExpr) ||
         node->isKind(ParseNodeKind::StringExpr) ||
         node->isKind(ParseNodeKind::TemplateStringExpr) ||
         node->isKind(ParseNodeKind::NumberExpr) ||
#ifdef ENABLE_BIGINT
         node->isKind(ParseNodeKind::BigIntExpr) ||
#endif
         node->isKind(ParseNodeKind::NullExpr) ||
         node->isKind(ParseNodeKind::RawUndefinedExpr) ||
         node->isKind(ParseNodeKind::Function);
}

enum Truthiness { Truthy, Falsy, Unknown };

static Truthiness Boolish(ParseNode* pn) {
  switch (pn->getKind()) {
    case ParseNodeKind::NumberExpr:
      return (pn->as<NumericLiteral>().value() != 0 &&
              !IsNaN(pn->as<NumericLiteral>().value()))
                 ? Truthy
                 : Falsy;

#ifdef ENABLE_BIGINT
    case ParseNodeKind::BigIntExpr:
      return (pn->as<BigIntLiteral>().box()->value()->toBoolean()) ? Truthy
                                                                   : Falsy;
#endif

    case ParseNodeKind::StringExpr:
    case ParseNodeKind::TemplateStringExpr:
      return (pn->as<NameNode>().atom()->length() > 0) ? Truthy : Falsy;

    case ParseNodeKind::TrueExpr:
    case ParseNodeKind::Function:
      return Truthy;

    case ParseNodeKind::FalseExpr:
    case ParseNodeKind::NullExpr:
    case ParseNodeKind::RawUndefinedExpr:
      return Falsy;

    case ParseNodeKind::VoidExpr: {
      // |void <foo>| evaluates to |undefined| which isn't truthy.  But the
      // sense of this method requires that the expression be literally
      // replaceable with true/false: not the case if the nested expression
      // is effectful, might throw, &c.  Walk past the |void| (and nested
      // |void| expressions, for good measure) and check that the nested
      // expression doesn't break this requirement before indicating falsity.
      do {
        pn = pn->as<UnaryNode>().kid();
      } while (pn->isKind(ParseNodeKind::VoidExpr));

      return IsEffectless(pn) ? Falsy : Unknown;
    }

    default:
      return Unknown;
  }
}

static bool SimplifyCondition(JSContext* cx, ParseNode** nodePtr) {
  // Conditions fold like any other expression, but then they sometimes can be
  // further folded to constants. *nodePtr should already have been
  // constant-folded.

  ParseNode* node = *nodePtr;
  Truthiness t = Boolish(node);
  if (t != Unknown) {
    // We can turn function nodes into constant nodes here, but mutating
    // function nodes is tricky --- in particular, mutating a function node
    // that appears on a method list corrupts the method list. However,
    // methods are M's in statements of the form 'this.foo = M;', which we
    // never fold, so we're okay.
    if (t == Truthy) {
      node->setKind(ParseNodeKind::TrueExpr);
      node->setOp(JSOP_TRUE);
    } else {
      node->setKind(ParseNodeKind::FalseExpr);
      node->setOp(JSOP_FALSE);
    }
  }

  return true;
}

static bool FoldTypeOfExpr(JSContext* cx, UnaryNode* node) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::TypeOfExpr));

  ParseNode* expr = node->kid();

  // Constant-fold the entire |typeof| if given a constant with known type.
  RootedPropertyName result(cx);
  if (expr->isKind(ParseNodeKind::StringExpr) ||
      expr->isKind(ParseNodeKind::TemplateStringExpr)) {
    result = cx->names().string;
  } else if (expr->isKind(ParseNodeKind::NumberExpr)) {
    result = cx->names().number;
  }
#ifdef ENABLE_BIGINT
  else if (expr->isKind(ParseNodeKind::BigIntExpr)) {
    result = cx->names().bigint;
  }
#endif
  else if (expr->isKind(ParseNodeKind::NullExpr)) {
    result = cx->names().object;
  } else if (expr->isKind(ParseNodeKind::TrueExpr) ||
             expr->isKind(ParseNodeKind::FalseExpr)) {
    result = cx->names().boolean;
  } else if (expr->isKind(ParseNodeKind::Function)) {
    result = cx->names().function;
  }

  if (result) {
    node->setKind(ParseNodeKind::StringExpr);
    node->setOp(JSOP_NOP);
    node->as<NameNode>().setAtom(result);
    node->as<NameNode>().setInitializer(nullptr);
  }

  return true;
}

static bool FoldDeleteExpr(JSContext* cx, UnaryNode* node) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteExpr));
  ParseNode* expr = node->kid();

  // Expression deletion evaluates the expression, then evaluates to true.
  // For effectless expressions, eliminate the expression evaluation.
  if (IsEffectless(expr)) {
    node->setKind(ParseNodeKind::TrueExpr);
    node->setOp(JSOP_TRUE);
  }

  return true;
}

static bool FoldDeleteElement(JSContext* cx, UnaryNode* node) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteElemExpr));
  ParseNode* expr = node->kid();

  // If we're deleting an element, but constant-folding converted our
  // element reference into a dotted property access, we must *also*
  // morph the node's kind.
  //
  // In principle this also applies to |super["foo"] -> super.foo|,
  // but we don't constant-fold |super["foo"]| yet.
  MOZ_ASSERT(expr->isKind(ParseNodeKind::ElemExpr) ||
             expr->isKind(ParseNodeKind::DotExpr));
  if (expr->isKind(ParseNodeKind::DotExpr)) {
    node->setKind(ParseNodeKind::DeletePropExpr);
  }

  return true;
}

static bool FoldNot(JSContext* cx, UnaryNode* node) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::NotExpr));

  if (!SimplifyCondition(cx, node->unsafeKidReference())) {
    return false;
  }

  ParseNode* expr = node->kid();

  if (expr->isKind(ParseNodeKind::TrueExpr) ||
      expr->isKind(ParseNodeKind::FalseExpr)) {
    bool newval = !expr->isKind(ParseNodeKind::TrueExpr);

    node->setKind(newval ? ParseNodeKind::TrueExpr : ParseNodeKind::FalseExpr);
    node->setOp(newval ? JSOP_TRUE : JSOP_FALSE);
  }

  return true;
}

static bool FoldUnaryArithmetic(JSContext* cx, UnaryNode* node) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::BitNotExpr) ||
                 node->isKind(ParseNodeKind::PosExpr) ||
                 node->isKind(ParseNodeKind::NegExpr),
             "need a different method for this node kind");

  ParseNode* expr = node->kid();

  if (expr->isKind(ParseNodeKind::NumberExpr) ||
      expr->isKind(ParseNodeKind::TrueExpr) ||
      expr->isKind(ParseNodeKind::FalseExpr)) {
    double d = expr->isKind(ParseNodeKind::NumberExpr)
                   ? expr->as<NumericLiteral>().value()
                   : double(expr->isKind(ParseNodeKind::TrueExpr));

    if (node->isKind(ParseNodeKind::BitNotExpr)) {
      d = ~ToInt32(d);
    } else if (node->isKind(ParseNodeKind::NegExpr)) {
      d = -d;
    } else {
      MOZ_ASSERT(node->isKind(ParseNodeKind::PosExpr));  // nothing to do
    }

    node->setKind(ParseNodeKind::NumberExpr);
    node->setOp(JSOP_DOUBLE);
    node->as<NumericLiteral>().setValue(d);
    node->as<NumericLiteral>().setDecimalPoint(NoDecimal);
  }

  return true;
}

static bool FoldAndOr(JSContext* cx, ParseNode** nodePtr) {
  ListNode* node = &(*nodePtr)->as<ListNode>();

  MOZ_ASSERT(node->isKind(ParseNodeKind::AndExpr) ||
             node->isKind(ParseNodeKind::OrExpr));

  bool isOrNode = node->isKind(ParseNodeKind::OrExpr);
  ParseNode** elem = node->unsafeHeadReference();
  do {
    Truthiness t = Boolish(*elem);

    // If we don't know the constant-folded node's truthiness, we can't
    // reduce this node with its surroundings.  Continue folding any
    // remaining nodes.
    if (t == Unknown) {
      elem = &(*elem)->pn_next;
      continue;
    }

    // If the constant-folded node's truthiness will terminate the
    // condition -- `a || true || expr` or |b && false && expr| -- then
    // trailing nodes will never be evaluated.  Truncate the list after
    // the known-truthiness node, as it's the overall result.
    if ((t == Truthy) == isOrNode) {
      for (ParseNode* next = (*elem)->pn_next; next; next = next->pn_next) {
        node->unsafeDecrementCount();
      }

      // Terminate the original and/or list at the known-truthiness
      // node.
      (*elem)->pn_next = nullptr;
      elem = &(*elem)->pn_next;
      break;
    }

    MOZ_ASSERT((t == Truthy) == !isOrNode);

    // We've encountered a vacuous node that'll never short-circuit
    // evaluation.
    if ((*elem)->pn_next) {
      // This node is never the overall result when there are
      // subsequent nodes.  Remove it.
      ParseNode* elt = *elem;
      *elem = elt->pn_next;
      node->unsafeDecrementCount();
    } else {
      // Otherwise this node is the result of the overall expression,
      // so leave it alone.  And we're done.
      elem = &(*elem)->pn_next;
      break;
    }
  } while (*elem);

  node->unsafeReplaceTail(elem);

  // If we removed nodes, we may have to replace a one-element list with
  // its element.
  if (node->count() == 1) {
    ParseNode* first = node->head();
    ReplaceNode(nodePtr, first);
  }

  return true;
}

static bool Fold(JSContext* cx, ParseNode** pnp);

static bool FoldConditional(JSContext* cx, ParseNode** nodePtr) {
  ParseNode** nextNode = nodePtr;

  do {
    // |nextNode| on entry points to the C?T:F expression to be folded.
    // Reset it to exit the loop in the common case where F isn't another
    // ?: expression.
    nodePtr = nextNode;
    nextNode = nullptr;

    TernaryNode* node = &(*nodePtr)->as<TernaryNode>();
    MOZ_ASSERT(node->isKind(ParseNodeKind::ConditionalExpr));

    ParseNode** expr = node->unsafeKid1Reference();
    if (!Fold(cx, expr)) {
      return false;
    }
    if (!SimplifyCondition(cx, expr)) {
      return false;
    }

    ParseNode** ifTruthy = node->unsafeKid2Reference();
    if (!Fold(cx, ifTruthy)) {
      return false;
    }

    ParseNode** ifFalsy = node->unsafeKid3Reference();

    // If our C?T:F node has F as another ?: node, *iteratively* constant-
    // fold F *after* folding C and T (and possibly eliminating C and one
    // of T/F entirely); otherwise fold F normally.  Making |nextNode| non-
    // null causes this loop to run again to fold F.
    //
    // Conceivably we could instead/also iteratively constant-fold T, if T
    // were more complex than F.  Such an optimization is unimplemented.
    if ((*ifFalsy)->isKind(ParseNodeKind::ConditionalExpr)) {
      MOZ_ASSERT((*ifFalsy)->is<TernaryNode>());
      nextNode = ifFalsy;
    } else {
      if (!Fold(cx, ifFalsy)) {
        return false;
      }
    }

    // Try to constant-fold based on the condition expression.
    Truthiness t = Boolish(*expr);
    if (t == Unknown) {
      continue;
    }

    // Otherwise reduce 'C ? T : F' to T or F as directed by C.
    ParseNode* replacement = t == Truthy ? *ifTruthy : *ifFalsy;

    // Otherwise perform a replacement.  This invalidates |nextNode|, so
    // reset it (if the replacement requires folding) or clear it (if
    // |ifFalsy| is dead code) as needed.
    if (nextNode) {
      nextNode = (*nextNode == replacement) ? nodePtr : nullptr;
    }
    ReplaceNode(nodePtr, replacement);
  } while (nextNode);

  return true;
}

static bool FoldIf(JSContext* cx, ParseNode** nodePtr) {
  ParseNode** nextNode = nodePtr;

  do {
    // |nextNode| on entry points to the initial |if| to be folded.  Reset
    // it to exit the loop when the |else| arm isn't another |if|.
    nodePtr = nextNode;
    nextNode = nullptr;

    TernaryNode* node = &(*nodePtr)->as<TernaryNode>();
    MOZ_ASSERT(node->isKind(ParseNodeKind::IfStmt));

    ParseNode** expr = node->unsafeKid1Reference();
    if (!Fold(cx, expr)) {
      return false;
    }
    if (!SimplifyCondition(cx, expr)) {
      return false;
    }

    ParseNode** consequent = node->unsafeKid2Reference();
    if (!Fold(cx, consequent)) {
      return false;
    }

    ParseNode** alternative = node->unsafeKid3Reference();
    if (*alternative) {
      // If in |if (C) T; else F;| we have |F| as another |if|,
      // *iteratively* constant-fold |F| *after* folding |C| and |T| (and
      // possibly completely replacing the whole thing with |T| or |F|);
      // otherwise fold F normally.  Making |nextNode| non-null causes
      // this loop to run again to fold F.
      if ((*alternative)->isKind(ParseNodeKind::IfStmt)) {
        MOZ_ASSERT((*alternative)->is<TernaryNode>());
        nextNode = alternative;
      } else {
        if (!Fold(cx, alternative)) {
          return false;
        }
      }
    }

    // Eliminate the consequent or alternative if the condition has
    // constant truthiness.
    Truthiness t = Boolish(*expr);
    if (t == Unknown) {
      continue;
    }

    // Careful!  Either of these can be null: |replacement| in |if (0) T;|,
    // and |discarded| in |if (true) T;|.
    ParseNode* replacement;
    ParseNode* discarded;
    if (t == Truthy) {
      replacement = *consequent;
      discarded = *alternative;
    } else {
      replacement = *alternative;
      discarded = *consequent;
    }

    bool performReplacement = true;
    if (discarded) {
      // A declaration that hoists outside the discarded arm prevents the
      // |if| from being folded away.
      bool containsHoistedDecls;
      if (!ContainsHoistedDeclaration(cx, discarded, &containsHoistedDecls)) {
        return false;
      }

      performReplacement = !containsHoistedDecls;
    }

    if (!performReplacement) {
      continue;
    }

    if (!replacement) {
      // If there's no replacement node, we have a constantly-false |if|
      // with no |else|.  Replace the entire thing with an empty
      // statement list.
      node->setKind(ParseNodeKind::StatementList);
      node->as<ListNode>().makeEmpty();
    } else {
      // Replacement invalidates |nextNode|, so reset it (if the
      // replacement requires folding) or clear it (if |alternative|
      // is dead code) as needed.
      if (nextNode) {
        nextNode = (*nextNode == replacement) ? nodePtr : nullptr;
      }
      ReplaceNode(nodePtr, replacement);
    }
  } while (nextNode);

  return true;
}

static double ComputeBinary(ParseNodeKind kind, double left, double right) {
  if (kind == ParseNodeKind::AddExpr) {
    return left + right;
  }

  if (kind == ParseNodeKind::SubExpr) {
    return left - right;
  }

  if (kind == ParseNodeKind::MulExpr) {
    return left * right;
  }

  if (kind == ParseNodeKind::ModExpr) {
    return NumberMod(left, right);
  }

  if (kind == ParseNodeKind::UrshExpr) {
    return ToUint32(left) >> (ToUint32(right) & 31);
  }

  if (kind == ParseNodeKind::DivExpr) {
    return NumberDiv(left, right);
  }

  MOZ_ASSERT(kind == ParseNodeKind::LshExpr || kind == ParseNodeKind::RshExpr);

  int32_t i = ToInt32(left);
  uint32_t j = ToUint32(right) & 31;
  return int32_t((kind == ParseNodeKind::LshExpr) ? uint32_t(i) << j : i >> j);
}

static bool FoldBinaryArithmetic(JSContext* cx, ListNode* node) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::SubExpr) ||
             node->isKind(ParseNodeKind::MulExpr) ||
             node->isKind(ParseNodeKind::LshExpr) ||
             node->isKind(ParseNodeKind::RshExpr) ||
             node->isKind(ParseNodeKind::UrshExpr) ||
             node->isKind(ParseNodeKind::DivExpr) ||
             node->isKind(ParseNodeKind::ModExpr));
  MOZ_ASSERT(node->count() >= 2);

  // Fold each operand to a number if possible.
  ParseNode** listp = node->unsafeHeadReference();
  for (; *listp; listp = &(*listp)->pn_next) {
    if (!FoldType(cx, *listp, ParseNodeKind::NumberExpr)) {
      return false;
    }
  }
  node->unsafeReplaceTail(listp);

  // Now fold all leading numeric terms together into a single number.
  // (Trailing terms for the non-shift operations can't be folded together
  // due to floating point imprecision.  For example, if |x === -2**53|,
  // |x - 1 - 1 === -2**53| but |x - 2 === -2**53 - 2|.  Shifts could be
  // folded, but it doesn't seem worth the effort.)
  ParseNode* elem = node->head();
  ParseNode* next = elem->pn_next;
  if (elem->isKind(ParseNodeKind::NumberExpr)) {
    ParseNodeKind kind = node->getKind();
    while (true) {
      if (!next || !next->isKind(ParseNodeKind::NumberExpr)) {
        break;
      }

      double d = ComputeBinary(kind, elem->as<NumericLiteral>().value(),
                               next->as<NumericLiteral>().value());

      next = next->pn_next;
      elem->pn_next = next;

      elem->setKind(ParseNodeKind::NumberExpr);
      elem->setOp(JSOP_DOUBLE);
      elem->as<NumericLiteral>().setValue(d);
      elem->as<NumericLiteral>().setDecimalPoint(NoDecimal);

      node->unsafeDecrementCount();
    }

    if (node->count() == 1) {
      MOZ_ASSERT(node->head() == elem);
      MOZ_ASSERT(elem->isKind(ParseNodeKind::NumberExpr));

      double d = elem->as<NumericLiteral>().value();
      node->setKind(ParseNodeKind::NumberExpr);
      node->setOp(JSOP_DOUBLE);
      node->as<NumericLiteral>().setValue(d);
      node->as<NumericLiteral>().setDecimalPoint(NoDecimal);
    }
  }

  return true;
}

static bool FoldExponentiation(JSContext* cx, ListNode* node) {
  MOZ_ASSERT(node->isKind(ParseNodeKind::PowExpr));
  MOZ_ASSERT(node->count() >= 2);

  // Fold each operand, ideally into a number.
  ParseNode** listp = node->unsafeHeadReference();
  for (; *listp; listp = &(*listp)->pn_next) {
    if (!FoldType(cx, *listp, ParseNodeKind::NumberExpr)) {
      return false;
    }
  }

  node->unsafeReplaceTail(listp);

  // Unlike all other binary arithmetic operators, ** is right-associative:
  // 2**3**5 is 2**(3**5), not (2**3)**5.  As list nodes singly-link their
  // children, full constant-folding requires either linear space or dodgy
  // in-place linked list reversal.  So we only fold one exponentiation: it's
  // easy and addresses common cases like |2**32|.
  if (node->count() > 2) {
    return true;
  }

  ParseNode* base = node->head();
  ParseNode* exponent = base->pn_next;
  if (!base->isKind(ParseNodeKind::NumberExpr) ||
      !exponent->isKind(ParseNodeKind::NumberExpr)) {
    return true;
  }

  double d1 = base->as<NumericLiteral>().value();
  double d2 = exponent->as<NumericLiteral>().value();

  node->setKind(ParseNodeKind::NumberExpr);
  node->setOp(JSOP_DOUBLE);
  node->as<NumericLiteral>().setValue(ecmaPow(d1, d2));
  node->as<NumericLiteral>().setDecimalPoint(NoDecimal);
  return true;
}

static bool FoldElement(JSContext* cx, ParseNode** nodePtr) {
  PropertyByValue* elem = &(*nodePtr)->as<PropertyByValue>();

  ParseNode* expr = &elem->expression();
  ParseNode* key = &elem->key();
  PropertyName* name = nullptr;
  if (key->isKind(ParseNodeKind::StringExpr)) {
    JSAtom* atom = key->as<NameNode>().atom();
    uint32_t index;

    if (atom->isIndex(&index)) {
      // Optimization 1: We have something like expr["100"]. This is
      // equivalent to expr[100] which is faster.
      key->setKind(ParseNodeKind::NumberExpr);
      key->setOp(JSOP_DOUBLE);
      key->as<NumericLiteral>().setValue(index);
      key->as<NumericLiteral>().setDecimalPoint(NoDecimal);
    } else {
      name = atom->asPropertyName();
    }
  } else if (key->isKind(ParseNodeKind::NumberExpr)) {
    double number = key->as<NumericLiteral>().value();
    if (number != ToUint32(number)) {
      // Optimization 2: We have something like expr[3.14]. The number
      // isn't an array index, so it converts to a string ("3.14"),
      // enabling optimization 3 below.
      JSAtom* atom = NumberToAtom(cx, number);
      if (!atom) {
        return false;
      }
      name = atom->asPropertyName();
    }
  }

  // If we don't have a name, we can't optimize to getprop.
  if (!name) {
    return true;
  }

  // Optimization 3: We have expr["foo"] where foo is not an index.  Convert
  // to a property access (like expr.foo) that optimizes better downstream.
  key->setKind(ParseNodeKind::PropertyNameExpr);
  key->setOp(JSOP_NOP);
  key->as<NameNode>().setAtom(name);
  key->as<NameNode>().setInitializer(nullptr);

  (*nodePtr)->setKind(ParseNodeKind::DotExpr);
  (*nodePtr)->setOp(JSOP_NOP);
  *(*nodePtr)->as<PropertyAccess>().unsafeLeftReference() = expr;
  *(*nodePtr)->as<PropertyAccess>().unsafeRightReference() = key;
  (*nodePtr)->as<PropertyAccess>().setInParens(elem->isInParens());

  return true;
}

static bool FoldAdd(JSContext* cx, ParseNode** nodePtr) {
  ListNode* node = &(*nodePtr)->as<ListNode>();

  MOZ_ASSERT(node->isKind(ParseNodeKind::AddExpr));
  MOZ_ASSERT(node->count() >= 2);

  // Fold leading numeric operands together:
  //
  //   (1 + 2 + x)  becomes  (3 + x)
  //
  // Don't go past the leading operands: additions after a string are
  // string concatenations, not additions: ("1" + 2 + 3 === "123").
  ParseNode* current = node->head();
  ParseNode* next = current->pn_next;
  if (current->isKind(ParseNodeKind::NumberExpr)) {
    do {
      if (!next->isKind(ParseNodeKind::NumberExpr)) {
        break;
      }

      NumericLiteral* num = &current->as<NumericLiteral>();

      num->setValue(num->value() + next->as<NumericLiteral>().value());
      current->pn_next = next->pn_next;
      next = current->pn_next;

      node->unsafeDecrementCount();
    } while (next);
  }

  // If any operands remain, attempt string concatenation folding.
  do {
    // If no operands remain, we're done.
    if (!next) {
      break;
    }

    // (number + string) is string concatenation *only* at the start of
    // the list: (x + 1 + "2" !== x + "12") when x is a number.
    if (current->isKind(ParseNodeKind::NumberExpr) &&
        next->isKind(ParseNodeKind::StringExpr)) {
      if (!FoldType(cx, current, ParseNodeKind::StringExpr)) {
        return false;
      }
      next = current->pn_next;
    }

    // The first string forces all subsequent additions to be
    // string concatenations.
    do {
      if (current->isKind(ParseNodeKind::StringExpr)) {
        break;
      }

      current = next;
      next = next->pn_next;
    } while (next);

    // If there's nothing left to fold, we're done.
    if (!next) {
      break;
    }

    RootedString combination(cx);
    RootedString tmp(cx);
    do {
      // Create a rope of the current string and all succeeding
      // constants that we can convert to strings, then atomize it
      // and replace them all with that fresh string.
      MOZ_ASSERT(current->isKind(ParseNodeKind::StringExpr));

      combination = current->as<NameNode>().atom();

      do {
        // Try folding the next operand to a string.
        if (!FoldType(cx, next, ParseNodeKind::StringExpr)) {
          return false;
        }

        // Stop glomming once folding doesn't produce a string.
        if (!next->isKind(ParseNodeKind::StringExpr)) {
          break;
        }

        // Add this string to the combination and remove the node.
        tmp = next->as<NameNode>().atom();
        combination = ConcatStrings<CanGC>(cx, combination, tmp);
        if (!combination) {
          return false;
        }

        next = next->pn_next;
        current->pn_next = next;

        node->unsafeDecrementCount();
      } while (next);

      // Replace |current|'s string with the entire combination.
      MOZ_ASSERT(current->isKind(ParseNodeKind::StringExpr));
      combination = AtomizeString(cx, combination);
      if (!combination) {
        return false;
      }
      current->as<NameNode>().setAtom(&combination->asAtom());

      // If we're out of nodes, we're done.
      if (!next) {
        break;
      }

      current = next;
      next = current->pn_next;

      // If we're out of nodes *after* the non-foldable-to-string
      // node, we're done.
      if (!next) {
        break;
      }

      // Otherwise find the next node foldable to a string, and loop.
      do {
        current = next;
        next = current->pn_next;

        if (!FoldType(cx, current, ParseNodeKind::StringExpr)) {
          return false;
        }
        next = current->pn_next;
      } while (!current->isKind(ParseNodeKind::StringExpr) && next);
    } while (next);
  } while (false);

  MOZ_ASSERT(!next, "must have considered all nodes here");
  MOZ_ASSERT(!current->pn_next, "current node must be the last node");

  node->unsafeReplaceTail(&current->pn_next);

  if (node->count() == 1) {
    // We reduced the list to a constant.  Replace the ParseNodeKind::Add node
    // with that constant.
    ReplaceNode(nodePtr, current);
  }

  return true;
}

class FoldVisitor : public ParseNodeVisitor<FoldVisitor> {
  using Base = ParseNodeVisitor;

 public:
  explicit FoldVisitor(JSContext* cx) : ParseNodeVisitor(cx) {}

  bool visitElemExpr(ParseNode*& pn) {
    return Base::visitElemExpr(pn) && FoldElement(cx, &pn);
  }

  bool visitTypeOfExpr(ParseNode*& pn) {
    return Base::visitTypeOfExpr(pn) &&
           FoldTypeOfExpr(cx, &pn->as<UnaryNode>());
  }

  bool visitDeleteExpr(ParseNode*& pn) {
    return Base::visitDeleteExpr(pn) &&
           FoldDeleteExpr(cx, &pn->as<UnaryNode>());
  }

  bool visitDeleteElemExpr(ParseNode*& pn) {
    return Base::visitDeleteElemExpr(pn) &&
           FoldDeleteElement(cx, &pn->as<UnaryNode>());
  }

  bool visitNotExpr(ParseNode*& pn) {
    return Base::visitNotExpr(pn) && FoldNot(cx, &pn->as<UnaryNode>());
  }

  bool visitBitNotExpr(ParseNode*& pn) {
    return Base::visitBitNotExpr(pn) &&
           FoldUnaryArithmetic(cx, &pn->as<UnaryNode>());
  }

  bool visitPosExpr(ParseNode*& pn) {
    return Base::visitPosExpr(pn) &&
           FoldUnaryArithmetic(cx, &pn->as<UnaryNode>());
  }

  bool visitNegExpr(ParseNode*& pn) {
    return Base::visitNegExpr(pn) &&
           FoldUnaryArithmetic(cx, &pn->as<UnaryNode>());
  }

  bool visitPowExpr(ParseNode*& pn) {
    return Base::visitPowExpr(pn) &&
           FoldExponentiation(cx, &pn->as<ListNode>());
  }

  bool visitMulExpr(ParseNode*& pn) {
    return Base::visitMulExpr(pn) &&
           FoldBinaryArithmetic(cx, &pn->as<ListNode>());
  }

  bool visitDivExpr(ParseNode*& pn) {
    return Base::visitDivExpr(pn) &&
           FoldBinaryArithmetic(cx, &pn->as<ListNode>());
  }

  bool visitModExpr(ParseNode*& pn) {
    return Base::visitModExpr(pn) &&
           FoldBinaryArithmetic(cx, &pn->as<ListNode>());
  }

  bool visitAddExpr(ParseNode*& pn) {
    return Base::visitAddExpr(pn) && FoldAdd(cx, &pn);
  }

  bool visitSubExpr(ParseNode*& pn) {
    return Base::visitSubExpr(pn) &&
           FoldBinaryArithmetic(cx, &pn->as<ListNode>());
  }

  bool visitLshExpr(ParseNode*& pn) {
    return Base::visitLshExpr(pn) &&
           FoldBinaryArithmetic(cx, &pn->as<ListNode>());
  }

  bool visitRshExpr(ParseNode*& pn) {
    return Base::visitRshExpr(pn) &&
           FoldBinaryArithmetic(cx, &pn->as<ListNode>());
  }

  bool visitUrshExpr(ParseNode*& pn) {
    return Base::visitUrshExpr(pn) &&
           FoldBinaryArithmetic(cx, &pn->as<ListNode>());
  }

  bool visitAndExpr(ParseNode*& pn) {
    // Note that this does result in the unfortunate fact that dead arms of this
    // node get constant folded. The same goes for visitOr.
    return Base::visitAndExpr(pn) && FoldAndOr(cx, &pn);
  }

  bool visitOrExpr(ParseNode*& pn) {
    return Base::visitOrExpr(pn) && FoldAndOr(cx, &pn);
  }

  bool visitConditionalExpr(ParseNode*& pn) {
    // Don't call base-class visitConditional because FoldConditional processes
    // pn's child nodes specially to save stack space.
    return FoldConditional(cx, &pn);
  }

 private:
  bool internalVisitCall(BinaryNode* node) {
    MOZ_ASSERT(node->isKind(ParseNodeKind::CallExpr) ||
               node->isKind(ParseNodeKind::SuperCallExpr) ||
               node->isKind(ParseNodeKind::NewExpr) ||
               node->isKind(ParseNodeKind::TaggedTemplateExpr));

    // Don't fold a parenthesized callable component in an invocation, as this
    // might cause a different |this| value to be used, changing semantics:
    //
    //   var prop = "global";
    //   var obj = { prop: "obj", f: function() { return this.prop; } };
    //   assertEq((true ? obj.f : null)(), "global");
    //   assertEq(obj.f(), "obj");
    //   assertEq((true ? obj.f : null)``, "global");
    //   assertEq(obj.f``, "obj");
    //
    // As an exception to this, we do allow folding the function in
    // `(function() { ... })()` (the module pattern), because that lets us
    // constant fold code inside that function.
    //
    // See bug 537673 and bug 1182373.
    ParseNode* callee = node->left();
    if (node->isKind(ParseNodeKind::NewExpr) || !callee->isInParens() ||
        callee->isKind(ParseNodeKind::Function)) {
      if (!visit(*node->unsafeLeftReference())) {
        return false;
      }
    }

    if (!visit(*node->unsafeRightReference())) {
      return false;
    }

    return true;
  }

 public:
  bool visitCallExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitNewExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitSuperCallExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitTaggedTemplateExpr(ParseNode*& pn) {
    return internalVisitCall(&pn->as<BinaryNode>());
  }

  bool visitIfStmt(ParseNode*& pn) {
    // Don't call base-class visitIf because FoldIf processes pn's child nodes
    // specially to save stack space.
    return FoldIf(cx, &pn);
  }

  bool visitForStmt(ParseNode*& pn) {
    if (!Base::visitForStmt(pn)) {
      return false;
    }

    ForNode& stmt = pn->as<ForNode>();
    if (stmt.left()->isKind(ParseNodeKind::ForHead)) {
      TernaryNode& head = stmt.left()->as<TernaryNode>();
      ParseNode** test = head.unsafeKid2Reference();
      if (*test) {
        if (!SimplifyCondition(cx, test)) {
          return false;
        }
        if ((*test)->isKind(ParseNodeKind::TrueExpr)) {
          *test = nullptr;
        }
      }
    }

    return true;
  }

  bool visitWhileStmt(ParseNode*& pn) {
    BinaryNode& node = pn->as<BinaryNode>();
    return Base::visitWhileStmt(pn) &&
           SimplifyCondition(cx, node.unsafeLeftReference());
  }

  bool visitDoWhileStmt(ParseNode*& pn) {
    BinaryNode& node = pn->as<BinaryNode>();
    return Base::visitDoWhileStmt(pn) &&
           SimplifyCondition(cx, node.unsafeRightReference());
  }

  bool visitFunction(ParseNode*& pn) {
    CodeNode& node = pn->as<CodeNode>();

    // Don't constant-fold inside "use asm" code, as this could create a parse
    // tree that doesn't type-check as asm.js.
    if (node.funbox()->useAsmOrInsideUseAsm()) {
      return true;
    }

    return Base::visitFunction(pn);
  }

  bool visitArrayExpr(ParseNode*& pn) {
    if (!Base::visitArrayExpr(pn)) {
      return false;
    }

    ListNode* list = &pn->as<ListNode>();
    // Empty arrays are non-constant, since we cannot easily determine their
    // type.
    if (list->hasNonConstInitializer() && list->count() > 0) {
      for (ParseNode* node : list->contents()) {
        if (!node->isConstant()) {
          return true;
        }
      }
      list->unsetHasNonConstInitializer();
    }
    return true;
  }

  bool visitObjectExpr(ParseNode*& pn) {
    if (!Base::visitObjectExpr(pn)) {
      return false;
    }

    ListNode* list = &pn->as<ListNode>();
    if (list->hasNonConstInitializer()) {
      for (ParseNode* node : list->contents()) {
        if (node->getKind() != ParseNodeKind::Colon) {
          return true;
        }
        BinaryNode* binary = &node->as<BinaryNode>();
        if (binary->left()->isKind(ParseNodeKind::ComputedName)) {
          return true;
        }
        if (!binary->right()->isConstant()) {
          return true;
        }
      }
      list->unsetHasNonConstInitializer();
    }
    return true;
  }
};

bool Fold(JSContext* cx, ParseNode** pnp) {
  FoldVisitor visitor(cx);
  return visitor.visit(*pnp);
}

bool frontend::FoldConstants(JSContext* cx, ParseNode** pnp,
                             PerHandlerParser<FullParseHandler>* parser) {
  // Don't constant-fold inside "use asm" code, as this could create a parse
  // tree that doesn't type-check as asm.js.
  if (parser->pc->useAsmOrInsideUseAsm()) {
    return true;
  }

  AutoTraceLog traceLog(TraceLoggerForCurrentThread(cx),
                        TraceLogger_BytecodeFoldConstants);

  return Fold(cx, pnp);
}
