// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_MOCK_AUDIO_DETECTOR_H_
#define POWER_MANAGER_MOCK_AUDIO_DETECTOR_H_

#include <gmock/gmock.h>

#include "power_manager/audio_detector_interface.h"

namespace power_manager {

class MockAudioDetector : public AudioDetectorInterface {
  public:
    MOCK_METHOD1(GetAudioStatus, bool(bool*));
};

}  // namespace power_manager

#endif // POWER_MANAGER_MOCK_AUDIO_DETECTOR_H_
