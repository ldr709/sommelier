// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_DAEMON_H_
#define POWER_MANAGER_POWERD_DAEMON_H_
#pragma once

#include <dbus/dbus-glib-lowlevel.h>
#include <glib.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <ctime>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/file_path.h"
#include "base/memory/scoped_ptr.h"
#include "base/time.h"
#include "power_manager/common/dbus_handler.h"
#include "power_manager/common/prefs_observer.h"
#include "power_manager/powerd/policy/backlight_controller_observer.h"
#include "power_manager/powerd/policy/dark_resume_policy.h"
#include "power_manager/powerd/policy/input_controller.h"
#include "power_manager/powerd/system/audio_observer.h"
#include "power_manager/powerd/system/peripheral_battery_watcher.h"
#include "power_manager/powerd/system/power_supply.h"
#include "power_manager/powerd/system/power_supply_observer.h"

class MetricsLibrary;

namespace power_manager {

class DBusSender;
class MetricsReporter;
class Prefs;

namespace policy {
class BacklightController;
class KeyboardBacklightController;
class StateController;
class Suspender;
}  // namespace policy

namespace system {
class AmbientLightSensor;
class AudioClient;
class DisplayPowerSetter;
class Input;
class InternalBacklight;
class Udev;
}  // namespace system

// Main class within the powerd daemon that ties all other classes together.
class Daemon : public policy::BacklightControllerObserver,
               public policy::InputController::Delegate,
               public system::AudioObserver,
               public system::PowerSupplyObserver {
 public:
  Daemon(const base::FilePath& read_write_prefs_dir,
         const base::FilePath& read_only_prefs_dir,
         const base::FilePath& run_dir);
  virtual ~Daemon();

  void Init();

  // Overridden from policy::BacklightControllerObserver:
  virtual void OnBrightnessChanged(
      double brightness_percent,
      policy::BacklightController::BrightnessChangeCause cause,
      policy::BacklightController* source) OVERRIDE;

  // Called by |suspender_| before other processes are informed that the
  // system will be suspending soon.
  void PrepareForSuspendAnnouncement();

  // Called by |suspender_| if a suspend request is aborted before
  // PrepareForSuspend() has been called.
  void HandleCanceledSuspendAnnouncement();

  // Called by |suspender_| just before a suspend attempt begins.
  void PrepareForSuspend();

  // Called by |suspender_| after the completion of a suspend/resume cycle
  // (which did not necessarily succeed).
  void HandleResume(bool suspend_was_successful,
                    int num_suspend_retries,
                    int max_suspend_retries);

  // Overridden from policy::InputController::Delegate:
  virtual void HandleLidClosed() OVERRIDE;
  virtual void HandleLidOpened() OVERRIDE;
  virtual void HandlePowerButtonEvent(ButtonState state) OVERRIDE;
  virtual void DeferInactivityTimeoutForVT2() OVERRIDE;
  virtual void ShutDownForPowerButtonWithNoDisplay() OVERRIDE;
  virtual void HandleMissingPowerButtonAcknowledgment() OVERRIDE;

  // Overridden from system::AudioObserver:
  virtual void OnAudioStateChange(bool active) OVERRIDE;

  // Overridden from system::PowerSupplyObserver:
  virtual void OnPowerStatusUpdate() OVERRIDE;

 private:
  class StateControllerDelegate;
  class SuspenderDelegate;

  // Passed to ShutDown() to specify whether the system should power-off or
  // reboot.
  enum ShutdownMode {
    SHUTDOWN_POWER_OFF,
    SHUTDOWN_REBOOT,
  };

  // Convenience method that returns true if |name| exists and is true.
  bool BoolPrefIsTrue(const std::string& name) const;

  // Decreases/increases the keyboard brightness; direction should be +1 for
  // increase and -1 for decrease.
  void AdjustKeyboardBrightness(int direction);

  // Emits a D-Bus signal named |signal_name| announcing that backlight
  // brightness was changed to |brightness_percent| due to |cause|.
  void SendBrightnessChangedSignal(
      double brightness_percent,
      policy::BacklightController::BrightnessChangeCause cause,
      const std::string& signal_name);

  // Registers the dbus message handler with appropriate dbus events.
  void RegisterDBusMessageHandler();

  // Handles changes to D-Bus name ownership.
  void HandleDBusNameOwnerChanged(const std::string& name,
                                  const std::string& old_owner,
                                  const std::string& new_owner);

  // Callbacks for handling dbus messages.
  bool HandleSessionStateChangedSignal(DBusMessage* message);
  bool HandleUpdateEngineStatusUpdateSignal(DBusMessage* message);
  bool HandleCrasNodesChangedSignal(DBusMessage* message);
  bool HandleCrasActiveOutputNodeChangedSignal(DBusMessage* message);
  bool HandleCrasNumberOfActiveStreamsChanged(DBusMessage* message);
  DBusMessage* HandleRequestShutdownMethod(DBusMessage* message);
  DBusMessage* HandleRequestRestartMethod(DBusMessage* message);
  DBusMessage* HandleRequestSuspendMethod(DBusMessage* message);
  DBusMessage* HandleDecreaseScreenBrightnessMethod(DBusMessage* message);
  DBusMessage* HandleIncreaseScreenBrightnessMethod(DBusMessage* message);
  DBusMessage* HandleGetScreenBrightnessMethod(DBusMessage* message);
  DBusMessage* HandleSetScreenBrightnessMethod(DBusMessage* message);
  DBusMessage* HandleDecreaseKeyboardBrightnessMethod(DBusMessage* message);
  DBusMessage* HandleIncreaseKeyboardBrightnessMethod(DBusMessage* message);
  DBusMessage* HandleGetPowerSupplyPropertiesMethod(DBusMessage* message);
  DBusMessage* HandleVideoActivityMethod(DBusMessage* message);
  DBusMessage* HandleUserActivityMethod(DBusMessage* message);
  DBusMessage* HandleSetIsProjectingMethod(DBusMessage* message);
  DBusMessage* HandleSetPolicyMethod(DBusMessage* message);
  DBusMessage* HandlePowerButtonAcknowledgment(DBusMessage* message);

  // Handles information from the session manager about the session state.
  void OnSessionStateChange(const std::string& state_str);

  // Shuts the system down immediately. |reason| describes the why the
  // system is shutting down; see power_constants.cc for valid values.
  void ShutDown(ShutdownMode mode, const std::string& reason);

  // Starts the suspend process. If |use_external_wakeup_count| is true,
  // passes |external_wakeup_count| to
  // policy::Suspender::RequestSuspendWithExternalWakeupCount();
  void Suspend(bool use_external_wakeup_count, uint64 external_wakeup_count);

  // Updates state in |backlight_controller_| and |keyboard_controller_|
  // (if non-NULL).
  void SetBacklightsDimmedForInactivity(bool dimmed);
  void SetBacklightsOffForInactivity(bool off);
  void SetBacklightsSuspended(bool suspended);
  void SetBacklightsDocked(bool docked);

  scoped_ptr<Prefs> prefs_;
  scoped_ptr<StateControllerDelegate> state_controller_delegate_;
  scoped_ptr<DBusSender> dbus_sender_;

  scoped_ptr<system::AmbientLightSensor> light_sensor_;
  scoped_ptr<system::DisplayPowerSetter> display_power_setter_;
  scoped_ptr<system::InternalBacklight> display_backlight_;
  scoped_ptr<policy::BacklightController> display_backlight_controller_;
  scoped_ptr<system::InternalBacklight> keyboard_backlight_;
  scoped_ptr<policy::KeyboardBacklightController>
      keyboard_backlight_controller_;

  scoped_ptr<system::Udev> udev_;
  scoped_ptr<system::Input> input_;
  scoped_ptr<policy::StateController> state_controller_;
  scoped_ptr<policy::InputController> input_controller_;
  scoped_ptr<system::AudioClient> audio_client_;
  scoped_ptr<system::PeripheralBatteryWatcher> peripheral_battery_watcher_;
  scoped_ptr<system::PowerSupply> power_supply_;
  scoped_ptr<policy::DarkResumePolicy> dark_resume_policy_;
  scoped_ptr<SuspenderDelegate> suspender_delegate_;
  scoped_ptr<policy::Suspender> suspender_;

  scoped_ptr<MetricsLibrary> metrics_library_;
  scoped_ptr<MetricsReporter> metrics_reporter_;

  // True once the shutdown process has started. Remains true until the
  // system has powered off.
  bool shutting_down_;

  base::FilePath run_dir_;
  base::TimeTicks session_start_;

  // Last session state that we have been informed of. Initialized as stopped.
  SessionState session_state_;

  // This is the DBus helper object that dispatches DBus messages to handlers
  util::DBusHandler dbus_handler_;

  // Has |state_controller_| been initialized?  Daemon::Init() invokes a
  // bunch of event-handling functions directly, but events shouldn't be
  // passed to |state_controller_| until it's been initialized.
  bool state_controller_initialized_;

  // Set to true if powerd touched a file for crash-reporter before
  // suspending. If true, the file will be unlinked after resuming.
  bool created_suspended_state_file_;

  // True if VT switching should be disabled before the system is suspended.
  bool lock_vt_before_suspend_;

  // True if the "mosys" command should be used to record suspend and resume
  // timestamps in eventlog.
  bool log_suspend_with_mosys_eventlog_;

  DISALLOW_COPY_AND_ASSIGN(Daemon);
};

}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_DAEMON_H_
