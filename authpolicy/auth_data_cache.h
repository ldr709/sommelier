// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AUTHPOLICY_AUTH_DATA_CACHE_H_
#define AUTHPOLICY_AUTH_DATA_CACHE_H_

#include <memory>
#include <string>

#include <base/macros.h>
#include <base/optional.h>
#include "base/time/time.h"

#include "bindings/authpolicy_containers.pb.h"

namespace base {
class FilePath;
class Clock;
}  // namespace base

namespace authpolicy {

// Cache for authentication-related data. Used to speed up user authentication.
// Basically a wrapper around protos::CachedAuthData to load from and save to a
// file and access fields conveniently. Cache keys are the device or user realm.
class AuthDataCache {
 public:
  // |flags| is a non-owned pointer to DebugFlags flags (the class listens to
  // flags->log_caches() to toggle logging).
  explicit AuthDataCache(const protos::DebugFlags* flags);
  ~AuthDataCache();

  // Loads |data_| from the file at |path|. Returns true if the file was
  // successfully loaded and parsed. Clears data and returns false on error.
  // Clears data and returns true if not enabled.
  bool Load(const base::FilePath& path);

  // Saves |data_| to the file at |path|. Returns true if the file was
  // successfully written. Returns true if not enabled.
  bool Save(const base::FilePath& path);

  // Clears the cache.
  void Clear();

  // Turns the cache on or off. While set to false, all Get*() operations return
  // nullopt and the Set() operation does nothing.
  void SetEnabled(bool enabled) { enabled_ = enabled; }
  bool IsEnabled() const { return enabled_; }

  // Getters return base::nullopt if the values are not in the cache.
  base::Optional<std::string> GetWorkgroup(const std::string& realm) const;
  base::Optional<std::string> GetKdcIp(const std::string& realm) const;
  base::Optional<std::string> GetDcName(const std::string& realm) const;
  base::Optional<bool> GetIsAffiliated(const std::string& realm) const;

  // Setters create new cache entries if they don't exist yet.
  void SetWorkgroup(const std::string& realm, const std::string& workgroup);
  void SetKdcIp(const std::string& realm, const std::string& kdc_ip);
  void SetDcName(const std::string& realm, const std::string& dc_name);
  void SetIsAffiliated(const std::string& realm, bool is_affiliated);

  // Removes all cache entriers older than |max_age|.
  void RemoveEntriesOlderThan(base::TimeDelta max_age);

  // Overrides the clock used for purging old cache entries.
  void SetClockForTesting(std::unique_ptr<base::Clock> clock);

  base::Clock* clock() { return clock_.get(); }

 private:
  // Gets |realm_data| from |data_| for the given |realm| if it exists and if
  // |enabled_| is true. Otherwise, returns nullptr. Used in the getters.
  const protos::CachedRealmData* GetRealmDataForRead(
      const std::string& realm) const;

  // Gets |realm_data| from |data_| for the given |realm|. Creates a new entry
  // and sets the |cache_time| if it doesn't exist yet. Returns nullptr if
  // |enabled_| is false. Used in the setters.
  protos::CachedRealmData* GetRealmDataForWrite(const std::string& realm);

  // Maps realms to protos::CachedRealmData.
  protos::CachedAuthData data_;

  // Pointer to debug flags, not owned.
  const protos::DebugFlags* flags_;

  // Clock to get cache time, can be overridden for tests.
  std::unique_ptr<base::Clock> clock_;

  // Whether the cache is enabled or not. While disabled, getters return nullopt
  // and setters do nothing.
  bool enabled_ = true;

  DISALLOW_COPY_AND_ASSIGN(AuthDataCache);
};

}  // namespace authpolicy

#endif  // AUTHPOLICY_AUTH_DATA_CACHE_H_
