// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VM_TOOLS_CONCIERGE_VIRTUAL_MACHINE_H_
#define VM_TOOLS_CONCIERGE_VIRTUAL_MACHINE_H_

#include <stdint.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <base/files/scoped_temp_dir.h>
#include <base/macros.h>
#include <brillo/process.h>

#include "vm_tools/concierge/mac_address_generator.h"
#include "vm_tools/concierge/subnet_pool.h"
#include "vm_tools/concierge/vsock_cid_pool.h"

#include "guest.grpc.pb.h"  // NOLINT(build/include)

namespace vm_tools {
namespace concierge {

// Represents a single instance of a running virtual machine.
class VirtualMachine {
 public:
  // Describes how maitre'd should handle exits for processes launched via the
  // StartProcess() function.
  enum class ProcessExitBehavior {
    // Don't respawn the process when it exits.
    ONE_SHOT,
    // Respawn the process when it exits.
    RESPAWN_ON_EXIT,
  };

  // Describes the status of a process launched inside the VM.
  enum class ProcessStatus {
    // ¯\_(ツ)_/¯
    UNKNOWN,

    // The process exited normally.
    EXITED,

    // The process was killed by a signal.
    SIGNALED,

    // Successfully launched but may or may not have exited yet.
    LAUNCHED,

    // One or more setup steps failed and the process did not launch.
    FAILED,
  };

  // Describes the result of attempting to launch a process inside the VM.
  struct LaunchProcessResult {
    // The current status of the process.
    ProcessStatus status;

    // If |status| is either EXITED or SIGNALED, then this holds the exit code
    // or signal number, respectively.  Otherwise this value is undefined.
    int32_t code;
  };

  // Describes a disk image to be mounted inside the VM.
  struct Disk {
    // Path to the disk image on the host.
    base::FilePath path;

    // Whether the disk should be writable by the VM.
    bool writable;
  };

  // Starts a new virtual machine.  Returns nullptr if the virtual machine
  // failed to start for any reason.
  static std::unique_ptr<VirtualMachine> Create(
      base::FilePath kernel,
      base::FilePath rootfs,
      std::vector<Disk> disks,
      MacAddress mac_addr,
      std::unique_ptr<SubnetPool::Subnet> subnet,
      uint32_t vsock_cid,
      base::FilePath runtime_dir);
  ~VirtualMachine();

  // Shuts down the VM.  First attempts a clean shutdown of the VM by sending
  // a Shutdown RPC to maitre'd.  If that fails, attempts to shut down the VM
  // using the control socket for the hypervisor.  If that fails, then sends a
  // SIGTERM to the hypervisor.  Finally, if nothing works forcibly stops the VM
  // by sending it a SIGKILL.  Returns true if the VM was shut down and false
  // otherwise.
  bool Shutdown();

  // Launches a process inside the VM and returns without waiting for it to
  // exit. |args[0]| must be either the name of a program in the default PATH
  // inside the VM or the path to the program to be executed.
  LaunchProcessResult StartProcess(std::vector<std::string> args,
                                   std::map<std::string, std::string> env,
                                   ProcessExitBehavior exit_behavior);

  // Launches a process inside the VM and synchronously waits for it to exit.
  // |args[0]| must be either the name of a program in the default PATH
  // inside the VM or the path to the program to be executed.
  LaunchProcessResult RunProcess(std::vector<std::string> args,
                                 std::map<std::string, std::string> env);

  // Configures the network interfaces inside the VM.  Returns true if
  // successful, false otherwise.
  bool ConfigureNetwork();

  // Mounts a file system inside the VM.  Both |source| (if it is a file path)
  // and |target| must be valid paths inside the VM.  Returns true on success.
  bool Mount(std::string source,
             std::string target,
             std::string fstype,
             uint64_t mountflags,
             std::string options);

  // The pid of the child process.
  pid_t pid() { return process_.pid(); }

  static std::unique_ptr<VirtualMachine> CreateForTesting(
      MacAddress mac_addr,
      std::unique_ptr<SubnetPool::Subnet> subnet,
      uint32_t vsock_cid,
      base::FilePath runtime_dir,
      std::unique_ptr<vm_tools::Maitred::Stub> stub);

 private:
  VirtualMachine(MacAddress mac_addr,
                 std::unique_ptr<SubnetPool::Subnet> subnet,
                 uint32_t vsock_cid,
                 base::FilePath runtime_dir);

  // Starts the VM with the given kernel and root file system.
  bool Start(base::FilePath kernel,
             base::FilePath rootfs,
             std::vector<Disk> disks);

  // Launches a process inside the VM.  Shared implementation of both
  // StartProcess and RunProcess.
  LaunchProcessResult LaunchProcess(std::vector<std::string> args,
                                    std::map<std::string, std::string> env,
                                    bool respawn,
                                    bool wait_for_exit);

  void set_stub_for_testing(std::unique_ptr<vm_tools::Maitred::Stub> stub);

  // EUI-48 mac address for the VM's network interface.
  MacAddress mac_addr_;

  // The /30 subnet assigned to the VM.
  std::unique_ptr<SubnetPool::Subnet> subnet_;

  // Virtual socket context id to be used when communicating with this VM.
  uint32_t vsock_cid_;

  // Runtime directory for this VM.
  base::ScopedTempDir runtime_dir_;

  // Handle to the VM process.
  brillo::ProcessImpl process_;

  // Stub for making RPC requests to the maitre'd process inside the VM.
  std::unique_ptr<vm_tools::Maitred::Stub> stub_;

  DISALLOW_COPY_AND_ASSIGN(VirtualMachine);
};

}  // namespace concierge
}  // namespace vm_tools

#endif  // VM_TOOLS_CONCIERGE_VIRTUAL_MACHINE_H_
