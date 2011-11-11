// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power_manager/power_button_handler.h"

#include <gdk/gdkx.h>

#include "base/logging.h"
#include "base/time.h"
#include "power_manager/backlight_controller.h"
#include "power_manager/powerd.h"
#include "power_manager/screen_locker.h"
#include "power_manager/util.h"

namespace power_manager {

// Amount of time that the power button needs to be held before we lock the
// screen.
static const guint kLockTimeoutMs = 400;

// Amount of time that the power button needs to be held before we shut down.
static const guint kShutdownTimeoutMs = 400;

// When the button has been held continuously from the unlocked state, amount of
// time that we wait after locking the screen before starting the pre-shutdown
// animation.
static const guint kLockToShutdownTimeoutMs = 600;

// Amount of time that we give the window manager to display the shutdown
// animation before we dim the screen and start actually shutting down the
// system.
static const guint kShutdownAnimationMs = 150;

// Avoid sending a second lock request for this many milliseconds after one has
// already been sent.
static const int64 kRecentLockRequestMs = 2000;

// If the ID pointed to by |timeout_id| is non-zero, remove the timeout and set
// the memory to 0.
static void RemoveTimeoutIfSet(guint* timeout_id) {
  if (timeout_id && *timeout_id > 0) {
    g_source_remove(*timeout_id);
    *timeout_id = 0;
  }
}

PowerButtonHandler::PowerButtonHandler(Daemon* daemon)
    : daemon_(daemon),
      lock_timeout_id_(0),
      lock_to_shutdown_timeout_id_(0),
      shutdown_timeout_id_(0),
      real_shutdown_timeout_id_(0),
      lock_button_down_(false),
      power_button_down_(false),
      shutting_down_(false),
      should_add_shutdown_timeout_after_lock_(false) {
}

PowerButtonHandler::~PowerButtonHandler() {
  RemoveTimeoutIfSet(&lock_timeout_id_);
  RemoveTimeoutIfSet(&lock_to_shutdown_timeout_id_);
  RemoveTimeoutIfSet(&lock_fail_timeout_id_);
  RemoveTimeoutIfSet(&shutdown_timeout_id_);
  RemoveTimeoutIfSet(&real_shutdown_timeout_id_);
}

void PowerButtonHandler::HandlePowerButtonDown() {
  if (shutting_down_)
    return;

  const bool should_lock = util::LoggedIn() &&
                           !daemon_->current_user().empty() &&
                           !daemon_->locker()->is_locked();

  power_button_down_ = true;
#ifdef NEW_POWER_BUTTON
  // There's a small window of time between when we ask the session manager to
  // lock the screen and when we receive confirmation that the screen has been
  // locked -- see http://crosbug.com/21137.  If we see the power button get
  // pressed in that window but we haven't yet heard that the screen is locked,
  // wait for confirmation before starting the pre-shutdown process.
  const base::TimeTicks request_time =
      daemon_->locker()->last_lock_request_time();
  const bool recently_sent_lock_request =
      !request_time.is_null() &&
      (base::TimeTicks::Now() - request_time <=
       base::TimeDelta::FromMilliseconds(kRecentLockRequestMs));
  if (recently_sent_lock_request && !daemon_->locker()->is_locked()) {
    should_add_shutdown_timeout_after_lock_ = true;
    return;
  }

  // Power button release supported. This allows us to schedule events based
  // on how long the button was held down.
  if (should_lock) {
    NotifyWindowManagerAboutPowerButtonState(
        chromeos::WM_IPC_POWER_BUTTON_PRE_LOCK);
    RemoveTimeoutIfSet(&lock_timeout_id_);
    lock_timeout_id_ =
        g_timeout_add(kLockTimeoutMs,
                      PowerButtonHandler::OnLockTimeoutThunk,
                      this);
  } else {
    AddShutdownTimeout();
  }
#else
  // Legacy behavior for x86 systems because the ACPI button driver sends both
  // down and release events at the time the acpi notify occurs for power
  // button.
  if (should_lock)
    daemon_->locker()->LockScreen();
  else
    OnShutdownTimeout();
#endif
}

void PowerButtonHandler::HandlePowerButtonUp() {
  if (shutting_down_)
    return;

  power_button_down_ = false;
  should_add_shutdown_timeout_after_lock_ = false;

#ifdef NEW_POWER_BUTTON
  if (lock_timeout_id_) {
    RemoveTimeoutIfSet(&lock_timeout_id_);
    NotifyWindowManagerAboutPowerButtonState(
        chromeos::WM_IPC_POWER_BUTTON_ABORTED_LOCK);
  }
  if (shutdown_timeout_id_) {
    RemoveTimeoutIfSet(&shutdown_timeout_id_);
    NotifyWindowManagerAboutPowerButtonState(
        chromeos::WM_IPC_POWER_BUTTON_ABORTED_SHUTDOWN);
  }
  RemoveTimeoutIfSet(&lock_to_shutdown_timeout_id_);
  RemoveTimeoutIfSet(&lock_fail_timeout_id_);
#endif
}

void PowerButtonHandler::HandleLockButtonDown() {
  if (shutting_down_)
    return;

  const bool should_lock = util::LoggedIn() &&
                           !daemon_->current_user().empty() &&
                           !daemon_->locker()->is_locked();
  lock_button_down_ = true;
  if (should_lock) {
    NotifyWindowManagerAboutPowerButtonState(
        chromeos::WM_IPC_POWER_BUTTON_PRE_LOCK);
    RemoveTimeoutIfSet(&lock_timeout_id_);
    lock_timeout_id_ =
        g_timeout_add(kLockTimeoutMs,
                      PowerButtonHandler::OnLockTimeoutThunk,
                      this);
  }
}

void PowerButtonHandler::HandleLockButtonUp() {
  if (shutting_down_)
    return;

  lock_button_down_ = false;
  if (lock_timeout_id_) {
    RemoveTimeoutIfSet(&lock_timeout_id_);
    NotifyWindowManagerAboutPowerButtonState(
        chromeos::WM_IPC_POWER_BUTTON_ABORTED_LOCK);
  }
}

void PowerButtonHandler::HandleScreenLocked() {
  if (should_add_shutdown_timeout_after_lock_) {
    should_add_shutdown_timeout_after_lock_ = false;
    RemoveTimeoutIfSet(&lock_fail_timeout_id_);
    AddShutdownTimeout();
  }
}

gboolean PowerButtonHandler::OnLockTimeout() {
  lock_timeout_id_ = 0;
  daemon_->locker()->LockScreen();
  daemon_->BrightenScreenIfOff();
  if (lock_button_down_ && !power_button_down_)
    return FALSE;

  // Only transition to shutdown on a power button push, not lock button.
  RemoveTimeoutIfSet(&lock_to_shutdown_timeout_id_);
  lock_to_shutdown_timeout_id_ =
      g_timeout_add(kLockToShutdownTimeoutMs,
                    PowerButtonHandler::OnLockToShutdownTimeoutThunk,
                    this);
  return FALSE;
}

gboolean PowerButtonHandler::OnLockToShutdownTimeout() {
  lock_to_shutdown_timeout_id_ = 0;

  // If the screen is already locked, then start the pre-shutdown process.
  // Otherwise, wait until we get notification that it's locked.
  if (daemon_->locker()->is_locked()) {
    AddShutdownTimeout();
  } else {
    should_add_shutdown_timeout_after_lock_ = true;
    // check again in kShutdownTimeoutMs if the screen is still not locked.
    lock_fail_timeout_id_ =
        g_timeout_add(kShutdownTimeoutMs,
                      PowerButtonHandler::OnLockFailTimeoutThunk,
                      this);
  }

  return FALSE;
}

gboolean PowerButtonHandler::OnLockFailTimeout() {
  lock_fail_timeout_id_ = 0;
  if (should_add_shutdown_timeout_after_lock_ &&
      !daemon_->locker()->is_locked()) {
    LOG(WARNING) << "Lock screen never locked. Continuing with shutdown.";
    should_add_shutdown_timeout_after_lock_ = false;
    AddShutdownTimeout();
  }
  return FALSE;
}

gboolean PowerButtonHandler::OnShutdownTimeout() {
  shutdown_timeout_id_ = 0;
  shutting_down_ = true;
  NotifyWindowManagerAboutShutdown();
  DCHECK(real_shutdown_timeout_id_ == 0) << "Shutdown already in-progress";
  real_shutdown_timeout_id_ =
      g_timeout_add(kShutdownAnimationMs,
                    PowerButtonHandler::OnRealShutdownTimeoutThunk,
                    this);
  return FALSE;
}

gboolean PowerButtonHandler::OnRealShutdownTimeout() {
  real_shutdown_timeout_id_ = 0;
  // TODO: Ideally, we'd use the backlight controller to turn off the display
  // after the window manager has had enough time to display the shutdown
  // animation.  Using DPMS for this is pretty ugly, though -- the backlight
  // turns back on when X exits or if the user moves the mouse or hits a key.
  // We just dim it instead for now.
  daemon_->backlight_controller()->SetPowerState(BACKLIGHT_DIM);
  daemon_->OnRequestShutdown(false);  // notify_window_manager=false
  return FALSE;
}

void PowerButtonHandler::AddShutdownTimeout() {
  NotifyWindowManagerAboutPowerButtonState(
      chromeos::WM_IPC_POWER_BUTTON_PRE_SHUTDOWN);
  RemoveTimeoutIfSet(&shutdown_timeout_id_);
  shutdown_timeout_id_ =
      g_timeout_add(kShutdownTimeoutMs,
                    PowerButtonHandler::OnShutdownTimeoutThunk,
                    this);
}

bool PowerButtonHandler::NotifyWindowManagerAboutPowerButtonState(
    chromeos::WmIpcPowerButtonState button_state) {
  return util::SendMessageToWindowManager(
             chromeos::WM_IPC_MESSAGE_WM_NOTIFY_POWER_BUTTON_STATE,
             button_state);
}

bool PowerButtonHandler::NotifyWindowManagerAboutShutdown() {
  return util::SendMessageToWindowManager(
             chromeos::WM_IPC_MESSAGE_WM_NOTIFY_SHUTTING_DOWN, 0);
}

}  // namespace power_manager
