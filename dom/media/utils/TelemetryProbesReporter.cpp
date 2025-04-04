/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TelemetryProbesReporter.h"

#include <cmath>

#include "FrameStatistics.h"
#include "MediaCodecsSupport.h"
#include "VideoUtils.h"
#include "mozilla/EMEUtils.h"
#include "mozilla/Logging.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/glean/DomMediaEmeMetrics.h"
#include "mozilla/glean/DomMediaMetrics.h"
#include "mozilla/glean/DomMediaPlatformsWmfMetrics.h"
#include "nsThreadUtils.h"

namespace mozilla {

LazyLogModule gTelemetryProbesReporterLog("TelemetryProbesReporter");
#define LOG(msg, ...)                                   \
  MOZ_LOG(gTelemetryProbesReporterLog, LogLevel::Debug, \
          ("TelemetryProbesReporter=%p, " msg, this, ##__VA_ARGS__))

static const char* ToMutedStr(bool aMuted) {
  return aMuted ? "muted" : "unmuted";
}

MediaContent TelemetryProbesReporter::MediaInfoToMediaContent(
    const MediaInfo& aInfo) {
  MediaContent content = MediaContent::MEDIA_HAS_NOTHING;
  if (aInfo.HasAudio()) {
    content |= MediaContent::MEDIA_HAS_AUDIO;
  }
  if (aInfo.HasVideo()) {
    content |= MediaContent::MEDIA_HAS_VIDEO;
    if (aInfo.mVideo.GetAsVideoInfo()->mColorDepth > gfx::ColorDepth::COLOR_8) {
      content |= MediaContent::MEDIA_HAS_COLOR_DEPTH_ABOVE_8;
    }
  }
  return content;
}

TelemetryProbesReporter::TelemetryProbesReporter(
    TelemetryProbesReporterOwner* aOwner)
    : mOwner(aOwner) {
  MOZ_ASSERT(mOwner);
}

void TelemetryProbesReporter::OnPlay(Visibility aVisibility,
                                     MediaContent aMediaContent,
                                     bool aIsMuted) {
  LOG("Start time accumulation for total play time");

  AssertOnMainThreadAndNotShutdown();
  MOZ_ASSERT_IF(mMediaContent & MediaContent::MEDIA_HAS_VIDEO,
                !mTotalVideoPlayTime.IsStarted());
  MOZ_ASSERT_IF(mMediaContent & MediaContent::MEDIA_HAS_AUDIO,
                !mTotalAudioPlayTime.IsStarted());

  if (aMediaContent & MediaContent::MEDIA_HAS_VIDEO) {
    mTotalVideoPlayTime.Start();

    MOZ_ASSERT_IF(mMediaContent & MediaContent::MEDIA_HAS_COLOR_DEPTH_ABOVE_8,
                  !mTotalVideoHDRPlayTime.IsStarted());
    if (aMediaContent & MediaContent::MEDIA_HAS_COLOR_DEPTH_ABOVE_8) {
      mTotalVideoHDRPlayTime.Start();
    }
  }
  if (aMediaContent & MediaContent::MEDIA_HAS_AUDIO) {
    mTotalAudioPlayTime.Start();
  }

  OnMediaContentChanged(aMediaContent);
  OnVisibilityChanged(aVisibility);
  OnMutedChanged(aIsMuted);

  mOwner->DispatchAsyncTestingEvent(u"moztotalplaytimestarted"_ns);

  mIsPlaying = true;
}

void TelemetryProbesReporter::OnPause(Visibility aVisibility) {
  if (!mIsPlaying) {
    // Not started
    LOG("TelemetryProbesReporter::OnPause: not started, early return");
    return;
  }

  LOG("Pause time accumulation for total play time");

  AssertOnMainThreadAndNotShutdown();
  MOZ_ASSERT_IF(mMediaContent & MediaContent::MEDIA_HAS_VIDEO,
                mTotalVideoPlayTime.IsStarted());
  MOZ_ASSERT_IF(mMediaContent & MediaContent::MEDIA_HAS_AUDIO,
                mTotalAudioPlayTime.IsStarted());

  if (mMediaContent & MediaContent::MEDIA_HAS_VIDEO) {
    MOZ_ASSERT_IF(mMediaContent & MediaContent::MEDIA_HAS_COLOR_DEPTH_ABOVE_8,
                  mTotalVideoHDRPlayTime.IsStarted());

    LOG("Pause video time accumulation for total play time");
    if (mInvisibleVideoPlayTime.IsStarted()) {
      LOG("Pause invisible video time accumulation for total play time");
      PauseInvisibleVideoTimeAccumulator();
    }
    mTotalVideoPlayTime.Pause();
    mTotalVideoHDRPlayTime.Pause();
  }
  if (mMediaContent & MediaContent::MEDIA_HAS_AUDIO) {
    LOG("Pause audio time accumulation for total play time");
    if (mInaudibleAudioPlayTime.IsStarted()) {
      LOG("Pause audible audio time accumulation for total play time");
      PauseInaudibleAudioTimeAccumulator();
    }
    if (mMutedAudioPlayTime.IsStarted()) {
      LOG("Pause muted audio time accumulation for total play time");
      PauseMutedAudioTimeAccumulator();
    }
    mTotalAudioPlayTime.Pause();
  }

  mOwner->DispatchAsyncTestingEvent(u"moztotalplaytimepaused"_ns);
  ReportTelemetry();

  mIsPlaying = false;
}

void TelemetryProbesReporter::OnVisibilityChanged(Visibility aVisibility) {
  AssertOnMainThreadAndNotShutdown();
  LOG("Corresponding media element visibility change=%s -> %s",
      EnumValueToString(mMediaElementVisibility),
      EnumValueToString(aVisibility));
  if (aVisibility == Visibility::eInvisible) {
    StartInvisibleVideoTimeAccumulator();
  } else {
    if (aVisibility != Visibility::eInitial) {
      PauseInvisibleVideoTimeAccumulator();
    } else {
      LOG("Visibility was initial, not pausing.");
    }
  }
  mMediaElementVisibility = aVisibility;
}

void TelemetryProbesReporter::OnAudibleChanged(AudibleState aAudibleState) {
  AssertOnMainThreadAndNotShutdown();
  LOG("Audibility changed, now %s",
      dom::AudioChannelService::EnumValueToString(aAudibleState));
  if (aAudibleState == AudibleState::eNotAudible) {
    if (!mInaudibleAudioPlayTime.IsStarted()) {
      StartInaudibleAudioTimeAccumulator();
    }
  } else {
    // This happens when starting playback, no need to pause, because it hasn't
    // been started yet.
    if (mInaudibleAudioPlayTime.IsStarted()) {
      PauseInaudibleAudioTimeAccumulator();
    }
  }
}

void TelemetryProbesReporter::OnMutedChanged(bool aMuted) {
  // There are multiple ways to mute an element:
  // - volume = 0
  // - muted = true
  // - set the enabled property of the playing AudioTrack to false
  // Muted -> Muted "transisition" can therefore happen, and we can't add
  // asserts here.
  AssertOnMainThreadAndNotShutdown();
  if (!(mMediaContent & MediaContent::MEDIA_HAS_AUDIO)) {
    return;
  }
  LOG("Muted changed, was %s now %s", ToMutedStr(mIsMuted), ToMutedStr(aMuted));
  if (aMuted) {
    if (!mMutedAudioPlayTime.IsStarted()) {
      StartMutedAudioTimeAccumulator();
    }
  } else {
    // This happens when starting playback, no need to pause, because it hasn't
    // been started yet.
    if (mMutedAudioPlayTime.IsStarted()) {
      PauseMutedAudioTimeAccumulator();
    }
  }
  mIsMuted = aMuted;
}

void TelemetryProbesReporter::OnMediaContentChanged(MediaContent aContent) {
  AssertOnMainThreadAndNotShutdown();
  if (aContent == mMediaContent) {
    return;
  }
  if (mMediaContent & MediaContent::MEDIA_HAS_VIDEO &&
      !(aContent & MediaContent::MEDIA_HAS_VIDEO)) {
    LOG("Video track removed from media.");
    if (mInvisibleVideoPlayTime.IsStarted()) {
      PauseInvisibleVideoTimeAccumulator();
    }
    if (mTotalVideoPlayTime.IsStarted()) {
      mTotalVideoPlayTime.Pause();
      mTotalVideoHDRPlayTime.Pause();
    }
  }
  if (mMediaContent & MediaContent::MEDIA_HAS_AUDIO &&
      !(aContent & MediaContent::MEDIA_HAS_AUDIO)) {
    LOG("Audio track removed from media.");
    if (mTotalAudioPlayTime.IsStarted()) {
      mTotalAudioPlayTime.Pause();
    }
    if (mInaudibleAudioPlayTime.IsStarted()) {
      mInaudibleAudioPlayTime.Pause();
    }
    if (mMutedAudioPlayTime.IsStarted()) {
      mMutedAudioPlayTime.Pause();
    }
  }
  if (!(mMediaContent & MediaContent::MEDIA_HAS_VIDEO) &&
      aContent & MediaContent::MEDIA_HAS_VIDEO) {
    LOG("Video track added to media.");
    if (mIsPlaying) {
      mTotalVideoPlayTime.Start();
      if (mMediaElementVisibility == Visibility::eInvisible) {
        StartInvisibleVideoTimeAccumulator();
      }
    }
  }
  if (!(mMediaContent & MediaContent::MEDIA_HAS_COLOR_DEPTH_ABOVE_8) &&
      aContent & MediaContent::MEDIA_HAS_COLOR_DEPTH_ABOVE_8) {
    if (mIsPlaying) {
      mTotalVideoHDRPlayTime.Start();
    }
  }
  if (!(mMediaContent & MediaContent::MEDIA_HAS_AUDIO) &&
      aContent & MediaContent::MEDIA_HAS_AUDIO) {
    LOG("Audio track added to media.");
    if (mIsPlaying) {
      mTotalAudioPlayTime.Start();
      if (mIsMuted) {
        StartMutedAudioTimeAccumulator();
      }
    }
  }

  mMediaContent = aContent;
}

void TelemetryProbesReporter::OnFirstFrameLoaded(
    const double aLoadedFirstFrameTime, const double aLoadedMetadataTime,
    const double aTotalWaitingDataTime, const double aTotalBufferingTime,
    const FirstFrameLoadedFlagSet aFlags, const MediaInfo& aInfo,
    const nsCString& aVideoDecoderName) {
  MOZ_ASSERT(aInfo.HasVideo());
  nsCString resolution;
  DetermineResolutionForTelemetry(aInfo, resolution);

  const bool isMSE = aFlags.contains(FirstFrameLoadedFlag::IsMSE);
  const bool isExternalEngineStateMachine =
      aFlags.contains(FirstFrameLoadedFlag::IsExternalEngineStateMachine);

  glean::media_playback::FirstFrameLoadedExtra extraData;
  extraData.firstFrameLoadedTime = Some(aLoadedFirstFrameTime);
  extraData.metadataLoadedTime = Some(aLoadedMetadataTime);
  extraData.totalWaitingDataTime = Some(aTotalWaitingDataTime);
  extraData.bufferingTime = Some(aTotalBufferingTime);
  if (!isMSE && !isExternalEngineStateMachine) {
    extraData.playbackType = Some("Non-MSE playback"_ns);
  } else if (isMSE && !isExternalEngineStateMachine) {
    extraData.playbackType = !mOwner->IsEncrypted() ? Some("MSE playback"_ns)
                                                    : Some("EME playback"_ns);
  } else if (!isMSE && isExternalEngineStateMachine) {
    extraData.playbackType = Some("Non-MSE media-engine playback"_ns);
  } else if (isMSE && isExternalEngineStateMachine) {
    extraData.playbackType = !mOwner->IsEncrypted()
                                 ? Some("MSE media-engine playback"_ns)
                                 : Some("EME media-engine playback"_ns);
  } else {
    extraData.playbackType = Some("ERROR TYPE"_ns);
    MOZ_ASSERT(false, "Unexpected playback type!");
  }
  extraData.videoCodec = Some(aInfo.mVideo.mMimeType);
  extraData.resolution = Some(resolution);
  if (const auto keySystem = mOwner->GetKeySystem()) {
    extraData.keySystem = Some(NS_ConvertUTF16toUTF8(*keySystem));
  }
  extraData.isHardwareDecoding =
      Some(aFlags.contains(FirstFrameLoadedFlag::IsHardwareDecoding));

#ifdef MOZ_WIDGET_ANDROID
  if (aFlags.contains(FirstFrameLoadedFlag::IsHLS)) {
    extraData.hlsDecoder = Some(true);
  }
#endif

  extraData.decoderName = Some(aVideoDecoderName);
  extraData.isHdr = Some(static_cast<bool>(
      mMediaContent & MediaContent::MEDIA_HAS_COLOR_DEPTH_ABOVE_8));

  if (MOZ_LOG_TEST(gTelemetryProbesReporterLog, LogLevel::Debug)) {
    nsPrintfCString logMessage{
        "Media_Playabck First_Frame_Loaded event, time(ms)=["
        "full:%f, loading-meta:%f, waiting-data:%f, buffering:%f], "
        "playback-type=%s, "
        "videoCodec=%s, resolution=%s, hardwareAccelerated=%d, decoderName=%s, "
        "hdr=%d",
        aLoadedFirstFrameTime,
        aLoadedMetadataTime,
        aTotalWaitingDataTime,
        aTotalBufferingTime,
        extraData.playbackType->get(),
        extraData.videoCodec->get(),
        extraData.resolution->get(),
        aFlags.contains(FirstFrameLoadedFlag::IsHardwareDecoding),
        aVideoDecoderName.get(),
        *extraData.isHdr};
    if (const auto keySystem = mOwner->GetKeySystem()) {
      logMessage.AppendPrintf(", keySystem=%s",
                              NS_ConvertUTF16toUTF8(*keySystem).get());
    }
    LOG("%s", logMessage.get());
  }
  glean::media_playback::first_frame_loaded.Record(Some(extraData));
  mOwner->DispatchAsyncTestingEvent(u"mozfirstframeloadedprobe"_ns);
}

void TelemetryProbesReporter::OnShutdown() {
  AssertOnMainThreadAndNotShutdown();
  LOG("Shutdown");
  OnPause(Visibility::eInvisible);
  mOwner = nullptr;
}

void TelemetryProbesReporter::StartInvisibleVideoTimeAccumulator() {
  AssertOnMainThreadAndNotShutdown();
  if (!mTotalVideoPlayTime.IsStarted() || mInvisibleVideoPlayTime.IsStarted() ||
      !HasOwnerHadValidVideo()) {
    return;
  }
  LOG("Start time accumulation for invisible video");
  mInvisibleVideoPlayTime.Start();
  mOwner->DispatchAsyncTestingEvent(u"mozinvisibleplaytimestarted"_ns);
}

void TelemetryProbesReporter::PauseInvisibleVideoTimeAccumulator() {
  AssertOnMainThreadAndNotShutdown();
  if (!mInvisibleVideoPlayTime.IsStarted()) {
    return;
  }
  LOG("Pause time accumulation for invisible video");
  mInvisibleVideoPlayTime.Pause();
  mOwner->DispatchAsyncTestingEvent(u"mozinvisibleplaytimepaused"_ns);
}

void TelemetryProbesReporter::StartInaudibleAudioTimeAccumulator() {
  AssertOnMainThreadAndNotShutdown();
  MOZ_ASSERT(!mInaudibleAudioPlayTime.IsStarted());
  mInaudibleAudioPlayTime.Start();
  mOwner->DispatchAsyncTestingEvent(u"mozinaudibleaudioplaytimestarted"_ns);
}

void TelemetryProbesReporter::PauseInaudibleAudioTimeAccumulator() {
  AssertOnMainThreadAndNotShutdown();
  MOZ_ASSERT(mInaudibleAudioPlayTime.IsStarted());
  mInaudibleAudioPlayTime.Pause();
  mOwner->DispatchAsyncTestingEvent(u"mozinaudibleaudioplaytimepaused"_ns);
}

void TelemetryProbesReporter::StartMutedAudioTimeAccumulator() {
  AssertOnMainThreadAndNotShutdown();
  MOZ_ASSERT(!mMutedAudioPlayTime.IsStarted());
  mMutedAudioPlayTime.Start();
  mOwner->DispatchAsyncTestingEvent(u"mozmutedaudioplaytimestarted"_ns);
}

void TelemetryProbesReporter::PauseMutedAudioTimeAccumulator() {
  AssertOnMainThreadAndNotShutdown();
  MOZ_ASSERT(mMutedAudioPlayTime.IsStarted());
  mMutedAudioPlayTime.Pause();
  mOwner->DispatchAsyncTestingEvent(u"mozmutedeaudioplaytimepaused"_ns);
}

bool TelemetryProbesReporter::HasOwnerHadValidVideo() const {
  // Checking both image and display dimensions helps address cases such as
  // suspending, where we use a null decoder. In that case a null decoder
  // produces 0x0 video frames, which might cause layout to resize the display
  // size, but the image dimensions would be still non-null.
  const VideoInfo info = mOwner->GetMediaInfo().mVideo;
  return (info.mDisplay.height > 0 && info.mDisplay.width > 0) ||
         (info.mImage.height > 0 && info.mImage.width > 0);
}

bool TelemetryProbesReporter::HasOwnerHadValidMedia() const {
  return mMediaContent != MediaContent::MEDIA_HAS_NOTHING;
}

void TelemetryProbesReporter::AssertOnMainThreadAndNotShutdown() const {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mOwner, "Already shutdown?");
}

void TelemetryProbesReporter::ReportTelemetry() {
  AssertOnMainThreadAndNotShutdown();
  // ReportResultForAudio needs to be called first, because it can use the video
  // play time, that is reset in ReportResultForVideo.
  ReportResultForAudio();
  ReportResultForVideo();
  mOwner->DispatchAsyncTestingEvent(u"mozreportedtelemetry"_ns);
}

void TelemetryProbesReporter::ReportResultForVideo() {
  // We don't want to know the result for video without valid video frames.
  if (!HasOwnerHadValidVideo()) {
    return;
  }

  const double totalVideoPlayTimeS = mTotalVideoPlayTime.GetAndClearTotal();
  const double invisiblePlayTimeS = mInvisibleVideoPlayTime.GetAndClearTotal();
  const double totalVideoHDRPlayTimeS =
      mTotalVideoHDRPlayTime.GetAndClearTotal();

  // No need to report result for video that didn't start playing.
  if (totalVideoPlayTimeS == 0.0) {
    return;
  }
  MOZ_ASSERT(totalVideoPlayTimeS >= invisiblePlayTimeS);

  LOG("VIDEO_PLAY_TIME_S = %f", totalVideoPlayTimeS);
  glean::media::video_play_time.AccumulateRawDuration(
      TimeDuration::FromSeconds(totalVideoPlayTimeS));

  LOG("VIDEO_HIDDEN_PLAY_TIME_S = %f", invisiblePlayTimeS);
  glean::media::video_hidden_play_time.AccumulateRawDuration(
      TimeDuration::FromSeconds(invisiblePlayTimeS));

  // We only want to accumulate non-zero samples for HDR playback.
  // This is different from the other timings tracked here, but
  // we don't need 0-length play times to do our calculations.
  if (totalVideoHDRPlayTimeS > 0.0) {
    LOG("VIDEO_HDR_PLAY_TIME_S = %f", totalVideoHDRPlayTimeS);
    glean::media::video_hdr_play_time.AccumulateRawDuration(
        TimeDuration::FromSeconds(totalVideoHDRPlayTimeS));
  }

  if (mOwner->IsEncrypted()) {
    LOG("VIDEO_ENCRYPTED_PLAY_TIME_S = %f", totalVideoPlayTimeS);
    glean::media::video_encrypted_play_time.AccumulateRawDuration(
        TimeDuration::FromSeconds(totalVideoPlayTimeS));
  }

  // TODO: deprecate the old probes.
  // Report result for video using CDM
  auto keySystem = mOwner->GetKeySystem();
  if (keySystem) {
    if (IsClearkeyKeySystem(*keySystem)) {
      LOG("VIDEO_CLEARKEY_PLAY_TIME_S = %f", totalVideoPlayTimeS);
      glean::media::video_clearkey_play_time.AccumulateRawDuration(
          TimeDuration::FromSeconds(totalVideoPlayTimeS));

    } else if (IsWidevineKeySystem(*keySystem)) {
      LOG("VIDEO_WIDEVINE_PLAY_TIME_S = %f", totalVideoPlayTimeS);
      glean::media::video_widevine_play_time.AccumulateRawDuration(
          TimeDuration::FromSeconds(totalVideoPlayTimeS));
    }
  }

  // Keyed by audio+video or video alone, and by a resolution range.
  const MediaInfo& info = mOwner->GetMediaInfo();
  nsCString key;
  DetermineResolutionForTelemetry(info, key);

  auto visiblePlayTimeS = totalVideoPlayTimeS - invisiblePlayTimeS;
  LOG("VIDEO_VISIBLE_PLAY_TIME = %f, keys: '%s' and 'All'", visiblePlayTimeS,
      key.get());
  glean::media::video_visible_play_time.Get(key).AccumulateRawDuration(
      TimeDuration::FromSeconds(visiblePlayTimeS));
  // Also accumulate result in an "All" key.
  glean::media::video_visible_play_time.Get("All"_ns).AccumulateRawDuration(
      TimeDuration::FromSeconds(visiblePlayTimeS));

  const uint32_t hiddenPercentage =
      lround(invisiblePlayTimeS / totalVideoPlayTimeS * 100.0);
  glean::media::video_hidden_play_time_percentage.Get(key)
      .AccumulateSingleSample(hiddenPercentage);
  // Also accumulate all percentages in an "All" key.
  glean::media::video_hidden_play_time_percentage.Get("All"_ns)
      .AccumulateSingleSample(hiddenPercentage);
  LOG("VIDEO_HIDDEN_PLAY_TIME_PERCENTAGE = %u, keys: '%s' and 'All'",
      hiddenPercentage, key.get());

  ReportResultForVideoFrameStatistics(totalVideoPlayTimeS, key);
#ifdef MOZ_WMF_CDM
  if (mOwner->IsUsingWMFCDM()) {
    ReportResultForMFCDMPlaybackIfNeeded(totalVideoPlayTimeS, key);
  }
#endif
  if (keySystem) {
    ReportPlaytimeForKeySystem(*keySystem, totalVideoPlayTimeS,
                               info.mVideo.mMimeType, key);
  }
}

#ifdef MOZ_WMF_CDM
void TelemetryProbesReporter::ReportResultForMFCDMPlaybackIfNeeded(
    double aTotalPlayTimeS, const nsCString& aResolution) {
  const auto keySystem = mOwner->GetKeySystem();
  if (!keySystem) {
    NS_WARNING("Can not find key system to report telemetry for MFCDM!!");
    return;
  }
  glean::mfcdm::EmePlaybackExtra extraData;
  extraData.keySystem = Some(NS_ConvertUTF16toUTF8(*keySystem));
  extraData.videoCodec = Some(mOwner->GetMediaInfo().mVideo.mMimeType);
  extraData.resolution = Some(aResolution);
  extraData.playedTime = Some(aTotalPlayTimeS);

  Maybe<uint64_t> renderedFrames;
  Maybe<uint64_t> droppedFrames;
  if (auto* stats = mOwner->GetFrameStatistics()) {
    renderedFrames = Some(stats->GetPresentedFrames());
    droppedFrames = Some(stats->GetDroppedFrames());
    extraData.renderedFrames = Some(*renderedFrames);
    extraData.droppedFrames = Some(*droppedFrames);
  }
  if (MOZ_LOG_TEST(gTelemetryProbesReporterLog, LogLevel::Debug)) {
    nsPrintfCString logMessage{
        "MFCDM EME_Playback event, keySystem=%s, videoCodec=%s, resolution=%s, "
        "playedTime=%lf",
        NS_ConvertUTF16toUTF8(*keySystem).get(),
        mOwner->GetMediaInfo().mVideo.mMimeType.get(), aResolution.get(),
        aTotalPlayTimeS};
    if (renderedFrames) {
      logMessage.AppendPrintf(", renderedFrames=%" PRIu64, *renderedFrames);
    }
    if (droppedFrames) {
      logMessage.AppendPrintf(", droppedFrames=%" PRIu64, *droppedFrames);
    }
    LOG("%s", logMessage.get());
  }
  glean::mfcdm::eme_playback.Record(Some(extraData));
}
#endif

void TelemetryProbesReporter::ReportPlaytimeForKeySystem(
    const nsAString& aKeySystem, const double aTotalPlayTimeS,
    const nsCString& aCodec, const nsCString& aResolution) {
  glean::mediadrm::EmePlaybackExtra extra = {
      .keySystem = Some(NS_ConvertUTF16toUTF8(aKeySystem)),
      .playedTime = Some(aTotalPlayTimeS),
      .resolution = Some(aResolution),
      .videoCodec = Some(aCodec)};
  glean::mediadrm::eme_playback.Record(Some(extra));
}

void TelemetryProbesReporter::ReportResultForAudio() {
  // Don't record telemetry for a media that didn't have a valid audio or video
  // to play, or hasn't played.
  if (!HasOwnerHadValidMedia() || (mTotalAudioPlayTime.PeekTotal() == 0.0 &&
                                   mTotalVideoPlayTime.PeekTotal() == 0.0)) {
    return;
  }

  nsCString key;
  nsCString avKey;
  const double totalAudioPlayTimeS = mTotalAudioPlayTime.GetAndClearTotal();
  const double inaudiblePlayTimeS = mInaudibleAudioPlayTime.GetAndClearTotal();
  const double mutedPlayTimeS = mMutedAudioPlayTime.GetAndClearTotal();
  const double audiblePlayTimeS = totalAudioPlayTimeS - inaudiblePlayTimeS;
  const double unmutedPlayTimeS = totalAudioPlayTimeS - mutedPlayTimeS;
  const uint32_t audiblePercentage =
      lround(audiblePlayTimeS / totalAudioPlayTimeS * 100.0);
  const uint32_t unmutedPercentage =
      lround(unmutedPlayTimeS / totalAudioPlayTimeS * 100.0);
  const double totalVideoPlayTimeS = mTotalVideoPlayTime.PeekTotal();

  // Key semantics:
  // - AV: Audible audio + video
  // - IV: Inaudible audio + video
  // - MV: Muted audio + video
  // - A: Audible audio-only
  // - I: Inaudible audio-only
  // - M: Muted audio-only
  // - V: Video-only
  if (mMediaContent & MediaContent::MEDIA_HAS_AUDIO) {
    if (audiblePercentage == 0) {
      // Media element had an audio track, but it was inaudible throughout
      key.AppendASCII("I");
    } else if (unmutedPercentage == 0) {
      // Media element had an audio track, but it was muted throughout
      key.AppendASCII("M");
    } else {
      // Media element had an audible audio track
      key.AppendASCII("A");
    }
    avKey.AppendASCII("A");
  }
  if (mMediaContent & MediaContent::MEDIA_HAS_VIDEO) {
    key.AppendASCII("V");
    avKey.AppendASCII("V");
  }

  LOG("Key: %s", key.get());

  if (mMediaContent & MediaContent::MEDIA_HAS_AUDIO) {
    LOG("Audio:\ntotal: %lf\naudible: %lf\ninaudible: %lf\nmuted: "
        "%lf\npercentage audible: "
        "%u\npercentage unmuted: %u\n",
        totalAudioPlayTimeS, audiblePlayTimeS, inaudiblePlayTimeS,
        mutedPlayTimeS, audiblePercentage, unmutedPercentage);
    glean::media::media_play_time.Get(key).AccumulateRawDuration(
        TimeDuration::FromSeconds(totalAudioPlayTimeS));
    glean::media::muted_play_time_percent.Get(avKey).AccumulateSingleSample(
        100 - unmutedPercentage);
    glean::media::audible_play_time_percent.Get(avKey).AccumulateSingleSample(
        audiblePercentage);
  } else {
    MOZ_ASSERT(mMediaContent & MediaContent::MEDIA_HAS_VIDEO);
    glean::media::media_play_time.Get(key).AccumulateRawDuration(
        TimeDuration::FromSeconds(totalVideoPlayTimeS));
  }
}

void TelemetryProbesReporter::ReportResultForVideoFrameStatistics(
    double aTotalPlayTimeS, const nsCString& key) {
  FrameStatistics* stats = mOwner->GetFrameStatistics();
  if (!stats) {
    return;
  }

  const uint64_t parsedFrames = stats->GetParsedFrames();
  if (parsedFrames) {
    const uint64_t droppedFrames = stats->GetDroppedFrames();
    MOZ_ASSERT(droppedFrames <= parsedFrames);
    // Dropped frames <= total frames, so 'percentage' cannot be higher than
    // 100 and therefore can fit in a uint32_t (that Telemetry takes).
    const uint32_t percentage = 100 * droppedFrames / parsedFrames;
    LOG("DROPPED_FRAMES_IN_VIDEO_PLAYBACK = %u", percentage);
    glean::media::video_dropped_frames_proportion.AccumulateSingleSample(
        percentage);
    const uint32_t proportion = 10000 * droppedFrames / parsedFrames;
    glean::media::video_dropped_frames_proportion_exponential
        .AccumulateSingleSample(proportion);

    {
      const uint64_t droppedFrames = stats->GetDroppedDecodedFrames();
      const uint32_t proportion = 10000 * droppedFrames / parsedFrames;
      glean::media::video_dropped_decoded_frames_proportion_exponential
          .AccumulateSingleSample(proportion);
    }
    {
      const uint64_t droppedFrames = stats->GetDroppedSinkFrames();
      const uint32_t proportion = 10000 * droppedFrames / parsedFrames;
      glean::media::video_dropped_sink_frames_proportion_exponential
          .AccumulateSingleSample(proportion);
    }
    {
      const uint64_t droppedFrames = stats->GetDroppedCompositorFrames();
      const uint32_t proportion = 10000 * droppedFrames / parsedFrames;
      glean::media::video_dropped_compositor_frames_proportion_exponential
          .AccumulateSingleSample(proportion);
    }
  }
}

double TelemetryProbesReporter::GetTotalVideoPlayTimeInSeconds() const {
  return mTotalVideoPlayTime.PeekTotal();
}

double TelemetryProbesReporter::GetTotalVideoHDRPlayTimeInSeconds() const {
  return mTotalVideoHDRPlayTime.PeekTotal();
}

double TelemetryProbesReporter::GetVisibleVideoPlayTimeInSeconds() const {
  return GetTotalVideoPlayTimeInSeconds() -
         GetInvisibleVideoPlayTimeInSeconds();
}

double TelemetryProbesReporter::GetInvisibleVideoPlayTimeInSeconds() const {
  return mInvisibleVideoPlayTime.PeekTotal();
}

double TelemetryProbesReporter::GetTotalAudioPlayTimeInSeconds() const {
  return mTotalAudioPlayTime.PeekTotal();
}

double TelemetryProbesReporter::GetInaudiblePlayTimeInSeconds() const {
  return mInaudibleAudioPlayTime.PeekTotal();
}

double TelemetryProbesReporter::GetMutedPlayTimeInSeconds() const {
  return mMutedAudioPlayTime.PeekTotal();
}

double TelemetryProbesReporter::GetAudiblePlayTimeInSeconds() const {
  return GetTotalAudioPlayTimeInSeconds() - GetInaudiblePlayTimeInSeconds();
}

/*  static */
void TelemetryProbesReporter::ReportDeviceMediaCodecSupported(
    const media::MediaCodecsSupported& aSupported) {
  static bool sReported = false;
  if (sReported) {
    return;
  }
  MOZ_ASSERT(ContainHardwareCodecsSupported(aSupported));
  sReported = true;

  glean::media_playback::device_hardware_decoder_support.Get("h264"_ns).Set(
      aSupported.contains(
          mozilla::media::MediaCodecsSupport::H264HardwareDecode));
  glean::media_playback::device_hardware_decoder_support.Get("vp8"_ns).Set(
      aSupported.contains(
          mozilla::media::MediaCodecsSupport::VP8HardwareDecode));
  glean::media_playback::device_hardware_decoder_support.Get("vp9"_ns).Set(
      aSupported.contains(
          mozilla::media::MediaCodecsSupport::VP9HardwareDecode));
  glean::media_playback::device_hardware_decoder_support.Get("av1"_ns).Set(
      aSupported.contains(
          mozilla::media::MediaCodecsSupport::AV1HardwareDecode));
  glean::media_playback::device_hardware_decoder_support.Get("hevc"_ns).Set(
      aSupported.contains(
          mozilla::media::MediaCodecsSupport::HEVCHardwareDecode));
}

#undef LOG
}  // namespace mozilla
