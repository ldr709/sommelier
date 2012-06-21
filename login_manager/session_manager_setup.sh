#!/bin/sh

# Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Set up to start the X server ASAP, then let the startup run in the
# background while we set up other stuff.
XAUTH_FILE="/var/run/chromelogin.auth"
MCOOKIE=$(mcookie)
xauth -q -f ${XAUTH_FILE} add :0 . ${MCOOKIE}

# The X server sends SIGUSR1 to its parent once it's ready to accept
# connections.  The subshell here starts X, waits for the signal, then
# terminates once X is ready.
( trap 'exit 0' USR1 ; xstart.sh ${XAUTH_FILE} & wait ) &

USE_FLAGS=$(cat /etc/session_manager_use_flags.txt)

# Returns success if the USE flag passed as its sole parameter was defined.
# New flags must be first be added to the ebuild file.
use_flag_is_set() {
  local flag i
  flag="$1"
  for i in $USE_FLAGS; do
    if [ $i = "${flag}" ]; then
      return 0
    fi
  done
  return 1
}

# Returns success if we were built for the board passed as the sole parameter.
# Not all boards are handled; see the ebuild file.
is_board() {
  use_flag_is_set "board_use_$1"
}

export USER=chronos
export DATA_DIR=/home/${USER}
export LOGIN_PROFILE_DIR=${DATA_DIR}/Default
export LOGNAME=${USER}
export SHELL=/bin/sh
export HOME=${DATA_DIR}/user
export DISPLAY=:0.0

# If used with Address Sanitizer, set the following flags to alter memory
# allocations by glibc. Hopefully later, when ASAN matures, we will not need
# any changes for it to run.
ASAN_FLAGS=
if use_flag_is_set asan; then
  # Make glib use system malloc.
  export G_SLICE=always-malloc

  # Make nss skip dlclosing dynamically loaded modules,
  # which would result in "obj:*" in backtraces.
  export NSS_DISABLE_ARENA_FREE_LIST=1

  # Make nss use system malloc.
  export NSS_DISABLE_UNLOAD=1

  # Disable sandboxing as it causes crashes in ASAN. crosbug.com/127536.
  ASAN_FLAGS="--no-sandbox"
fi

# Change the directory for ibus-daemon socket file from ~/.config/ibus/bus/ to
# /tmp/.ibus-socket-<unique random string>/ to fix crosbug.com/16501 and 17270.
# Every time when you change IBUS_ADDRESS_FILE, you should also update the
# variable in desktopui_ImeTest.py in autotest.git not to break IME autotests.
export IBUS_ADDRESS_FILE=\
"$(sudo -u ${USER} /bin/mktemp -d /tmp/.ibus-socket-XXXXXXXXXX)\
/ibus-socket-file"
# By default, libdbus treats all warnings as fatal errors. That's too strict.
export DBUS_FATAL_WARNINGS=0

# Tell Chrome where to write logging messages.
# $CHROME_LOG_DIR and $CHROME_LOG_PREFIX are defined in ui.conf,
# and the directory is created there as well.
export CHROME_LOG_FILE="${CHROME_LOG_DIR}/${CHROME_LOG_PREFIX}"

# Log directory for this session.  Note that ${HOME} might not be
# mounted until later (when the cryptohome is mounted), so we don't
# mkdir CHROMEOS_SESSION_LOG_DIR immediately.
export CHROMEOS_SESSION_LOG_DIR="${HOME}/log"

# Forces Chrome mini dumps that are sent to the crash server to also be written
# locally.  Chrome by default will create these mini dump files in
# ~/.config/google-chrome/Crash Reports/
if [ -f /mnt/stateful_partition/etc/enable_chromium_minidumps ] ; then
  export CHROME_HEADLESS=1
  # If possible we would like to have the crash reports located somewhere else
  if [ ! -f ~/.config/google-chrome/Crash\ Reports ] ; then
    mkdir -p /mnt/stateful_partition/var/minidumps/
    chown chronos /mnt/stateful_partition/var/minidumps/
    ln -s /mnt/stateful_partition/var/minidumps/ \
      ~/.config/google-chrome/Crash\ Reports
  fi
