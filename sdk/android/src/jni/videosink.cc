/*
 *  Copyright 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "sdk/android/src/jni/videosink.h"

#include "sdk/android/generated_video_jni/jni/VideoSink_jni.h"
#include "sdk/android/src/jni/videoframe.h"

namespace webrtc {
namespace jni {

VideoSinkWrapper::VideoSinkWrapper(JNIEnv* jni, const JavaRef<jobject>& j_sink)
    : j_sink_(jni, j_sink) {}

VideoSinkWrapper::~VideoSinkWrapper() {}

void VideoSinkWrapper::OnFrame(const VideoFrame& frame) {
  JNIEnv* jni = AttachCurrentThreadIfNeeded();
  Java_VideoSink_onFrame(jni, j_sink_, NativeToJavaFrame(jni, frame));
}

}  // namespace jni
}  // namespace webrtc
