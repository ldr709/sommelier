// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mock_tpm.h"

using testing::_;
using testing::DoAll;
using testing::SetArgumentPointee;
using testing::Return;

namespace cryptohome {

MockTpm::MockTpm() {
  ON_CALL(*this, Encrypt(_, _, _, _))
      .WillByDefault(Invoke(this, &MockTpm::Xor));
  ON_CALL(*this, Decrypt(_, _, _, _))
      .WillByDefault(Invoke(this, &MockTpm::Xor));
  ON_CALL(*this, IsConnected())
      .WillByDefault(Return(true));
  ON_CALL(*this, Connect(_))
      .WillByDefault(Return(true));
  ON_CALL(*this, GetPublicKey(_, _))
      .WillByDefault(Invoke(this, &MockTpm::GetBlankPublicKey));
  ON_CALL(*this, GetPublicKeyHash(_))
      .WillByDefault(Return(Tpm::Fatal));
  ON_CALL(*this, Init(_, _))
      .WillByDefault(Return(true));
  ON_CALL(*this, GetEndorsementPublicKey(_))
      .WillByDefault(Return(true));
  ON_CALL(*this, GetEndorsementCredential(_))
      .WillByDefault(DoAll(SetArgumentPointee<0>(chromeos::SecureBlob("test")),
                           Return(true)));
  ON_CALL(*this, MakeIdentity(_, _, _, _, _, _, _, _, _))
      .WillByDefault(Return(true));
  ON_CALL(*this, ActivateIdentity(_, _, _, _, _, _))
      .WillByDefault(Return(true));
  ON_CALL(*this, QuotePCR0(_, _, _, _, _))
      .WillByDefault(Return(true));
  ON_CALL(*this, SealToPCR0(_, _))
      .WillByDefault(Return(true));
  ON_CALL(*this, Unseal(_, _))
      .WillByDefault(Return(true));
  ON_CALL(*this, GetRandomData(_, _))
      .WillByDefault(Invoke(this, &MockTpm::FakeGetRandomData));
  ON_CALL(*this, CreateDelegate(_, _, _))
      .WillByDefault(Return(true));
  ON_CALL(*this, CreateCertifiedKey(_, _, _, _, _, _, _))
      .WillByDefault(Return(true));
  ON_CALL(*this, Sign(_, _, _))
      .WillByDefault(Return(true));
}

MockTpm::~MockTpm() {}

}  // namespace cryptohome