fi

export XAUTHORITY=${DATA_DIR}/.Xauthority

mkdir -p ${DATA_DIR} && chown ${USER}:${USER} ${DATA_DIR}
mkdir -p ${HOME} && chown ${USER}:${USER} ${HOME}
xauth -q -f ${XAUTHORITY} add :0 . ${MCOOKIE} &&
  chown ${USER}:${USER} ${XAUTHORITY}

# Old builds will have a ${LOGIN_PROFILE_DIR} that's owned by root; newer ones
# won't have this directory at all.
mkdir -p ${LOGIN_PROFILE_DIR}
chown ${USER}:${USER} ${LOGIN_PROFILE_DIR}

CHROME="/opt/google/chrome/chrome"
# Note: If this script is renamed, ChildJob::kWindowManagerSuffix needs to be
# updated to contain the new name.  See http://crosbug.com/7901 for more info.
WM_SCRIPT="/sbin/window-manager-session.sh"
CONSENT_FILE="$DATA_DIR/Consent To Send Stats"

# xdg-open is used to open downloaded files.
# It runs sensible-browser, which uses $BROWSER.
export BROWSER=${CHROME}

USER_ID=$(id -u ${USER})

SKIP_OOBE=
# For test automation.  If file exists, do not remember last username and skip
# out-of-box-experience windows except the login window
if [ -f /root/.forget_usernames ] ; then
  rm -f "${DATA_DIR}/Local State"
  SKIP_OOBE="--login-screen=login"
fi

# To always force OOBE. This works ok with test images as they
# are always started with OOBE.
if [ -f /root/.test_repeat_oobe ] ; then
  rm -f "${DATA_DIR}/.oobe_completed"
  rm -f "${DATA_DIR}/Local State"
  SKIP_OOBE=
fi

NEW_OOBE_FLAGS=
# For testing new OOBE/sign in flow.
if [ -f /root/.enable_new_oobe ] ; then
  NEW_OOBE_FLAGS="--enable-new-oobe --allow-webui-compositing \
                  --enable-html5-camera"
fi

# For recovery image, do NOT display OOBE or login window
if [ -f /mnt/stateful_partition/.recovery ]; then
  # Verify recovery UI HTML file exists
  if [ -f /usr/share/misc/recovery_ui.html ]; then
    SKIP_OOBE="--login-screen=html file:///usr/share/misc/recovery_ui.html"
  else
    # Fall back to displaying a blank screen
    # the magic string "test:nowindow" comes from
    # src/chrome/browser/chromeos/login/wizard_controller.cc
    SKIP_OOBE="--login-screen=test:nowindow"
  fi
fi

# Enables gathering of chrome dumps.  In stateful partition so testers
# can enable getting core dumps after build time.
if [ -f /mnt/stateful_partition/etc/enable_chromium_coredumps ] ; then
  mkdir -p /mnt/stateful_partition/var/coredumps/
  # Chrome runs and chronos so we need to change the permissions of this folder
  # so it can write there when it crashes
  chown chronos /mnt/stateful_partition/var/coredumps/
  ulimit -c unlimited
  echo "/mnt/stateful_partition/var/coredumps/core.%e.%p" > \
    /proc/sys/kernel/core_pattern
fi

# Remove consent file if it had at one point been created by this script.
if [ -f "$CONSENT_FILE" ]; then
  CONSENT_USER_GROUP=$(stat -c %U:%G "$CONSENT_FILE")
  # normally, the consent file would be owned by "chronos:chronos".
  if [ "$CONSENT_USER_GROUP" = "root:root" ]; then
    TAG="$(basename $0)[$$]"
    logger -t "${TAG}" "Removing consent file owned by root"
    rm -f "$CONSENT_FILE"
  fi
fi

