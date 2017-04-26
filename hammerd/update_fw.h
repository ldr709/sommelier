// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file contains structures used to facilitate EC firmware updates
// over USB. Note that many contents in this file are copied from EC overlay,
// and it might be trickier to include them directly.
//
// The firmware update protocol consists of two phases: connection
// establishment and actual image transfer.
//
// Image transfer is done in 1K blocks. The host supplying the image
// encapsulates blocks in PDUs by prepending a header including the flash
// offset where the block is destined and its digest.
//
// The EC device responds to each PDU with a confirmation which is 1 byte
// response. Zero value means success, non zero value is the error code
// reported by EC.
//
// To establish the connection, the host sends a different PDU, which
// contains no data and is destined to offset 0. Receiving such a PDU
// signals the EC that the host intends to transfer a new image.
//
// The connection establishment response is described by the
// FirstResponsePDU structure below.

#ifndef HAMMERD_UPDATE_FW_H_
#define HAMMERD_UPDATE_FW_H_

#include <stdint.h>
#include <string>
#include <vector>

#include <base/macros.h>

#include "hammerd/usb_utils.h"

namespace hammerd {

const int kUpdateProtocolVersion = 6;
const uint32_t kUpdateDoneCmd = 0xB007AB1E;
const uint32_t kUpdateExtraCmd = 0xB007AB1F;

enum class FirstResponsePDUHeaderType {
  kCR50 = 0,
  kCommon = 1,
};

// The extra vendor subcommand.
enum class UpdateExtraCommand : uint16_t {
  kImmediateReset = 0,
  kJumpToRW = 1,
  kStayInRO = 2,
  kUnlockRW = 3,
};

// This is the frame format the host uses when sending update PDUs over USB.
//
// The PDUs are up to 1K bytes in size, they are fragmented into USB chunks of
// 64 bytes each and reassembled on the receive side before being passed to
// the flash update function.
//
// The flash update function receives the unframed PDU body, and puts its reply
// into the same buffer the PDU was in.
struct UpdateFrameHeader {
  uint32_t block_size;  // Total frame size, including this field.
  uint32_t block_digest;
  uint32_t block_base;
};

// Response to the connection establishment request.
//
// When responding to the very first packet of the update sequence, the
// original USB update implementation was responding with a four byte value,
// just as to any other block of the transfer sequence.
//
// It became clear that there is a need to be able to enhance the update
// protocol, while staying backwards compatible.
//
// All newer protocol versions (starting with version 2) respond to the very
// first packet with an 8 byte or larger response, where the first 4 bytes are
// a version specific data, and the second 4 bytes - the protocol version
// number.
//
// This way the host receiving of a four byte value in response to the first
// packet is considered an indication of the target running the 'legacy'
// protocol, version 1. Receiving of an 8 byte or longer response would
// communicates the protocol version in the second 4 bytes.
struct FirstResponsePDU {
  uint32_t return_value;

  // The below fields are present in versions 2 and up.
  // Type of header following (one of first_response_pdu_header_type)
  uint16_t header_type;
  uint16_t protocol_version;  // Must be kUpdateProtocolVersion
  uint32_t maximum_pdu_size;  // Maximum PDU size
  uint32_t flash_protection;  // Flash protection status
  uint32_t offset;            // Offset of the other region
  char version[32];           // Version string of the other region
  int32_t min_rollback;       // Minimum rollback version that RO will accept
  uint32_t key_version;       // RO public key version
};

// This array describes all four sections of the new image.
struct SectionInfo {
  std::string name;
  uint32_t offset;
  uint32_t size;
  char version[32];
  int32_t rollback;
  int32_t key_version;
  explicit SectionInfo(std::string name);
};

// Implement the core logic of updating firmware.
// It contains the data of the original transfer_descriptor.
class FirmwareUpdater {
 public:
  FirmwareUpdater();
  ~FirmwareUpdater();

  // Scans the new image and retrieve versions of RO and RW sections.
  bool LoadImage(const std::string& image);
  // Prints the information of the RO and RW sections.
  void ShowHeadersVersions();

  // Transfer the image to the target section.
  bool TransferImage(const std::string& section_name);

  // Send the external command through USB. The format of the payload is:
  //   4 bytes      4 bytes         4 bytes       2 bytes      variable size
  // +-----------+--------------+---------------+-----------+------~~~-------+
  // + total size| block digest |    EXT_CMD    | Vend. sub.|      data      |
  // +-----------+--------------+---------------+-----------+------~~~-------+
  //
  // Where 'Vend. sub' is the vendor subcommand, and data field is subcommand
  // dependent. The target tells between update PDUs and encapsulated vendor
  // subcommands by looking at the EXT_CMD value - it is kUpdateExtraCmd and
  // as such is guaranteed not to be a valid update PDU destination address.
  bool SendSubcommand(UpdateExtraCommand subcommand);

 private:
  // Setup the connection with the EC firmware by sending the first PDU.
  // Returns true if successfully setup the connection.
  bool SendFirstPDU();

  // Indicate to the target that update image transfer has been completed. Upon
  // receiveing of this message the target state machine transitions into the
  // 'rx_idle' state. The host may send an extension command to reset the target
  // after this.
  void SendDone();

  bool TransferSection(const uint8_t* data_ptr,
                       uint32_t section_addr,
                       size_t data_len);
  bool TransferBlock(UpdateFrameHeader* ufh,
                     const uint8_t* transfer_data_ptr,
                     size_t payload_size);

  // The USB endpoint to the hammer EC.
  UsbEndpoint uep_;
  // The information of the first response PDU.
  FirstResponsePDU targ_;
  // The image data to be updated.
  std::string image_;
  // The information of the RO and RW sections in the image data.
  std::vector<SectionInfo> sections_;

  DISALLOW_COPY_AND_ASSIGN(FirmwareUpdater);
};

}  // namespace hammerd
#endif  // HAMMERD_UPDATE_FW_H_
