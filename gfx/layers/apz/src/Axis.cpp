/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Axis.h"
#include <math.h>                       // for fabsf, pow, powf
#include <algorithm>                    // for max
#include "AsyncPanZoomController.h"     // for AsyncPanZoomController
#include "mozilla/dom/AnimationPlayer.h" // for ComputedTimingFunction
#include "mozilla/layers/APZCTreeManager.h" // for APZCTreeManager
#include "FrameMetrics.h"               // for FrameMetrics
#include "mozilla/Attributes.h"         // for MOZ_FINAL
#include "mozilla/Preferences.h"        // for Preferences
#include "mozilla/gfx/Rect.h"           // for RoundedIn
#include "mozilla/mozalloc.h"           // for operator new
#include "mozilla/FloatingPoint.h"      // for FuzzyEqualsAdditive
#include "mozilla/StaticPtr.h"          // for StaticAutoPtr
#include "nsMathUtils.h"                // for NS_lround
#include "nsPrintfCString.h"            // for nsPrintfCString
#include "nsThreadUtils.h"              // for NS_DispatchToMainThread, etc
#include "nscore.h"                     // for NS_IMETHOD
#include "gfxPrefs.h"                   // for the preferences

#define AXIS_LOG(...)
// #define AXIS_LOG(...) printf_stderr("AXIS: " __VA_ARGS__)

