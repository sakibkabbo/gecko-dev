/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScrollAnchorContainer.h"

#include "mozilla/StaticPrefs.h"
#include "nsGfxScrollFrame.h"
#include "nsLayoutUtils.h"

#define ANCHOR_LOG(...)
// #define ANCHOR_LOG(...) printf_stderr("ANCHOR: " __VA_ARGS__)

namespace mozilla {
namespace layout {

ScrollAnchorContainer::ScrollAnchorContainer(ScrollFrameHelper* aScrollFrame)
    : mScrollFrame(aScrollFrame),
      mAnchorNode(nullptr),
      mLastAnchorPos(0, 0),
      mAnchorNodeIsDirty(true),
      mApplyingAnchorAdjustment(false),
      mSuppressAnchorAdjustment(false) {}

ScrollAnchorContainer::~ScrollAnchorContainer() {}

ScrollAnchorContainer* ScrollAnchorContainer::FindFor(nsIFrame* aFrame) {
  aFrame = aFrame->GetParent();
  if (!aFrame) {
    return nullptr;
  }
  nsIScrollableFrame* nearest = nsLayoutUtils::GetNearestScrollableFrame(
      aFrame, nsLayoutUtils::SCROLLABLE_SAME_DOC |
                  nsLayoutUtils::SCROLLABLE_INCLUDE_HIDDEN);
  if (nearest) {
    return nearest->GetAnchor();
  }
  return nullptr;
}

nsIFrame* ScrollAnchorContainer::Frame() const { return mScrollFrame->mOuter; }

nsIScrollableFrame* ScrollAnchorContainer::ScrollableFrame() const {
  return Frame()->GetScrollTargetFrame();
}

/**
 * Set the appropriate frame flags for a frame that has become or is no longer
 * an anchor node.
 */
static void SetAnchorFlags(const nsIFrame* aScrolledFrame,
                           nsIFrame* aAnchorNode, bool aInScrollAnchorChain) {
  nsIFrame* frame = aAnchorNode;
  while (frame && frame != aScrolledFrame) {
    MOZ_ASSERT(
        frame == aAnchorNode || !frame->IsScrollFrame(),
        "We shouldn't select an anchor node inside a nested scroll frame.");

    frame->SetInScrollAnchorChain(aInScrollAnchorChain);
    frame = frame->GetParent();
  }
  MOZ_ASSERT(frame,
             "The anchor node should be a descendant of the scroll frame");
  // If needed, invalidate the frame so that we start/stop highlighting the
  // anchor
  if (StaticPrefs::layout_css_scroll_anchoring_highlight()) {
    for (nsIFrame* frame = aAnchorNode->FirstContinuation(); !!frame;
         frame = frame->GetNextContinuation()) {
      frame->InvalidateFrame();
    }
  }
}

/**
 * Compute the scrollable overflow rect [1] of aCandidate relative to
 * aScrollFrame with all transforms applied. The specification is also
 * ambiguous about what can be selected as a scroll anchor, which makes
 * the scroll anchoring bounding rect partially undefined [2]. This code
 * attempts to match the implementation in Blink.
 *
 * [1]
 * https://drafts.csswg.org/css-scroll-anchoring-1/#scroll-anchoring-bounding-rect
 * [2] https://github.com/w3c/csswg-drafts/issues/3478
 */
static nsRect FindScrollAnchoringBoundingRect(const nsIFrame* aScrollFrame,
                                              nsIFrame* aCandidate) {
  MOZ_ASSERT(nsLayoutUtils::IsProperAncestorFrame(aScrollFrame, aCandidate));
  if (aCandidate->GetContent()->IsText()) {
    nsRect bounding;
    for (nsIFrame* continuation = aCandidate->FirstContinuation(); continuation;
         continuation = continuation->GetNextContinuation()) {
      nsRect localRect =
          continuation->GetScrollableOverflowRectRelativeToSelf();
      nsRect transformed = nsLayoutUtils::TransformFrameRectToAncestor(
          continuation, localRect, aScrollFrame);
      bounding = bounding.Union(transformed);
    }
    return bounding;
  }

  nsRect localRect = aCandidate->GetScrollableOverflowRectRelativeToSelf();
  nsRect transformed = nsLayoutUtils::TransformFrameRectToAncestor(
      aCandidate, localRect, aScrollFrame);
  return transformed;
}

void ScrollAnchorContainer::SelectAnchor() {
  MOZ_ASSERT(mScrollFrame->mScrolledFrame);
  MOZ_ASSERT(mAnchorNodeIsDirty);

  if (!StaticPrefs::layout_css_scroll_anchoring_enabled()) {
    return;
  }

  ANCHOR_LOG("Selecting anchor for %p with scroll-port [%d %d x %d %d].\n",
             this, mScrollFrame->mScrollPort.x, mScrollFrame->mScrollPort.y,
             mScrollFrame->mScrollPort.width, mScrollFrame->mScrollPort.height);

  const nsStyleDisplay* disp = Frame()->StyleDisplay();

  // Don't select a scroll anchor if the scroll frame has `overflow-anchor:
  // none`.
  bool overflowAnchor =
      disp->mOverflowAnchor == mozilla::StyleOverflowAnchor::Auto;

  // Or if the scroll frame has not been scrolled from the logical origin. This
  // is not in the specification [1], but Blink does this.
  //
  // [1] https://github.com/w3c/csswg-drafts/issues/3319
  bool isScrolled = mScrollFrame->GetLogicalScrollPosition() != nsPoint();

  // Or if there is perspective that could affect the scrollable overflow rect
  // for descendant frames. This is not in the specification as Blink doesn't
  // share this behavior with perspective [1].
  //
  // [1] https://github.com/w3c/csswg-drafts/issues/3322
  bool hasPerspective = Frame()->ChildrenHavePerspective();

  // Select a new scroll anchor
  nsIFrame* oldAnchor = mAnchorNode;
  if (overflowAnchor && isScrolled && !hasPerspective) {
    ANCHOR_LOG("Beginning candidate selection.\n");
    mAnchorNode = FindAnchorIn(mScrollFrame->mScrolledFrame);
  } else {
    if (!overflowAnchor) {
      ANCHOR_LOG("Skipping candidate selection for `overflow-anchor: none`\n");
    }
    if (!isScrolled) {
      ANCHOR_LOG("Skipping candidate selection for not being scrolled\n");
    }
    if (hasPerspective) {
      ANCHOR_LOG(
          "Skipping candidate selection for scroll frame with perspective\n");
    }
    mAnchorNode = nullptr;
  }

  // Update the anchor flags if needed
  if (oldAnchor != mAnchorNode) {
    ANCHOR_LOG("Anchor node has changed from (%p) to (%p).\n", oldAnchor,
               mAnchorNode);

    // Unset all flags for the old scroll anchor
    if (oldAnchor) {
      SetAnchorFlags(mScrollFrame->mScrolledFrame, oldAnchor, false);
    }

    // Set all flags for the new scroll anchor
    if (mAnchorNode) {
      // Anchor selection will never select a descendant of a different scroll
      // frame, so we can set flags without conflicting with other scroll
      // anchor containers.
      SetAnchorFlags(mScrollFrame->mScrolledFrame, mAnchorNode, true);
    }
  } else {
    ANCHOR_LOG("Anchor node has remained (%p).\n", mAnchorNode);
  }

  // Calculate the position to use for scroll adjustments
  if (mAnchorNode) {
    mLastAnchorPos =
        FindScrollAnchoringBoundingRect(Frame(), mAnchorNode).TopLeft();
    ANCHOR_LOG("Using last anchor position = [%d, %d].\n", mLastAnchorPos.x,
               mLastAnchorPos.y);
  } else {
    mLastAnchorPos = nsPoint();
  }

  mAnchorNodeIsDirty = false;
}

void ScrollAnchorContainer::UserScrolled() {
  if (mApplyingAnchorAdjustment) {
    return;
  }
  InvalidateAnchor();
}

void ScrollAnchorContainer::SuppressAdjustments() {
  ANCHOR_LOG("Received a scroll anchor suppression for %p.\n", this);
  mSuppressAnchorAdjustment = true;
}

void ScrollAnchorContainer::InvalidateAnchor() {
  if (!StaticPrefs::layout_css_scroll_anchoring_enabled()) {
    return;
  }

  ANCHOR_LOG("Invalidating scroll anchor %p for %p.\n", mAnchorNode, this);

  if (mAnchorNode) {
    SetAnchorFlags(mScrollFrame->mScrolledFrame, mAnchorNode, false);
  }
  mAnchorNode = nullptr;
  mAnchorNodeIsDirty = true;
  mLastAnchorPos = nsPoint();
  Frame()->PresShell()->PostDirtyScrollAnchorContainer(ScrollableFrame());
}

void ScrollAnchorContainer::Destroy() {
  if (mAnchorNode) {
    SetAnchorFlags(mScrollFrame->mScrolledFrame, mAnchorNode, false);
  }
  mAnchorNode = nullptr;
  mAnchorNodeIsDirty = false;
  mLastAnchorPos = nsPoint();
}

void ScrollAnchorContainer::ApplyAdjustments() {
  if (!mAnchorNode || mAnchorNodeIsDirty) {
    mSuppressAnchorAdjustment = false;
    ANCHOR_LOG("Ignoring post-reflow (anchor=%p, dirty=%d, container=%p).\n",
               mAnchorNode, mAnchorNodeIsDirty, this);
    return;
  }

  nsPoint current =
      FindScrollAnchoringBoundingRect(Frame(), mAnchorNode).TopLeft();
  nsPoint adjustment = current - mLastAnchorPos;
  nsIntPoint adjustmentDevicePixels =
      adjustment.ToNearestPixels(Frame()->PresContext()->AppUnitsPerDevPixel());

  ANCHOR_LOG("Anchor has moved from [%d, %d] to [%d, %d].\n", mLastAnchorPos.x,
             mLastAnchorPos.y, current.x, current.y);

  WritingMode writingMode = Frame()->GetWritingMode();

  // The specification only allows for anchor adjustments in the block
  // dimension, so remove the other component.
  if (writingMode.IsVertical()) {
    adjustmentDevicePixels.y = 0;
  } else {
    adjustmentDevicePixels.x = 0;
  }

  if (adjustmentDevicePixels == nsIntPoint()) {
    ANCHOR_LOG("Ignoring zero delta anchor adjustment for %p.\n", this);
    mSuppressAnchorAdjustment = false;
    return;
  }

  if (mSuppressAnchorAdjustment) {
    ANCHOR_LOG("Applying anchor adjustment suppression for %p.\n", this);
    mSuppressAnchorAdjustment = false;
    InvalidateAnchor();
    return;
  }

  ANCHOR_LOG("Applying anchor adjustment of (%d %d) for %p and anchor %p.\n",
             adjustment.x, adjustment.y, this, mAnchorNode);

  MOZ_ASSERT(!mApplyingAnchorAdjustment);
  // We should use AutoRestore here, but that doesn't work with bitfields
  mApplyingAnchorAdjustment = true;
  mScrollFrame->ScrollBy(
      adjustmentDevicePixels, nsIScrollableFrame::DEVICE_PIXELS,
      nsIScrollableFrame::INSTANT, nullptr, nsGkAtoms::relative);
  mApplyingAnchorAdjustment = false;

  // The anchor position may not be in the same relative position after
  // adjustment. Update ourselves so we have consistent state.
  mLastAnchorPos =
      FindScrollAnchoringBoundingRect(Frame(), mAnchorNode).TopLeft();
}

ScrollAnchorContainer::ExamineResult
ScrollAnchorContainer::ExamineAnchorCandidate(nsIFrame* aFrame) const {
#ifdef DEBUG_FRAME_DUMP
  nsCString tag = aFrame->ListTag();
  ANCHOR_LOG("\tVisiting frame=%s (%p).\n", tag.get(), aFrame);
#else
  ANCHOR_LOG("\t\tVisiting frame=%p.\n", aFrame);
#endif

  // Check if the author has opted out of scroll anchoring for this frame
  // and its descendants.
  const nsStyleDisplay* disp = aFrame->StyleDisplay();
  if (disp->mOverflowAnchor == mozilla::StyleOverflowAnchor::None) {
    ANCHOR_LOG("\t\tExcluding `overflow-anchor: none`.\n");
    return ExamineResult::Exclude;
  }

  // Sticky positioned elements can move with the scroll frame, making them
  // unsuitable scroll anchors. This isn't in the specification yet [1], but
  // matches Blink's implementation.
  //
  // [1] https://github.com/w3c/csswg-drafts/issues/3319
  if (aFrame->IsStickyPositioned()) {
    ANCHOR_LOG("\t\tExcluding `position: sticky`.\n");
    return ExamineResult::Exclude;
  }

  // The frame for a <br> element has a non-zero area, but Blink treats them
  // as if they have no area, so exclude them specially.
  if (aFrame->IsBrFrame()) {
    ANCHOR_LOG("\t\tExcluding <br>.\n");
    return ExamineResult::Exclude;
  }

  // Exclude frames that aren't accessible to content.
  bool isChrome =
      aFrame->GetContent() && aFrame->GetContent()->ChromeOnlyAccess();
  bool isPseudo = aFrame->Style()->IsPseudoElement();
  if (isChrome && !isPseudo) {
    ANCHOR_LOG("\t\tExcluding chrome only content.\n");
    return ExamineResult::Exclude;
  }

  // See if this frame could have its own anchor node. We could check
  // IsScrollFrame(), but that would miss nsListControlFrame which is not a
  // scroll frame, but still inherits from nsHTMLScrollFrame.
  nsIScrollableFrame* scrollable = do_QueryFrame(aFrame);

  // We don't allow scroll anchors to be selected inside of scrollable frames as
  // it's not clear how an anchor adjustment should apply to multiple scrollable
  // frames. Blink allows this to happen, but they're not sure why [1].
  //
  // We also don't allow scroll anchors to be selected inside of SVG as it uses
  // a different layout model than CSS, and the specification doesn't say it
  // should apply.
  //
  // [1] https://github.com/w3c/csswg-drafts/issues/3477
  bool canDescend = !scrollable && !aFrame->IsSVGOuterSVGFrame();

  // Check what kind of frame this is
  bool isBlockOutside = aFrame->IsBlockOutside();
  bool isText = aFrame->GetContent()->IsText();
  bool isAnonBox = aFrame->Style()->IsAnonBox() && !isText;
  bool isInlineOutside = aFrame->IsInlineOutside() && !isText;
  bool isContinuation = !!aFrame->GetPrevContinuation();

  // If the frame is anonymous or inline-outside, search its descendants for a
  // scroll anchor.
  if ((isAnonBox || isInlineOutside) && canDescend) {
    ANCHOR_LOG(
        "\t\tSearching descendants of anon or inline box (a=%d, i=%d).\n",
        isAnonBox, isInlineOutside);
    return ExamineResult::PassThrough;
  }

  // If the frame is not block-outside or a text node then exclude it.
  if (!isBlockOutside && !isText) {
    ANCHOR_LOG("\t\tExcluding non block-outside or text node (b=%d, t=%d).\n",
               isBlockOutside, isText);
    return ExamineResult::Exclude;
  }

  // Find the scroll anchoring bounding rect.
  nsRect rect = FindScrollAnchoringBoundingRect(Frame(), aFrame);
  ANCHOR_LOG("\t\trect = [%d %d x %d %d].\n", rect.x, rect.y, rect.width,
             rect.height);

  // Check if this frame is visible in the scroll port. This will exclude rects
  // with zero sized area. The specification is ambiguous about this [1], but
  // this matches Blink's implementation.
  //
  // [1] https://github.com/w3c/csswg-drafts/issues/3483
  nsRect visibleRect;
  if (!visibleRect.IntersectRect(rect, mScrollFrame->mScrollPort)) {
    return ExamineResult::Exclude;
  }

  // At this point, if canDescend is true, we should only have visible
  // non-anonymous frames that are either:
  //   1. block-outside
  //   2. text nodes
  //
  // It's not clear what the scroll anchoring bounding rect of elements that are
  // block-outside should be when they are fragmented. For text nodes that are
  // fragmented, it's specified that we need to consider the union of its line
  // boxes.
  //
  // So for text nodes we handle them by including the union of line boxes in
  // the bounding rect of the primary frame, and not selecting any
  // continuations.
  //
  // For block-outside elements we choose to consider the bounding rect of each
  // frame individually, allowing ourselves to descend into any frame, but only
  // selecting a frame if it's not a continuation.
  if (canDescend && isContinuation) {
    ANCHOR_LOG("\t\tSearching descendants of a continuation.\n");
    return ExamineResult::PassThrough;
  }

  // If this frame is fully visible, then select it as the scroll anchor.
  if (visibleRect.IsEqualEdges(rect)) {
    ANCHOR_LOG("\t\tFully visible, taking.\n");
    return ExamineResult::Accept;
  }

  // If we can't descend into this frame, then select it as the scroll anchor.
  if (!canDescend) {
    ANCHOR_LOG("\t\tIntersects a frame that we can't descend into, taking.\n");
    return ExamineResult::Accept;
  }

  // It must be partially visible and we can descend into this frame. Examine
  // its children for a better scroll anchor or fall back to this one.
  ANCHOR_LOG("\t\tIntersects valid candidate, checking descendants.\n");
  return ExamineResult::Traverse;
}

nsIFrame* ScrollAnchorContainer::FindAnchorIn(nsIFrame* aFrame) const {
  // Visit the child lists of this frame
  for (nsIFrame::ChildListIterator lists(aFrame); !lists.IsDone();
       lists.Next()) {
    // Skip child lists that contain out-of-flow frames, we'll visit them by
    // following placeholders in the in-flow lists so that we visit these
    // frames in DOM order.
    // XXX do we actually need to exclude kOverflowOutOfFlowList too?
    if (lists.CurrentID() == FrameChildListID::kAbsoluteList ||
        lists.CurrentID() == FrameChildListID::kFixedList ||
        lists.CurrentID() == FrameChildListID::kFloatList ||
        lists.CurrentID() == FrameChildListID::kOverflowOutOfFlowList) {
      continue;
    }

    // Search the child list, and return if we selected an anchor
    if (nsIFrame* anchor = FindAnchorInList(lists.CurrentList())) {
      return anchor;
    }
  }

  // The spec requires us to do an extra pass to visit absolutely positioned
  // frames a second time after all the children of their containing block have
  // been visited.
  //
  // It's not clear why this is needed [1], but it matches Blink's
  // implementation, and is needed for a WPT test.
  //
  // [1] https://github.com/w3c/csswg-drafts/issues/3465
  const nsFrameList& absPosList =
      aFrame->GetChildList(FrameChildListID::kAbsoluteList);
  if (nsIFrame* anchor = FindAnchorInList(absPosList)) {
    return anchor;
  }

  return nullptr;
}

nsIFrame* ScrollAnchorContainer::FindAnchorInList(
    const nsFrameList& aFrameList) const {
  for (nsIFrame* child : aFrameList) {
    // If this is a placeholder, try to follow it to the out of flow frame.
    nsIFrame* realFrame = nsPlaceholderFrame::GetRealFrameFor(child);
    if (child != realFrame) {
      // If the out of flow frame is not a descendant of our scroll frame,
      // then it must have a different containing block and cannot be an
      // anchor node.
      if (!nsLayoutUtils::IsProperAncestorFrame(Frame(), realFrame)) {
        ANCHOR_LOG(
            "\t\tSkipping out of flow frame that is not a descendant of the "
            "scroll frame.\n");
        continue;
      }
      ANCHOR_LOG("\t\tFollowing placeholder to out of flow frame.\n");
      child = realFrame;
    }

    // Perform the candidate examination algorithm
    ExamineResult examine = ExamineAnchorCandidate(child);

    // See the comment before the definition of `ExamineResult` in
    // `ScrollAnchorContainer.h` for an explanation of this behavior.
    switch (examine) {
      case ExamineResult::Exclude: {
        continue;
      }
      case ExamineResult::PassThrough: {
        nsIFrame* candidate = FindAnchorIn(child);
        if (!candidate) {
          continue;
        }
        return candidate;
      }
      case ExamineResult::Traverse: {
        nsIFrame* candidate = FindAnchorIn(child);
        if (!candidate) {
          return child;
        }
        return candidate;
      }
      case ExamineResult::Accept: {
        return child;
      }
    }
  }
  return nullptr;
}

}  // namespace layout
}  // namespace mozilla