# We need to delete these files as Chrome may have left them around from
# its prior run (if it crashed).
rm -f ${DATA_DIR}/SingletonLock
rm -f ${DATA_DIR}/SingletonSocket

# Set an environment variable to prevent Flash asserts from crashing the plugin
# process.
export DONT_CRASH_ON_ASSERT=1

# Look for pepper plugins and register them
PEPPER_PATH=/opt/google/chrome/pepper
REGISTER_PLUGINS=
COMMA=
FLASH_FLAGS=
for file in $(find $PEPPER_PATH -name '*.info'); do
  FILE_NAME=
  PLUGIN_NAME=
  DESCRIPTION=
  VERSION=
  MIME_TYPES=
  . $file
  [ -z "$FILE_NAME" ] && continue
  PLUGIN_STRING="${FILE_NAME}"
  if [ -n "$PLUGIN_NAME" ]; then
    PLUGIN_STRING="${PLUGIN_STRING}#${PLUGIN_NAME}"
    if [ -n "$DESCRIPTION" ]; then
      PLUGIN_STRING="${PLUGIN_STRING}#${DESCRIPTION}"
      [ -n "$VERSION" ] && PLUGIN_STRING="${PLUGIN_STRING}#${VERSION}"
    fi
  fi
  if [ "$PLUGIN_NAME" = "Shockwave Flash" ]; then
    # Flash is treated specially.
    FLASH_FLAGS="--ppapi-flash-path=${FILE_NAME}"
    FLASH_FLAGS="${FLASH_FLAGS} --ppapi-flash-version=${VERSION}"
  else
    PLUGIN_STRING="${PLUGIN_STRING};${MIME_TYPES}"
    REGISTER_PLUGINS="${REGISTER_PLUGINS}${COMMA}${PLUGIN_STRING}"
    COMMA=","
  fi
done
if [ -n "$REGISTER_PLUGINS" ]; then
  REGISTER_PLUGINS="--register-pepper-plugins=$REGISTER_PLUGINS"
fi

# Desktop configurations may use keyboards without volume controls.
SHOW_VOLUME_STATUS_FLAG=
if use_flag_is_set is_desktop; then
  SHOW_VOLUME_STATUS_FLAG="--show-volume-status"
fi

AURA_FLAGS=
if use_flag_is_set aura; then
  WM_SCRIPT=""

  if ! use_flag_is_set new_power_button; then
    AURA_FLAGS="$AURA_FLAGS --aura-legacy-power-button"
  fi
  if use_flag_is_set disable_login_animations; then
    AURA_FLAGS="$AURA_FLAGS --disable-login-animations"
  fi
fi

# Setup GPU & acceleration flags which differ between x86/ARM SoC
if [ "$(uname -m)" != "armv7l" ] ; then
  ACCELERATED_FLAGS="--enable-accelerated-layers"
else
  ACCELERATED_FLAGS="--use-gl=egl \
                     --ppapi-flash-args=enable-hardware-decoder=1"
fi

# TODO(katierh): Remove this when Mali drivers fixed, crosbug.com/p/8085
if is_board daisy || is_board ironhide; then
  export MALI_FORCE_WINDOW_CONFIG_ID=10
fi

HIGHDPI_FLAGS=
if use_flag_is_set highdpi; then
  HIGHDPI_FLAGS="--allow-webui-compositing --load-2x-resources"
  HIGHDPI_FLAGS="$HIGHDPI_FLAGS --enable-text-subpixel-positioning"
fi

# If screensaver use isn't disabled, set screensaver.
SCREENSAVERS_PATH=/usr/share/chromeos-assets/screensavers
SCREENSAVER_FLAG=
if [ -d "${SCREENSAVERS_PATH}" ]; then
  SCREENSAVER_FLAG="--screen-saver-url=\
file://${SCREENSAVERS_PATH}/default/index.htm"
fi

TOUCHUI_FLAGS=
if use_flag_is_set touchui; then
  TOUCHUI_FLAGS="--enable-touch-calibration --enable-touch-events"
fi

