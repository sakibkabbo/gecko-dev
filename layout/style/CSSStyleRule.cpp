/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/CSSStyleRule.h"

#include "mozilla/DeclarationBlock.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/dom/CSSStyleRuleBinding.h"

#include "mozAutoDocUpdate.h"

using namespace mozilla::dom;

namespace mozilla {
namespace dom {

// -- CSSStyleRuleDeclaration ---------------------------------------

CSSStyleRuleDeclaration::CSSStyleRuleDeclaration(
    already_AddRefed<RawServoDeclarationBlock> aDecls)
    : mDecls(new DeclarationBlock(std::move(aDecls))) {}

CSSStyleRuleDeclaration::~CSSStyleRuleDeclaration() {
  mDecls->SetOwningRule(nullptr);
}

// QueryInterface implementation for CSSStyleRuleDeclaration
NS_INTERFACE_MAP_BEGIN(CSSStyleRuleDeclaration)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  // We forward the cycle collection interfaces to Rule(), which is
  // never null (in fact, we're part of that object!)
  if (aIID.Equals(NS_GET_IID(nsCycleCollectionISupports)) ||
      aIID.Equals(NS_GET_IID(nsXPCOMCycleCollectionParticipant))) {
    return Rule()->QueryInterface(aIID, aInstancePtr);
  }
NS_INTERFACE_MAP_END_INHERITING(nsDOMCSSDeclaration)

NS_IMPL_ADDREF_USING_AGGREGATOR(CSSStyleRuleDeclaration, Rule())
NS_IMPL_RELEASE_USING_AGGREGATOR(CSSStyleRuleDeclaration, Rule())

/* nsDOMCSSDeclaration implementation */

css::Rule* CSSStyleRuleDeclaration::GetParentRule() { return Rule(); }

nsINode* CSSStyleRuleDeclaration::GetParentObject() {
  return Rule()->GetParentObject();
}

DeclarationBlock* CSSStyleRuleDeclaration::GetOrCreateCSSDeclaration(
    Operation aOperation, DeclarationBlock** aCreated) {
  return mDecls;
}

nsresult CSSStyleRuleDeclaration::SetCSSDeclaration(
    DeclarationBlock* aDecl, MutationClosureData* aClosureData) {
  CSSStyleRule* rule = Rule();
  if (RefPtr<StyleSheet> sheet = rule->GetStyleSheet()) {
    if (aDecl != mDecls) {
      mDecls->SetOwningRule(nullptr);
      RefPtr<DeclarationBlock> decls = aDecl;
      Servo_StyleRule_SetStyle(rule->Raw(), decls->Raw());
      mDecls = decls.forget();
      mDecls->SetOwningRule(rule);
    }
    sheet->RuleChanged(rule);
  }
  return NS_OK;
}

Document* CSSStyleRuleDeclaration::DocToUpdate() { return nullptr; }

nsDOMCSSDeclaration::ParsingEnvironment
CSSStyleRuleDeclaration::GetParsingEnvironment(
    nsIPrincipal* aSubjectPrincipal) const {
  return GetParsingEnvironmentForRule(Rule());
}

// -- CSSStyleRule --------------------------------------------------

CSSStyleRule::CSSStyleRule(already_AddRefed<RawServoStyleRule> aRawRule,
                           StyleSheet* aSheet, css::Rule* aParentRule,
                           uint32_t aLine, uint32_t aColumn)
    : BindingStyleRule(aSheet, aParentRule, aLine, aColumn),
      mRawRule(aRawRule),
      mDecls(Servo_StyleRule_GetStyle(mRawRule).Consume()) {}

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(CSSStyleRule, css::Rule)

NS_IMPL_CYCLE_COLLECTION_CLASS(CSSStyleRule)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(CSSStyleRule, css::Rule)
  // Keep this in sync with IsCCLeaf.

  // Trace the wrapper for our declaration.  This just expands out
  // NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER which we can't use
  // directly because the wrapper is on the declaration, not on us.
  tmp->mDecls.TraceWrapper(aCallbacks, aClosure);
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(CSSStyleRule, css::Rule)
  // Keep this in sync with IsCCLeaf.

