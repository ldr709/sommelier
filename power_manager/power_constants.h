// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWER_CONSTANTS_H_
#define POWER_MANAGER_POWER_CONSTANTS_H_

namespace power_manager {

// Preference names.
extern const char kPluggedBrightnessOffsetPref[];
extern const char kUnpluggedBrightnessOffsetPref[];
extern const char kLowBatterySuspendTimePref[];
extern const char kCleanShutdownTimeoutMsPref[];
extern const char kPluggedDimMsPref[];
extern const char kPluggedOffMsPref[];
extern const char kUnpluggedDimMsPref[];
extern const char kUnpluggedOffMsPref[];
extern const char kUnpluggedSuspendMsPref[];
extern const char kEnforceLockPref[];
extern const char kDisableIdleSuspendPref[];
extern const char kUseLidPref[];
extern const char kLockOnIdleSuspendPref[];
extern const char kLockMsPref[];
extern const char kRetrySuspendMsPref[];
extern const char kRetrySuspendAttemptsPref[];
extern const char kPluggedSuspendMsPref[];
extern const char kMinVisibleBacklightLevelPref[];
extern const char kDisableALSPref[];
extern const char kWakeupInputPref[];
extern const char kReactMsPref[];
extern const char kFuzzMsPref[];
extern const char kStateMaxDisabledDurationSecPref[];

// Miscellaneous constants.
extern const char kBacklightPath[];
extern const char kBacklightPattern[];
extern const char kKeyboardBacklightPath[];
extern const char kKeyboardBacklightPattern[];

// Interface names.
extern const char kRootPowerManagerInterface[];
extern const char kRootPowerManagerServiceName[];

// powerd -> powerm constants.
extern const char kCheckLidStateSignal[];
extern const char kRestartSignal[];
extern const char kRequestCleanShutdown[];
extern const char kSuspendSignal[];
extern const char kShutdownSignal[];
extern const char kExternalBacklightGetMethod[];
extern const char kExternalBacklightSetMethod[];

// powerm -> powerd constants.
extern const char kLidClosed[];
extern const char kLidOpened[];
extern const char kPowerButtonDown[];
extern const char kPowerButtonUp[];
extern const char kExternalBacklightUpdate[];
extern const char kKeyLeftCtrl[];
extern const char kKeyRightCtrl[];
extern const char kKeyLeftAlt[];
extern const char kKeyRightAlt[];
extern const char kKeyLeftShift[];
extern const char kKeyRightShift[];
extern const char kKeyF4[];

// Broadcast signals.
extern const char kPowerStateChanged[];

// Files to signal powerd_suspend whether suspend should be cancelled.
extern const char kLidOpenFile[];
extern const char kUserActiveFile[];

// Control parameters for the battery time rolling averages
extern const unsigned int kRollingAverageSampleWindowMax;
extern const unsigned int kRollingAverageSampleWindowMin;
extern const unsigned int kRollingAverageSampleWindowDiff;
extern const unsigned int kRollingAverageTaperTimeMax;
extern const unsigned int kRollingAverageTaperTimeMin;
extern const unsigned int kRollingAverageTaperTimeDiff;

}  // namespace power_manager

#endif  // POWER_MANAGER_POWER_CONSTANTS_H_