# Device Manager Server used to fetch the enterprise policy, if applicable.
DMSERVER="https://m.google.com/devicemanagement/data/api"

# Set up cgroups for chrome. We create two task groups, one for at most one
# foreground renderer and one for all the background renderers and set the
# background group to the lowest possible priority.
mkdir -p /tmp/cgroup/cpu
if ! grep -q '^cgroup /tmp/cgroup/cpu cgroup' /proc/mounts; then
  mount -t cgroup cgroup /tmp/cgroup/cpu -o cpu
fi
mkdir -p /tmp/cgroup/cpu/chrome_renderers
mkdir -p /tmp/cgroup/cpu/chrome_renderers/foreground
mkdir -p /tmp/cgroup/cpu/chrome_renderers/background
echo "2" > /tmp/cgroup/cpu/chrome_renderers/background/cpu.shares
chown -R chronos /tmp/cgroup/cpu/chrome_renderers

# For i18n keyboard support (crbug.com/116999)
export LC_CTYPE=en_US.utf8

# The subshell that started the X server will terminate once X is
# ready.  Wait here for that event before continuing.
#
# RED ALERT!  The code from the 'wait' to the end of the script is
# part of the boot time critical path.  Every millisecond spent after
# the wait is a millisecond longer till the login screen.
#
# KEEP THIS CODE PATH CLEAN!  The code must be obviously fast by
# inspection; nothing should go after the wait that isn't required
# for correctness.

wait

initctl emit x-started
bootstat x-started

# When X starts, it copies the contents of the framebuffer to the root
# window.  We clear the framebuffer here to make sure that it doesn't flash
# back onscreen when X exits later.
ply-image --clear 0x000000 &

# This is a bad place to add your code.  See "RED ALERT", above.
# Regrettably, this comment is not redundant.  :-(

#
# Reset PATH to exclude directories unneeded by session_manager.
# Save that until here, because many of the commands above depend
# on the default PATH handed to us by init.
#
export PATH=/bin:/usr/bin:/usr/bin/X11

exec /sbin/session_manager --uid=${USER_ID} -- \
    $CHROME --apps-gallery-title="Web Store" \
            --apps-gallery-url="https://chrome.google.com/webstore/" \
            --compress-sys-feedback \
            --device-management-url="$DMSERVER" \
            --disable-seccomp-sandbox \
            --enable-accelerated-plugins \
            --enable-device-policy \
            --enable-gview \
            --enable-logging \
            --enable-mobile-dialog \
            --enable-onc-policy \
            --enable-partial-swap \
            --enable-smooth-scrolling \
            --enable-sync-tabs \
            --enable-sync-tabs-for-other-clients \
            --enable-threaded-compositing \
            --enable-ui-release-front-surface \
            --enterprise-enrollment-initial-modulus=5 \
            --enterprise-enrollment-modulus-limit=12 \
            --force-compositing-mode \
            --load-opencryptoki \
            --log-level=1 \
            --login-manager \
            --login-profile=user \
            --no-first-run \
            --ppapi-flash-args=enable_stagevideo_auto=0 \
            --reload-killed-tabs \
            --scroll-pixels=3 \
            --ui-enable-partial-swap \
            --ui-enable-per-tile-painting \
            --ui-prioritize-in-gpu-process \
            --use-cras \
            --user-data-dir="$DATA_DIR" \
            --no-protector \
            --disable-gpu-sandbox \
            --allow-webui-compositing \
            "$REGISTER_PLUGINS" \
            ${ACCELERATED_FLAGS} \
            ${AURA_FLAGS} \
            ${FLASH_FLAGS} \
            ${HIGHDPI_FLAGS} \
            ${SCREENSAVER_FLAG} \
            ${SHOW_VOLUME_STATUS_FLAG} \
            ${SKIP_OOBE} \
            ${TOUCHUI_FLAGS} \
            ${ASAN_FLAGS} \
            ${NEW_OOBE_FLAGS} \
    ${WM_SCRIPT:+-- "${WM_SCRIPT}"}
