// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <time.h>

#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ether.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <fcntl.h>
#include <string>

#include <base/callback_old.h>
#include <base/file_util.h>
#include <base/logging.h>
#include <base/memory/scoped_ptr.h>
#include <base/stl_util-inl.h>
#include <base/stringprintf.h>

#include "shill/control_interface.h"
#include "shill/device.h"
#include "shill/device_info.h"
#include "shill/device_stub.h"
#include "shill/ethernet.h"
#include "shill/manager.h"
#include "shill/rtnl_handler.h"
#include "shill/rtnl_listener.h"
#include "shill/rtnl_message.h"
#include "shill/service.h"
#include "shill/wifi.h"

using std::map;
using std::string;

namespace shill {

// static
const char DeviceInfo::kInterfaceUevent[] = "/sys/class/net/%s/uevent";
// static
const char DeviceInfo::kInterfaceDriver[] = "/sys/class/net/%s/device/driver";
// static
const char *DeviceInfo::kModemDrivers[] = {
    "gobi",
    "QCUSBNet2k",
    "GobiNet",
    NULL
};
// static
const char DeviceInfo::kInterfaceIPv6Privacy[] =
    "/proc/sys/net/ipv6/conf/%s/use_tempaddr";


DeviceInfo::DeviceInfo(ControlInterface *control_interface,
                       EventDispatcher *dispatcher,
                       Manager *manager)
    : control_interface_(control_interface),
      dispatcher_(dispatcher),
      manager_(manager),
      link_callback_(NewCallback(this, &DeviceInfo::LinkMsgHandler)),
      link_listener_(NULL) {
}

DeviceInfo::~DeviceInfo() {}

void DeviceInfo::AddDeviceToBlackList(const string &device_name) {
  black_list_.insert(device_name);
}

void DeviceInfo::Start() {
  link_listener_.reset(
      new RTNLListener(RTNLHandler::kRequestLink, link_callback_.get()));
  RTNLHandler::GetInstance()->RequestDump(RTNLHandler::kRequestLink);
}

void DeviceInfo::Stop() {
  link_listener_.reset(NULL);
}

void DeviceInfo::RegisterDevice(const DeviceRefPtr &device) {
  VLOG(2) << __func__ << "(" << device->link_name() << ", "
          << device->interface_index() << ")";
  CHECK(!GetDevice(device->interface_index()).get());
  infos_[device->interface_index()].device = device;
  if (device->TechnologyIs(Device::kCellular) ||
      device->TechnologyIs(Device::kEthernet) ||
      device->TechnologyIs(Device::kWifi)) {
    manager_->RegisterDevice(device);
  }
}

Device::Technology DeviceInfo::GetDeviceTechnology(const string &iface_name) {
  char contents[1024];
  int length;
  int fd;
  string uevent_file = StringPrintf(kInterfaceUevent, iface_name.c_str());
  string driver_file = StringPrintf(kInterfaceDriver, iface_name.c_str());
  const char *wifi_type;
  const char *driver_name;
  int modem_idx;

  fd = open(uevent_file.c_str(), O_RDONLY);
  if (fd < 0)
    return Device::kUnknown;

  length = read(fd, contents, sizeof(contents) - 1);
  if (length < 0)
    return Device::kUnknown;

  /*
   * If the "uevent" file contains the string "DEVTYPE=wlan\n" at the
   * start of the file or after a newline, we can safely assume this
   * is a wifi device.
   */
  contents[length] = '\0';
  wifi_type = strstr(contents, "DEVTYPE=wlan\n");
  if (wifi_type != NULL && (wifi_type == contents || wifi_type[-1] == '\n'))
    return Device::kWifi;

  length = readlink(driver_file.c_str(), contents, sizeof(contents)-1);
  if (length < 0)
    return Device::kUnknown;

  contents[length] = '\0';
  driver_name = strrchr(contents, '/');
  if (driver_name != NULL) {
    driver_name++;
    // See if driver for this interface is in a list of known modem driver names
    for (modem_idx = 0; kModemDrivers[modem_idx] != NULL; modem_idx++)
      if (strcmp(driver_name, kModemDrivers[modem_idx]) == 0)
        return Device::kCellular;
  }

  return Device::kEthernet;
}

void DeviceInfo::AddLinkMsgHandler(const RTNLMessage &msg) {
  DCHECK(msg.type() == RTNLMessage::kMessageTypeLink &&
         msg.mode() == RTNLMessage::kMessageModeAdd);
  int dev_index = msg.interface_index();
  Device::Technology technology = Device::kUnknown;

  unsigned int flags = msg.link_status().flags;
  unsigned int change = msg.link_status().change;
  VLOG(2) << __func__ << "(index=" << dev_index
          << std::showbase << std::hex
          << ", flags=" << flags << ", change=" << change << ")"
          << std::dec << std::noshowbase;
  infos_[dev_index].flags = flags;

  DeviceRefPtr device = GetDevice(dev_index);
  if (!device.get()) {
    if (msg.HasAttribute(IFLA_ADDRESS)) {
      infos_[dev_index].address = msg.GetAttribute(IFLA_ADDRESS);
      VLOG(2) << "link index " << dev_index << " address "
              << infos_[dev_index].address.HexEncode();
    } else {
      LOG(ERROR) << "Add Link message does not have IFLA_ADDRESS!";
      return;
    }
    if (!msg.HasAttribute(IFLA_IFNAME)) {
      LOG(ERROR) << "Add Link message does not have IFLA_IFNAME!";
      return;
    }
    ByteString b(msg.GetAttribute(IFLA_IFNAME));
    string link_name(reinterpret_cast<const char*>(b.GetConstData()));
    VLOG(2) << "add link index "  << dev_index << " name " << link_name;

    if (!link_name.empty()) {
      if (ContainsKey(black_list_, link_name)) {
        technology = Device::kBlacklisted;
      } else {
        technology = GetDeviceTechnology(link_name);
      }
    }
    string address = infos_[dev_index].address.HexEncode();
    switch (technology) {
      case Device::kCellular:
        // Cellular devices are managed by ModemInfo.
        VLOG(2) << "Cellular link " << link_name << " at index " << dev_index
                << " ignored.";
        return;
      case Device::kEthernet:
        EnableDeviceIPv6Privacy(link_name);
        device = new Ethernet(control_interface_, dispatcher_, manager_,
                              link_name, address, dev_index);
        break;
      case Device::kWifi:
        EnableDeviceIPv6Privacy(link_name);
        device = new WiFi(control_interface_, dispatcher_, manager_,
                          link_name, address, dev_index);
        break;
      default:
        device = new DeviceStub(control_interface_, dispatcher_, manager_,
                                link_name, address, dev_index, technology);
        break;
    }
    RegisterDevice(device);
  }
  device->LinkEvent(flags, change);
}

void DeviceInfo::DelLinkMsgHandler(const RTNLMessage &msg) {
  VLOG(2) << __func__ << "(index=" << msg.interface_index() << ")";

  DCHECK(msg.type() == RTNLMessage::kMessageTypeLink &&
         msg.mode() == RTNLMessage::kMessageModeDelete);
  VLOG(2) << __func__ << "(index=" << msg.interface_index()
          << std::showbase << std::hex
          << ", flags=" << msg.link_status().flags
          << ", change=" << msg.link_status().change << ")";
  RemoveInfo(msg.interface_index());
}

DeviceRefPtr DeviceInfo::GetDevice(int interface_index) const {
  const Info *info = GetInfo(interface_index);
  return info ? info->device : NULL;
}

bool DeviceInfo::GetAddress(int interface_index, ByteString *address) const {
  const Info *info = GetInfo(interface_index);
  if (!info) {
    return false;
  }
  *address = info->address;
  return true;
}

bool DeviceInfo::GetFlags(int interface_index, unsigned int *flags) const {
  const Info *info = GetInfo(interface_index);
  if (!info) {
    return false;
  }
  *flags = info->flags;
  return true;
}

const DeviceInfo::Info *DeviceInfo::GetInfo(int interface_index) const {
  map<int, Info>::const_iterator iter = infos_.find(interface_index);
  if (iter == infos_.end()) {
    return NULL;
  }
  return &iter->second;
}

void DeviceInfo::RemoveInfo(int interface_index) {
  map<int, Info>::iterator iter = infos_.find(interface_index);
  if (iter != infos_.end()) {
    VLOG(2) << "Removing info for device index: " << interface_index;
    if (iter->second.device.get()) {
      manager_->DeregisterDevice(iter->second.device);
    }
    infos_.erase(iter);
  } else {
    VLOG(2) << __func__ << "unknown device index: " << interface_index;
  }
}

void DeviceInfo::LinkMsgHandler(const RTNLMessage &msg) {
  DCHECK(msg.type() == RTNLMessage::kMessageTypeLink);
  if (msg.mode() == RTNLMessage::kMessageModeAdd) {
    AddLinkMsgHandler(msg);
  } else if (msg.mode() == RTNLMessage::kMessageModeDelete) {
    DelLinkMsgHandler(msg);
  } else {
    NOTREACHED();
  }
}

void DeviceInfo::EnableDeviceIPv6Privacy(const string &iface_name) {
  FilePath priv_file(StringPrintf(kInterfaceIPv6Privacy, iface_name.c_str()));
  LOG_IF(ERROR, file_util::WriteFile(priv_file, "2", 1) != 1)
      << "Write failed for use_tempaddr: " << priv_file.value();
}

}  // namespace shill
