/*
 *  Copyright 2020 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "call/adaptation/video_stream_adapter.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "absl/types/optional.h"
#include "api/video/video_adaptation_reason.h"
#include "api/video_codecs/video_encoder.h"
#include "rtc_base/constructor_magic.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/safe_conversions.h"

namespace webrtc {

const int kMinFrameRateFps = 2;

namespace {

// Generate suggested higher and lower frame rates and resolutions, to be
// applied to the VideoSourceRestrictor. These are used in "maintain-resolution"
// and "maintain-framerate". The "balanced" degradation preference also makes
// use of BalancedDegradationPreference when generating suggestions. The
// VideoSourceRestrictor decidedes whether or not a proposed adaptation is
// valid.

// For frame rate, the steps we take are 2/3 (down) and 3/2 (up).
int GetLowerFrameRateThan(int fps) {
  RTC_DCHECK(fps != std::numeric_limits<int>::max());
  return (fps * 2) / 3;
}
// TODO(hbos): Use absl::optional<> instead?
int GetHigherFrameRateThan(int fps) {
  return fps != std::numeric_limits<int>::max()
             ? (fps * 3) / 2
             : std::numeric_limits<int>::max();
}

// For resolution, the steps we take are 3/5 (down) and 5/3 (up).
// Notice the asymmetry of which restriction property is set depending on if
// we are adapting up or down:
// - VideoSourceRestrictor::DecreaseResolution() sets the max_pixels_per_frame()
//   to the desired target and target_pixels_per_frame() to null.
// - VideoSourceRestrictor::IncreaseResolutionTo() sets the
//   target_pixels_per_frame() to the desired target, and max_pixels_per_frame()
//   is set according to VideoSourceRestrictor::GetIncreasedMaxPixelsWanted().
int GetLowerResolutionThan(int pixel_count) {
  RTC_DCHECK(pixel_count != std::numeric_limits<int>::max());
  return (pixel_count * 3) / 5;
}

}  // namespace

VideoSourceRestrictionsListener::~VideoSourceRestrictionsListener() = default;

VideoSourceRestrictions FilterRestrictionsByDegradationPreference(
    VideoSourceRestrictions source_restrictions,
    DegradationPreference degradation_preference) {
  switch (degradation_preference) {
    case DegradationPreference::BALANCED:
      break;
    case DegradationPreference::MAINTAIN_FRAMERATE:
      source_restrictions.set_max_frame_rate(absl::nullopt);
      break;
    case DegradationPreference::MAINTAIN_RESOLUTION:
      source_restrictions.set_max_pixels_per_frame(absl::nullopt);
      source_restrictions.set_target_pixels_per_frame(absl::nullopt);
      break;
    case DegradationPreference::DISABLED:
      source_restrictions.set_max_pixels_per_frame(absl::nullopt);
      source_restrictions.set_target_pixels_per_frame(absl::nullopt);
      source_restrictions.set_max_frame_rate(absl::nullopt);
  }
  return source_restrictions;
}

// TODO(hbos): Use absl::optional<> instead?
int GetHigherResolutionThan(int pixel_count) {
  return pixel_count != std::numeric_limits<int>::max()
             ? (pixel_count * 5) / 3
             : std::numeric_limits<int>::max();
}

// static
const char* Adaptation::StatusToString(Adaptation::Status status) {
  switch (status) {
    case Adaptation::Status::kValid:
      return "kValid";
    case Adaptation::Status::kLimitReached:
      return "kLimitReached";
    case Adaptation::Status::kAwaitingPreviousAdaptation:
      return "kAwaitingPreviousAdaptation";
    case Status::kInsufficientInput:
      return "kInsufficientInput";
  }
}

Adaptation::Step::Step(StepType type, int target) : type(type), target(target) {
  RTC_DCHECK_NE(type, Adaptation::StepType::kForce);
}

Adaptation::Step::Step(VideoSourceRestrictions restrictions,
                       VideoAdaptationCounters counters)
    : type(Adaptation::StepType::kForce),
      restrictions(restrictions),
      counters(counters) {}

Adaptation::Adaptation(int validation_id,
                       Step step,
                       VideoStreamInputState input_state)
    : validation_id_(validation_id),
      status_(Status::kValid),
      step_(std::move(step)),
      min_pixel_limit_reached_(false),
      input_state_(input_state) {}

Adaptation::Adaptation(int validation_id,
                       Step step,
                       VideoStreamInputState input_state,
                       bool min_pixel_limit_reached)
    : validation_id_(validation_id),
      status_(Status::kValid),
      step_(std::move(step)),
      min_pixel_limit_reached_(min_pixel_limit_reached),
      input_state_(input_state) {}

Adaptation::Adaptation(int validation_id,
                       Status invalid_status,
                       VideoStreamInputState input_state)
    : validation_id_(validation_id),
      status_(invalid_status),
      step_(absl::nullopt),
      min_pixel_limit_reached_(false),
      input_state_(input_state) {
  RTC_DCHECK_NE(status_, Status::kValid);
}

Adaptation::Adaptation(int validation_id,
                       Status invalid_status,
                       VideoStreamInputState input_state,
                       bool min_pixel_limit_reached)
    : validation_id_(validation_id),
      status_(invalid_status),
      step_(absl::nullopt),
      min_pixel_limit_reached_(min_pixel_limit_reached),
      input_state_(input_state) {
  RTC_DCHECK_NE(status_, Status::kValid);
}

Adaptation::Status Adaptation::status() const {
  return status_;
}

bool Adaptation::min_pixel_limit_reached() const {
  return min_pixel_limit_reached_;
}

const Adaptation::Step& Adaptation::step() const {
  RTC_DCHECK_EQ(status_, Status::kValid);
  return step_.value();
}
const VideoStreamInputState& Adaptation::input_state() const {
  return input_state_;
}

// VideoSourceRestrictor is responsible for keeping track of current
// VideoSourceRestrictions.
class VideoStreamAdapter::VideoSourceRestrictor {
 public:
  VideoSourceRestrictor() {}

  VideoSourceRestrictions source_restrictions() const {
    return source_restrictions_;
  }
  const VideoAdaptationCounters& adaptation_counters() const {
    return adaptations_;
  }
  void ClearRestrictions() {
    source_restrictions_ = VideoSourceRestrictions();
    adaptations_ = VideoAdaptationCounters();
  }

  void ForceRestrictions(const VideoSourceRestrictions& restrictions,
                         const VideoAdaptationCounters& counters) {
    source_restrictions_ = restrictions;
    adaptations_ = counters;
  }

  void set_min_pixels_per_frame(int min_pixels_per_frame) {
    min_pixels_per_frame_ = min_pixels_per_frame;
  }

  int min_pixels_per_frame() const { return min_pixels_per_frame_; }

  bool CanDecreaseResolutionTo(int target_pixels) const {
    int max_pixels_per_frame = rtc::dchecked_cast<int>(
        source_restrictions_.max_pixels_per_frame().value_or(
            std::numeric_limits<int>::max()));
    return target_pixels < max_pixels_per_frame &&
           target_pixels >= min_pixels_per_frame_;
  }

  bool CanIncreaseResolutionTo(int target_pixels) const {
    int max_pixels_wanted = GetIncreasedMaxPixelsWanted(target_pixels);
    int max_pixels_per_frame = rtc::dchecked_cast<int>(
        source_restrictions_.max_pixels_per_frame().value_or(
            std::numeric_limits<int>::max()));
    return max_pixels_wanted > max_pixels_per_frame;
  }

  bool CanDecreaseFrameRateTo(int max_frame_rate) const {
    const int fps_wanted = std::max(kMinFrameRateFps, max_frame_rate);
    return fps_wanted < rtc::dchecked_cast<int>(
                            source_restrictions_.max_frame_rate().value_or(
                                std::numeric_limits<int>::max()));
  }

  bool CanIncreaseFrameRateTo(int max_frame_rate) const {
    return max_frame_rate > rtc::dchecked_cast<int>(
                                source_restrictions_.max_frame_rate().value_or(
                                    std::numeric_limits<int>::max()));
  }

  void ApplyAdaptationStep(const Adaptation::Step& step,
                           DegradationPreference degradation_preference) {
    switch (step.type) {
      case Adaptation::StepType::kIncreaseResolution:
        RTC_DCHECK(step.target);
        IncreaseResolutionTo(step.target.value());
        break;
      case Adaptation::StepType::kDecreaseResolution:
        RTC_DCHECK(step.target);
        DecreaseResolutionTo(step.target.value());
        break;
      case Adaptation::StepType::kIncreaseFrameRate:
        RTC_DCHECK(step.target);
        IncreaseFrameRateTo(step.target.value());
        // TODO(https://crbug.com/webrtc/11222): Don't adapt in two steps.
        // GetAdaptationUp() should tell us the correct value, but BALANCED
        // logic in DecrementFramerate() makes it hard to predict whether this
        // will be the last step. Remove the dependency on
        // adaptation_counters().
        if (degradation_preference == DegradationPreference::BALANCED &&
            adaptation_counters().fps_adaptations == 0 &&
            step.target != std::numeric_limits<int>::max()) {
          RTC_LOG(LS_INFO) << "Removing framerate down-scaling setting.";
          IncreaseFrameRateTo(std::numeric_limits<int>::max());
        }
        break;
      case Adaptation::StepType::kDecreaseFrameRate:
        RTC_DCHECK(step.target);
        DecreaseFrameRateTo(step.target.value());
        break;
      case Adaptation::StepType::kForce:
        RTC_DCHECK(step.restrictions);
        RTC_DCHECK(step.counters);
        ForceRestrictions(step.restrictions.value(), step.counters.value());
        break;
    }
  }

 private:
  static int GetIncreasedMaxPixelsWanted(int target_pixels) {
    if (target_pixels == std::numeric_limits<int>::max())
      return std::numeric_limits<int>::max();
    // When we decrease resolution, we go down to at most 3/5 of current pixels.
    // Thus to increase resolution, we need 3/5 to get back to where we started.
    // When going up, the desired max_pixels_per_frame() has to be significantly
    // higher than the target because the source's native resolutions might not
    // match the target. We pick 12/5 of the target.
    //
    // (This value was historically 4 times the old target, which is (3/5)*4 of
    // the new target - or 12/5 - assuming the target is adjusted according to
    // the above steps.)
    RTC_DCHECK(target_pixels != std::numeric_limits<int>::max());
    return (target_pixels * 12) / 5;
  }

  void DecreaseResolutionTo(int target_pixels) {
    RTC_DCHECK(CanDecreaseResolutionTo(target_pixels));
    RTC_LOG(LS_INFO) << "Scaling down resolution, max pixels: "
                     << target_pixels;
    source_restrictions_.set_max_pixels_per_frame(
        target_pixels != std::numeric_limits<int>::max()
            ? absl::optional<size_t>(target_pixels)
            : absl::nullopt);
    source_restrictions_.set_target_pixels_per_frame(absl::nullopt);
    ++adaptations_.resolution_adaptations;
  }

  void IncreaseResolutionTo(int target_pixels) {
    RTC_DCHECK(CanIncreaseResolutionTo(target_pixels));
    int max_pixels_wanted = GetIncreasedMaxPixelsWanted(target_pixels);
    RTC_LOG(LS_INFO) << "Scaling up resolution, max pixels: "
                     << max_pixels_wanted;
    source_restrictions_.set_max_pixels_per_frame(
        max_pixels_wanted != std::numeric_limits<int>::max()
            ? absl::optional<size_t>(max_pixels_wanted)
            : absl::nullopt);
    source_restrictions_.set_target_pixels_per_frame(
        max_pixels_wanted != std::numeric_limits<int>::max()
            ? absl::optional<size_t>(target_pixels)
            : absl::nullopt);
    --adaptations_.resolution_adaptations;
    RTC_DCHECK_GE(adaptations_.resolution_adaptations, 0);
  }

  void DecreaseFrameRateTo(int max_frame_rate) {
    RTC_DCHECK(CanDecreaseFrameRateTo(max_frame_rate));
    max_frame_rate = std::max(kMinFrameRateFps, max_frame_rate);
    RTC_LOG(LS_INFO) << "Scaling down framerate: " << max_frame_rate;
    source_restrictions_.set_max_frame_rate(
        max_frame_rate != std::numeric_limits<int>::max()
            ? absl::optional<double>(max_frame_rate)
            : absl::nullopt);
    ++adaptations_.fps_adaptations;
  }

  void IncreaseFrameRateTo(int max_frame_rate) {
    RTC_DCHECK(CanIncreaseFrameRateTo(max_frame_rate));
    RTC_LOG(LS_INFO) << "Scaling up framerate: " << max_frame_rate;
    source_restrictions_.set_max_frame_rate(
        max_frame_rate != std::numeric_limits<int>::max()
            ? absl::optional<double>(max_frame_rate)
            : absl::nullopt);
    --adaptations_.fps_adaptations;
    RTC_DCHECK_GE(adaptations_.fps_adaptations, 0);
  }

  // Needed by CanDecreaseResolutionTo().
  int min_pixels_per_frame_ = 0;
  // Current State.
  VideoSourceRestrictions source_restrictions_;
  VideoAdaptationCounters adaptations_;
};

VideoStreamAdapter::VideoStreamAdapter(
    VideoStreamInputStateProvider* input_state_provider)
    : source_restrictor_(std::make_unique<VideoSourceRestrictor>()),
      input_state_provider_(input_state_provider),
      balanced_settings_(),
      adaptation_validation_id_(0),
      degradation_preference_(DegradationPreference::DISABLED),
      last_adaptation_request_(absl::nullopt),
      last_video_source_restrictions_() {
  sequence_checker_.Detach();
}

VideoStreamAdapter::~VideoStreamAdapter() {}

VideoSourceRestrictions VideoStreamAdapter::source_restrictions() const {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  return source_restrictor_->source_restrictions();
}

const VideoAdaptationCounters& VideoStreamAdapter::adaptation_counters() const {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  return source_restrictor_->adaptation_counters();
}

void VideoStreamAdapter::ClearRestrictions() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  // Invalidate any previously returned Adaptation.
  RTC_LOG(INFO) << "Resetting restrictions";
  ++adaptation_validation_id_;
  source_restrictor_->ClearRestrictions();
  last_adaptation_request_.reset();
  BroadcastVideoRestrictionsUpdate(nullptr);
}

void VideoStreamAdapter::AddRestrictionsListener(
    VideoSourceRestrictionsListener* restrictions_listener) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  RTC_DCHECK(std::find(restrictions_listeners_.begin(),
                       restrictions_listeners_.end(),
                       restrictions_listener) == restrictions_listeners_.end());
  restrictions_listeners_.push_back(restrictions_listener);
}

void VideoStreamAdapter::RemoveRestrictionsListener(
    VideoSourceRestrictionsListener* restrictions_listener) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  auto it = std::find(restrictions_listeners_.begin(),
                      restrictions_listeners_.end(), restrictions_listener);
  RTC_DCHECK(it != restrictions_listeners_.end());
  restrictions_listeners_.erase(it);
}

void VideoStreamAdapter::SetDegradationPreference(
    DegradationPreference degradation_preference) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  if (degradation_preference_ == degradation_preference)
    return;
  // Invalidate any previously returned Adaptation.
  ++adaptation_validation_id_;
  bool balanced_switch =
      degradation_preference == DegradationPreference::BALANCED ||
      degradation_preference_ == DegradationPreference::BALANCED;
  degradation_preference_ = degradation_preference;
  if (balanced_switch) {
    // ClearRestrictions() calls BroadcastVideoRestrictionsUpdate(nullptr).
    ClearRestrictions();
  } else {
    BroadcastVideoRestrictionsUpdate(nullptr);
  }
}

Adaptation VideoStreamAdapter::GetAdaptationUp() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  RTC_DCHECK_NE(degradation_preference_, DegradationPreference::DISABLED);
  VideoStreamInputState input_state = input_state_provider_->InputState();
  ++adaptation_validation_id_;
  if (!HasSufficientInputForAdaptation(input_state)) {
    return Adaptation(adaptation_validation_id_,
                      Adaptation::Status::kInsufficientInput, input_state);
  }
  source_restrictor_->set_min_pixels_per_frame(
      input_state.min_pixels_per_frame());
  // Don't adapt if we're awaiting a previous adaptation to have an effect.
  bool last_request_increased_resolution =
      last_adaptation_request_ && last_adaptation_request_->step_type_ ==
                                      Adaptation::StepType::kIncreaseResolution;
  if (last_request_increased_resolution &&
      degradation_preference_ == DegradationPreference::MAINTAIN_FRAMERATE &&
      input_state.frame_size_pixels().value() <=
          last_adaptation_request_->input_pixel_count_) {
    return Adaptation(adaptation_validation_id_,
                      Adaptation::Status::kAwaitingPreviousAdaptation,
                      input_state);
  }

  // Maybe propose targets based on degradation preference.
  switch (degradation_preference_) {
    case DegradationPreference::BALANCED: {
      // Attempt to increase target frame rate.
      int target_fps =
          balanced_settings_.MaxFps(input_state.video_codec_type(),
                                    input_state.frame_size_pixels().value());
      if (source_restrictor_->CanIncreaseFrameRateTo(target_fps)) {
        return Adaptation(
            adaptation_validation_id_,
            Adaptation::Step(Adaptation::StepType::kIncreaseFrameRate,
                             target_fps),
            input_state);
      }
      // Scale up resolution.
      ABSL_FALLTHROUGH_INTENDED;
    }
    case DegradationPreference::MAINTAIN_FRAMERATE: {
      // Attempt to increase pixel count.
      int target_pixels = input_state.frame_size_pixels().value();
      if (source_restrictor_->adaptation_counters().resolution_adaptations ==
          1) {
        RTC_LOG(LS_INFO) << "Removing resolution down-scaling setting.";
        target_pixels = std::numeric_limits<int>::max();
      }
      target_pixels = GetHigherResolutionThan(target_pixels);
      if (!source_restrictor_->CanIncreaseResolutionTo(target_pixels)) {
        return Adaptation(adaptation_validation_id_,
                          Adaptation::Status::kLimitReached, input_state);
      }
      return Adaptation(
          adaptation_validation_id_,
          Adaptation::Step(Adaptation::StepType::kIncreaseResolution,
                           target_pixels),
          input_state);
    }
    case DegradationPreference::MAINTAIN_RESOLUTION: {
      // Scale up framerate.
      int target_fps = input_state.frames_per_second();
      if (source_restrictor_->adaptation_counters().fps_adaptations == 1) {
        RTC_LOG(LS_INFO) << "Removing framerate down-scaling setting.";
        target_fps = std::numeric_limits<int>::max();
      }
      target_fps = GetHigherFrameRateThan(target_fps);
      if (!source_restrictor_->CanIncreaseFrameRateTo(target_fps)) {
        return Adaptation(adaptation_validation_id_,
                          Adaptation::Status::kLimitReached, input_state);
      }
      return Adaptation(
          adaptation_validation_id_,
          Adaptation::Step(Adaptation::StepType::kIncreaseFrameRate,
                           target_fps),
          input_state);
    }
    case DegradationPreference::DISABLED:
      RTC_NOTREACHED();
      return Adaptation(adaptation_validation_id_,
                        Adaptation::Status::kLimitReached, input_state);
  }
}

Adaptation VideoStreamAdapter::GetAdaptationDown() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  RTC_DCHECK_NE(degradation_preference_, DegradationPreference::DISABLED);
  VideoStreamInputState input_state = input_state_provider_->InputState();
  ++adaptation_validation_id_;
  if (!HasSufficientInputForAdaptation(input_state)) {
    return Adaptation(adaptation_validation_id_,
                      Adaptation::Status::kInsufficientInput, input_state);
  }
  source_restrictor_->set_min_pixels_per_frame(
      input_state.min_pixels_per_frame());
  // Don't adapt if we're awaiting a previous adaptation to have an effect or
  // if we switched degradation preference.
  bool last_request_decreased_resolution =
      last_adaptation_request_ && last_adaptation_request_->step_type_ ==
                                      Adaptation::StepType::kDecreaseResolution;
  if (last_request_decreased_resolution &&
      degradation_preference_ == DegradationPreference::MAINTAIN_FRAMERATE &&
      input_state.frame_size_pixels().value() >=
          last_adaptation_request_->input_pixel_count_) {
    return Adaptation(adaptation_validation_id_,
                      Adaptation::Status::kAwaitingPreviousAdaptation,
                      input_state);
  }

  // Maybe propose targets based on degradation preference.
  switch (degradation_preference_) {
    case DegradationPreference::BALANCED: {
      // Try scale down framerate, if lower.
      int target_fps =
          balanced_settings_.MinFps(input_state.video_codec_type(),
                                    input_state.frame_size_pixels().value());
      if (source_restrictor_->CanDecreaseFrameRateTo(target_fps)) {
        return Adaptation(
            adaptation_validation_id_,
            Adaptation::Step(Adaptation::StepType::kDecreaseFrameRate,
                             target_fps),
            input_state);
      }
      // Scale down resolution.
      ABSL_FALLTHROUGH_INTENDED;
    }
    case DegradationPreference::MAINTAIN_FRAMERATE: {
      // Scale down resolution.
      int target_pixels =
          GetLowerResolutionThan(input_state.frame_size_pixels().value());
      bool min_pixel_limit_reached =
          target_pixels < source_restrictor_->min_pixels_per_frame();
      if (!source_restrictor_->CanDecreaseResolutionTo(target_pixels)) {
        return Adaptation(adaptation_validation_id_,
                          Adaptation::Status::kLimitReached, input_state,
                          min_pixel_limit_reached);
      }
      return Adaptation(
          adaptation_validation_id_,
          Adaptation::Step(Adaptation::StepType::kDecreaseResolution,
                           target_pixels),
          input_state, min_pixel_limit_reached);
    }
    case DegradationPreference::MAINTAIN_RESOLUTION: {
      int target_fps = GetLowerFrameRateThan(input_state.frames_per_second());
      if (!source_restrictor_->CanDecreaseFrameRateTo(target_fps)) {
        return Adaptation(adaptation_validation_id_,
                          Adaptation::Status::kLimitReached, input_state);
      }
      return Adaptation(
          adaptation_validation_id_,
          Adaptation::Step(Adaptation::StepType::kDecreaseFrameRate,
                           target_fps),
          input_state);
    }
    case DegradationPreference::DISABLED:
      RTC_NOTREACHED();
      return Adaptation(adaptation_validation_id_,
                        Adaptation::Status::kLimitReached, input_state);
  }
}

VideoStreamAdapter::RestrictionsWithCounters
VideoStreamAdapter::PeekNextRestrictions(const Adaptation& adaptation) const {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  RTC_DCHECK_EQ(adaptation.validation_id_, adaptation_validation_id_);
  RTC_LOG(LS_INFO) << "PeekNextRestrictions called";
  if (adaptation.status() != Adaptation::Status::kValid) {
    return {source_restrictor_->source_restrictions(),
            source_restrictor_->adaptation_counters()};
  }
  VideoSourceRestrictor restrictor_copy = *source_restrictor_;
  restrictor_copy.ApplyAdaptationStep(adaptation.step(),
                                      degradation_preference_);
  return {restrictor_copy.source_restrictions(),
          restrictor_copy.adaptation_counters()};
}

void VideoStreamAdapter::ApplyAdaptation(
    const Adaptation& adaptation,
    rtc::scoped_refptr<Resource> resource) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  RTC_DCHECK_EQ(adaptation.validation_id_, adaptation_validation_id_);
  RTC_LOG(LS_INFO) << "ApplyAdaptation called";
  if (adaptation.status() != Adaptation::Status::kValid)
    return;
  // Remember the input pixels and fps of this adaptation. Used to avoid
  // adapting again before this adaptation has had an effect.
  last_adaptation_request_.emplace(AdaptationRequest{
      adaptation.input_state_.frame_size_pixels().value(),
      adaptation.input_state_.frames_per_second(), adaptation.step().type});
  // Adapt!
  source_restrictor_->ApplyAdaptationStep(adaptation.step(),
                                          degradation_preference_);
  BroadcastVideoRestrictionsUpdate(resource);
}

Adaptation VideoStreamAdapter::GetAdaptationTo(
    const VideoAdaptationCounters& counters,
    const VideoSourceRestrictions& restrictions) {
  // Adapts up/down from the current levels so counters are equal.
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  VideoStreamInputState input_state = input_state_provider_->InputState();
  return Adaptation(adaptation_validation_id_,
                    Adaptation::Step(restrictions, counters), input_state);
}

void VideoStreamAdapter::BroadcastVideoRestrictionsUpdate(
    const rtc::scoped_refptr<Resource>& resource) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  VideoSourceRestrictions filtered = FilterRestrictionsByDegradationPreference(
      source_restrictions(), degradation_preference_);
  if (last_filtered_restrictions_ == filtered) {
    return;
  }
  for (auto* restrictions_listener : restrictions_listeners_) {
    restrictions_listener->OnVideoSourceRestrictionsUpdated(
        filtered, source_restrictor_->adaptation_counters(), resource,
        source_restrictions());
  }
  last_video_source_restrictions_ = source_restrictor_->source_restrictions();
  last_filtered_restrictions_ = filtered;
}

bool VideoStreamAdapter::HasSufficientInputForAdaptation(
    const VideoStreamInputState& input_state) const {
  return input_state.HasInputFrameSizeAndFramesPerSecond() &&
         (degradation_preference_ !=
              DegradationPreference::MAINTAIN_RESOLUTION ||
          input_state.frames_per_second() >= kMinFrameRateFps);
}

}  // namespace webrtc
