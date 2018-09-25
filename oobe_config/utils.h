// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OOBE_CONFIG_UTILS_H_
#define OOBE_CONFIG_UTILS_H_

#include <string>
#include <vector>

#include <base/files/file_path.h>

namespace oobe_config {

int RunCommand(const std::vector<std::string>& command);

// Using |priv_key|, signs |src|, and writes the digest into |dst|.
bool SignFile(const base::FilePath& priv_key,
              const base::FilePath& src,
              const base::FilePath& dst);

}  // namespace oobe_config

#endif  // OOBE_CONFIG_UTILS_H_