  // Unlink the wrapper for our declaraton.  This just expands out
  // NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER which we can't use
  // directly because the wrapper is on the declaration, not on us.
  tmp->mDecls.ReleaseWrapper(static_cast<nsISupports*>(p));
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(CSSStyleRule, css::Rule)
  // Keep this in sync with IsCCLeaf.
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

bool CSSStyleRule::IsCCLeaf() const {
  if (!Rule::IsCCLeaf()) {
    return false;
  }

  return !mDecls.PreservingWrapper();
}

size_t CSSStyleRule::SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
  size_t n = aMallocSizeOf(this);

  // Measurement of the following members may be added later if DMD finds it
  // is worthwhile:
  // - mRawRule
  // - mDecls

  return n;
}

#ifdef DEBUG
void CSSStyleRule::List(FILE* out, int32_t aIndent) const {
  nsAutoCString str;
  for (int32_t i = 0; i < aIndent; i++) {
    str.AppendLiteral("  ");
  }
  Servo_StyleRule_Debug(mRawRule, &str);
  fprintf_stderr(out, "%s\n", str.get());
}
#endif

/* CSSRule implementation */

void CSSStyleRule::GetCssText(nsAString& aCssText) const {
  Servo_StyleRule_GetCssText(mRawRule, &aCssText);
}

nsICSSDeclaration* CSSStyleRule::Style() { return &mDecls; }

/* CSSStyleRule implementation */

void CSSStyleRule::GetSelectorText(nsAString& aSelectorText) {
  Servo_StyleRule_GetSelectorText(mRawRule, &aSelectorText);
}

void CSSStyleRule::SetSelectorText(const nsAString& aSelectorText) {
  if (RefPtr<StyleSheet> sheet = GetStyleSheet()) {
    // StyleRule lives inside of the Inner, it is unsafe to call WillDirty
    // if sheet does not already have a unique Inner.
    sheet->AssertHasUniqueInner();
    sheet->WillDirty();

    const RawServoStyleSheetContents* contents = sheet->RawContents();
    if (Servo_StyleRule_SetSelectorText(contents, mRawRule, &aSelectorText)) {
      sheet->RuleChanged(this);
    }
  }
}

uint32_t CSSStyleRule::GetSelectorCount() {
  uint32_t aCount;
  Servo_StyleRule_GetSelectorCount(mRawRule, &aCount);
  return aCount;
}

nsresult CSSStyleRule::GetSelectorText(uint32_t aSelectorIndex,
                                       nsAString& aText) {
  Servo_StyleRule_GetSelectorTextAtIndex(mRawRule, aSelectorIndex, &aText);
  return NS_OK;
}

nsresult CSSStyleRule::GetSpecificity(uint32_t aSelectorIndex,
                                      uint64_t* aSpecificity) {
  Servo_StyleRule_GetSpecificityAtIndex(mRawRule, aSelectorIndex, aSpecificity);
  return NS_OK;
}

nsresult CSSStyleRule::SelectorMatchesElement(Element* aElement,
                                              uint32_t aSelectorIndex,
                                              const nsAString& aPseudo,
                                              bool* aMatches) {
  CSSPseudoElementType pseudoType = CSSPseudoElementType::NotPseudo;
  if (!aPseudo.IsEmpty()) {
    RefPtr<nsAtom> pseudoElt = NS_Atomize(aPseudo);
    pseudoType = nsCSSPseudoElements::GetPseudoType(
        pseudoElt, CSSEnabledState::eIgnoreEnabledState);

    if (pseudoType == CSSPseudoElementType::NotPseudo) {
      *aMatches = false;
      return NS_OK;
    }
  }

  *aMatches = Servo_StyleRule_SelectorMatchesElement(
      mRawRule, aElement, aSelectorIndex, pseudoType);

  return NS_OK;
}

NotNull<DeclarationBlock*> CSSStyleRule::GetDeclarationBlock() const {
  return WrapNotNull(mDecls.mDecls);
}

}  // namespace dom
}  // namespace mozilla