namespace mozilla {
namespace layers {

extern StaticAutoPtr<ComputedTimingFunction> gVelocityCurveFunction;

Axis::Axis(AsyncPanZoomController* aAsyncPanZoomController)
  : mPos(0),
    mPosTimeMs(0),
    mVelocity(0.0f),
    mAxisLocked(false),
    mAsyncPanZoomController(aAsyncPanZoomController),
    mOverscroll(0),
    mInUnderscroll(false)
{
}

float Axis::ToLocalVelocity(float aVelocityInchesPerMs) const {
  ScreenPoint velocity = MakePoint(aVelocityInchesPerMs * APZCTreeManager::GetDPI());
  // Use ToScreenCoordinates() to convert a point rather than a vector by
  // treating the point as a vector, and using (0, 0) as the anchor.
  ScreenPoint panStart = mAsyncPanZoomController->ToScreenCoordinates(
      mAsyncPanZoomController->PanStart(),
      ParentLayerPoint());
  ParentLayerPoint localVelocity =
      mAsyncPanZoomController->ToParentLayerCoordinates(velocity, panStart);
  return localVelocity.Length();
}

void Axis::UpdateWithTouchAtDevicePoint(ParentLayerCoord aPos, uint32_t aTimestampMs) {
  // mVelocityQueue is controller-thread only
  AsyncPanZoomController::AssertOnControllerThread();

  if (aTimestampMs == mPosTimeMs) {
    // This could be a duplicate event, or it could be a legitimate event
    // on some platforms that generate events really fast. As a compromise
    // update mPos so we don't run into problems like bug 1042734, even though
    // that means the velocity will be stale. Better than doing a divide-by-zero.
    mPos = aPos;
    return;
  }

  float newVelocity = mAxisLocked ? 0.0f : (float)(mPos - aPos) / (float)(aTimestampMs - mPosTimeMs);
  if (gfxPrefs::APZMaxVelocity() > 0.0f) {
    bool velocityIsNegative = (newVelocity < 0);
    newVelocity = fabs(newVelocity);

    float maxVelocity = ToLocalVelocity(gfxPrefs::APZMaxVelocity());
    newVelocity = std::min(newVelocity, maxVelocity);

    if (gfxPrefs::APZCurveThreshold() > 0.0f && gfxPrefs::APZCurveThreshold() < gfxPrefs::APZMaxVelocity()) {
      float curveThreshold = ToLocalVelocity(gfxPrefs::APZCurveThreshold());
      if (newVelocity > curveThreshold) {
        // here, 0 < curveThreshold < newVelocity <= maxVelocity, so we apply the curve
        float scale = maxVelocity - curveThreshold;
        float funcInput = (newVelocity - curveThreshold) / scale;
        float funcOutput = gVelocityCurveFunction->GetValue(funcInput);
        float curvedVelocity = (funcOutput * scale) + curveThreshold;
        AXIS_LOG("Curving up velocity from %f to %f\n", newVelocity, curvedVelocity);
        newVelocity = curvedVelocity;
      }
    }

    if (velocityIsNegative) {
      newVelocity = -newVelocity;
    }
  }

  mVelocity = newVelocity;
  mPos = aPos;
  mPosTimeMs = aTimestampMs;

  // Limit queue size pased on pref
  mVelocityQueue.AppendElement(std::make_pair(aTimestampMs, mVelocity));
  if (mVelocityQueue.Length() > gfxPrefs::APZMaxVelocityQueueSize()) {
    mVelocityQueue.RemoveElementAt(0);
  }
}

void Axis::StartTouch(ParentLayerCoord aPos, uint32_t aTimestampMs) {
  mStartPos = aPos;
  mPos = aPos;
  mPosTimeMs = aTimestampMs;
  mAxisLocked = false;
}

bool Axis::AdjustDisplacement(ParentLayerCoord aDisplacement,
                              /* ParentLayerCoord */ float& aDisplacementOut,
                              /* ParentLayerCoord */ float& aOverscrollAmountOut)
{
  if (mAxisLocked) {
    aOverscrollAmountOut = 0;
    aDisplacementOut = 0;
    return false;
  }

  ParentLayerCoord displacement = aDisplacement;

  // First consume any overscroll in the opposite direction along this axis.
  ParentLayerCoord consumedOverscroll = 0;
  if (mOverscroll > 0 && aDisplacement < 0) {
    consumedOverscroll = std::min(mOverscroll, -aDisplacement);
  } else if (mOverscroll < 0 && aDisplacement > 0) {
    consumedOverscroll = 0.f - std::min(-mOverscroll, aDisplacement);
  }
  mOverscroll -= consumedOverscroll;
  displacement += consumedOverscroll;

  // Split the requested displacement into an allowed displacement that does
  // not overscroll, and an overscroll amount.
  aOverscrollAmountOut = DisplacementWillOverscrollAmount(displacement);
  if (aOverscrollAmountOut != 0.0f) {
    // No need to have a velocity along this axis anymore; it won't take us
    // anywhere, so we're just spinning needlessly.
    mVelocity = 0.0f;
    displacement -= aOverscrollAmountOut;
  }
  aDisplacementOut = displacement;
  return fabsf(consumedOverscroll) > EPSILON;
}

ParentLayerCoord Axis::ApplyResistance(ParentLayerCoord aRequestedOverscroll) const {
  // 'resistanceFactor' is a value between 0 and 1, which:
  //   - tends to 1 as the existing overscroll tends to 0
  //   - tends to 0 as the existing overscroll tends to the composition length
  // The actual overscroll is the requested overscroll multiplied by this
  // factor; this should prevent overscrolling by more than the composition
  // length.
  float resistanceFactor = 1 - fabsf(mOverscroll) / GetCompositionLength();
  return resistanceFactor < 0 ? ParentLayerCoord(0) : aRequestedOverscroll * resistanceFactor;
}

void Axis::OverscrollBy(ParentLayerCoord aOverscroll) {
  MOZ_ASSERT(CanScroll());
  aOverscroll = ApplyResistance(aOverscroll);
  if (aOverscroll > 0) {
#ifdef DEBUG
    if (!FuzzyEqualsAdditive(GetCompositionEnd().value, GetPageEnd().value, COORDINATE_EPSILON)) {
      nsPrintfCString message("composition end (%f) is not within COORDINATE_EPISLON of page end (%f)\n",
                              GetCompositionEnd().value, GetPageEnd().value);
      NS_ASSERTION(false, message.get());
      MOZ_CRASH();
    }
#endif
    MOZ_ASSERT(mOverscroll >= 0);
  } else if (aOverscroll < 0) {
#ifdef DEBUG
    if (!FuzzyEqualsAdditive(GetOrigin().value, GetPageStart().value, COORDINATE_EPSILON)) {
      nsPrintfCString message("composition origin (%f) is not within COORDINATE_EPISLON of page origin (%f)\n",
                              GetOrigin().value, GetPageStart().value);
      NS_ASSERTION(false, message.get());
      MOZ_CRASH();
    }
#endif
    MOZ_ASSERT(mOverscroll <= 0);
  }
  mOverscroll += aOverscroll;
}

ParentLayerCoord Axis::GetOverscroll() const {
  return mOverscroll;
}

bool Axis::IsInUnderscroll() const {
  return mInUnderscroll;
}

bool Axis::SampleOverscrollAnimation(const TimeDuration& aDelta) {
  // Apply spring physics to the overscroll as time goes on.
  // Note: this method of sampling isn't perfectly smooth, as it assumes
  // a constant velocity over 'aDelta', instead of an accelerating velocity.
  // (The way we applying friction to flings has the same issue.)
  // Hooke's law with damping:
  //   F = -kx - bv
  // where
  //   k is a constant related to the stiffness of the spring
  //     The larger the constant, the stiffer the spring.
  //   x is the displacement of the end of the spring from its equilibrium
  //     In our scenario, it's the amount of overscroll on the axis.
  //   b is a constant that provides damping (friction)
  //   v is the velocity of the point at the end of the spring
  // See http://gafferongames.com/game-physics/spring-physics/
  const float kSpringStiffness = gfxPrefs::APZOverscrollSpringStiffness();
  const float kSpringFriction = gfxPrefs::APZOverscrollSpringFriction();

  // Apply spring force.
  float springForce = -1 * kSpringStiffness * mOverscroll;
  // Assume unit mass, so force = acceleration.
  mVelocity += springForce * aDelta.ToMilliseconds();

  // Apply dampening.
  mVelocity *= pow(double(1 - kSpringFriction), aDelta.ToMilliseconds());

  // Adjust the amount of overscroll based on the velocity.
  // Note that we allow for oscillations. mInUnderscroll tracks whether
  // we are currently in a state where we have overshot and the spring is
  // displaced in the other direction.
  float oldOverscroll = mOverscroll;
  mOverscroll += (mVelocity * aDelta.ToMilliseconds());
  bool signChange = (oldOverscroll * mOverscroll) < 0;
  if (signChange) {
    // If the sign of mOverscroll changed, we have either entered underscroll
    // or exited it.
    mInUnderscroll = !mInUnderscroll;
  }

  // If both the velocity and the displacement fall below a threshold, stop
  // the animation so we don't continue doing tiny oscillations that aren't
  // noticeable.
  if (fabs(mOverscroll) < gfxPrefs::APZOverscrollStopDistanceThreshold() &&
      fabs(mVelocity) < gfxPrefs::APZOverscrollStopVelocityThreshold()) {
    // "Jump" to the at-rest state. The jump shouldn't be noticeable as the
    // velocity and overscroll are already low.
    mOverscroll = 0;
    mVelocity = 0;
    mInUnderscroll = false;
    return false;
  }

  // Otherwise, continue the animation.
  return true;
}

bool Axis::IsOverscrolled() const {
  return mOverscroll != 0.f;
}

void Axis::ClearOverscroll() {
  mOverscroll = 0;
}

ParentLayerCoord Axis::PanStart() const {
  return mStartPos;
}

ParentLayerCoord Axis::PanDistance() const {
  return fabs(mPos - mStartPos);
}

ParentLayerCoord Axis::PanDistance(ParentLayerCoord aPos) const {
  return fabs(aPos - mStartPos);
}

void Axis::EndTouch(uint32_t aTimestampMs) {
  // mVelocityQueue is controller-thread only
  AsyncPanZoomController::AssertOnControllerThread();

  mVelocity = 0;
  int count = 0;
  while (!mVelocityQueue.IsEmpty()) {
    uint32_t timeDelta = (aTimestampMs - mVelocityQueue[0].first);
    if (timeDelta < gfxPrefs::APZVelocityRelevanceTime()) {
      count++;
      mVelocity += mVelocityQueue[0].second;
    }
    mVelocityQueue.RemoveElementAt(0);
  }
  if (count > 1) {
    mVelocity /= count;
  }
}

void Axis::CancelTouch() {
  // mVelocityQueue is controller-thread only
  AsyncPanZoomController::AssertOnControllerThread();

  mVelocity = 0.0f;
  while (!mVelocityQueue.IsEmpty()) {
    mVelocityQueue.RemoveElementAt(0);
  }
}

bool Axis::CanScroll() const {
  return GetPageLength() - GetCompositionLength() > COORDINATE_EPSILON;
}

bool Axis::CanScrollNow() const {
  return !mAxisLocked && CanScroll();
}

bool Axis::FlingApplyFrictionOrCancel(const TimeDuration& aDelta,
                                      float aFriction,
                                      float aThreshold) {
  if (fabsf(mVelocity) <= aThreshold) {
    // If the velocity is very low, just set it to 0 and stop the fling,
    // otherwise we'll just asymptotically approach 0 and the user won't
    // actually see any changes.
    mVelocity = 0.0f;
    return false;
  } else {
    mVelocity *= pow(1.0f - aFriction, float(aDelta.ToMilliseconds()));
  }
  return true;
}

ParentLayerCoord Axis::DisplacementWillOverscrollAmount(ParentLayerCoord aDisplacement) const {
  ParentLayerCoord newOrigin = GetOrigin() + aDisplacement;
  ParentLayerCoord newCompositionEnd = GetCompositionEnd() + aDisplacement;
  // If the current pan plus a displacement takes the window to the left of or
  // above the current page rect.
  bool minus = newOrigin < GetPageStart();
  // If the current pan plus a displacement takes the window to the right of or
  // below the current page rect.
  bool plus = newCompositionEnd > GetPageEnd();
  if (minus && plus) {
    // Don't handle overscrolled in both directions; a displacement can't cause
    // this, it must have already been zoomed out too far.
    return 0;
  }
  if (minus) {
    return newOrigin - GetPageStart();
  }
  if (plus) {
    return newCompositionEnd - GetPageEnd();
  }
  return 0;
}

CSSCoord Axis::ScaleWillOverscrollAmount(float aScale, CSSCoord aFocus) const {
  // Internally, do computations in ParentLayer coordinates *before* the scale
  // is applied.
  CSSToParentLayerScale zoom = GetFrameMetrics().GetZoom();
  ParentLayerCoord focus = aFocus * zoom;
  ParentLayerCoord originAfterScale = (GetOrigin() + focus) - (focus / aScale);

  bool both = ScaleWillOverscrollBothSides(aScale);
  bool minus = GetPageStart() - originAfterScale > COORDINATE_EPSILON;
  bool plus = (originAfterScale + (GetCompositionLength() / aScale)) - GetPageEnd() > COORDINATE_EPSILON;

  if ((minus && plus) || both) {
    // If we ever reach here it's a bug in the client code.
    MOZ_ASSERT(false, "In an OVERSCROLL_BOTH condition in ScaleWillOverscrollAmount");
    return 0;
  }
  if (minus) {
    return (originAfterScale - GetPageStart()) / zoom;
  }
  if (plus) {
    return (originAfterScale + (GetCompositionLength() / aScale) - GetPageEnd()) / zoom;
  }
  return 0;
}

float Axis::GetVelocity() const {
  return mAxisLocked ? 0 : mVelocity;
}

void Axis::SetVelocity(float aVelocity) {
  mVelocity = aVelocity;
}

ParentLayerCoord Axis::GetCompositionEnd() const {
  return GetOrigin() + GetCompositionLength();
}

ParentLayerCoord Axis::GetPageEnd() const {
  return GetPageStart() + GetPageLength();
}

ParentLayerCoord Axis::GetOrigin() const {
  ParentLayerPoint origin = GetFrameMetrics().GetScrollOffset() * GetFrameMetrics().GetZoom();
  return GetPointOffset(origin);
}

ParentLayerCoord Axis::GetCompositionLength() const {
  return GetRectLength(GetFrameMetrics().mCompositionBounds);
}

ParentLayerCoord Axis::GetPageStart() const {
  ParentLayerRect pageRect = GetFrameMetrics().GetExpandedScrollableRect() * GetFrameMetrics().GetZoom();
  return GetRectOffset(pageRect);
}

ParentLayerCoord Axis::GetPageLength() const {
  ParentLayerRect pageRect = GetFrameMetrics().GetExpandedScrollableRect() * GetFrameMetrics().GetZoom();
  return GetRectLength(pageRect);
}

bool Axis::ScaleWillOverscrollBothSides(float aScale) const {
  const FrameMetrics& metrics = GetFrameMetrics();
  ParentLayerRect screenCompositionBounds = metrics.mCompositionBounds
                                          / ParentLayerToParentLayerScale(aScale);
  return GetRectLength(screenCompositionBounds) - GetPageLength() > COORDINATE_EPSILON;
}

const FrameMetrics& Axis::GetFrameMetrics() const {
  return mAsyncPanZoomController->GetFrameMetrics();
}


AxisX::AxisX(AsyncPanZoomController* aAsyncPanZoomController)
  : Axis(aAsyncPanZoomController)
{

}

ParentLayerCoord AxisX::GetPointOffset(const ParentLayerPoint& aPoint) const
{
  return aPoint.x;
}

ParentLayerCoord AxisX::GetRectLength(const ParentLayerRect& aRect) const
{
  return aRect.width;
}

ParentLayerCoord AxisX::GetRectOffset(const ParentLayerRect& aRect) const
{
  return aRect.x;
}

ScreenPoint AxisX::MakePoint(ScreenCoord aCoord) const
{
  return ScreenPoint(aCoord, 0);
}

AxisY::AxisY(AsyncPanZoomController* aAsyncPanZoomController)
  : Axis(aAsyncPanZoomController)
{

}

ParentLayerCoord AxisY::GetPointOffset(const ParentLayerPoint& aPoint) const
{
  return aPoint.y;
}

ParentLayerCoord AxisY::GetRectLength(const ParentLayerRect& aRect) const
{
  return aRect.height;
}

ParentLayerCoord AxisY::GetRectOffset(const ParentLayerRect& aRect) const
{
  return aRect.y;
}

ScreenPoint AxisY::MakePoint(ScreenCoord aCoord) const
{
  return ScreenPoint(0, aCoord);
}

}
}
