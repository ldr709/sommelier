// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chaps/session_impl.h"

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/logging.h>
#include <brillo/secure_blob.h>
#include <crypto/scoped_openssl_types.h>
#include <openssl/bio.h>
#include <openssl/des.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>

#include "chaps/chaps.h"
#include "chaps/chaps_factory.h"
#include "chaps/chaps_utility.h"
#include "chaps/object.h"
#include "chaps/object_pool.h"
#include "chaps/tpm_utility.h"
#include "pkcs11/cryptoki.h"

#define CKK_INVALID_KEY_TYPE (CKK_VENDOR_DEFINED + 0)

using brillo::SecureBlob;
using ScopedASN1_OCTET_STRING =
    crypto::ScopedOpenSSL<ASN1_OCTET_STRING, ASN1_OCTET_STRING_free>;
using std::hex;
using std::map;
using std::set;
using std::string;
using std::vector;

namespace {

using chaps::Object;
using chaps::OperationType::kDecrypt;
using chaps::OperationType::kDigest;
using chaps::OperationType::kEncrypt;
using chaps::OperationType::kSign;
using chaps::OperationType::kVerify;

const int kDefaultAuthDataBytes = 20;
const int kMaxCipherBlockBytes = 16;
const int kMaxRSAOutputBytes = 2048;
const int kMaxDigestOutputBytes = EVP_MAX_MD_SIZE;
const int kMinRSAKeyBits = 512;
const int kMaxRSAKeyBits = kMaxRSAOutputBytes * 8;

CK_RV ResultToRV(chaps::ObjectPool::Result result, CK_RV fail_rv) {
  switch (result) {
    case chaps::ObjectPool::Result::Success:
      return CKR_OK;
    case chaps::ObjectPool::Result::Failure:
      return fail_rv;
    case chaps::ObjectPool::Result::WaitForPrivateObjects:
      return CKR_WOULD_BLOCK_FOR_PRIVATE_OBJECTS;
  }
}

bool IsSuccess(chaps::ObjectPool::Result result) {
  return result == chaps::ObjectPool::Result::Success;
}

class MechanismInfo {
 public:
  explicit MechanismInfo(CK_MECHANISM_TYPE mechanism);
  bool IsSupported() const;
  bool IsOperationValid(chaps::OperationType op) const;
  bool IsForKeyType(CK_KEY_TYPE keytype) const;

 private:
  struct MechanismInfoData {
    bool is_supported;
    set<chaps::OperationType> operation;
    CK_KEY_TYPE key_type;
  };

  static MechanismInfoData GetSupportedMechanismInfo(
      CK_MECHANISM_TYPE mechanism);

  MechanismInfoData data_;
};

MechanismInfo::MechanismInfo(CK_MECHANISM_TYPE mechanism)
    : data_(GetSupportedMechanismInfo(mechanism)) {}

bool MechanismInfo::IsSupported() const {
  return data_.is_supported;
}

bool MechanismInfo::IsOperationValid(chaps::OperationType op) const {
  return IsSupported() && data_.operation.count(op) > 0;
}

bool MechanismInfo::IsForKeyType(CK_KEY_TYPE keytype) const {
  return IsSupported() && data_.key_type == keytype;
}

MechanismInfo::MechanismInfoData MechanismInfo::GetSupportedMechanismInfo(
    CK_MECHANISM_TYPE mechanism) {
  switch (mechanism) {
    // DES
    case CKM_DES_ECB:
    case CKM_DES_CBC:
    case CKM_DES_CBC_PAD:
      return {true, {kEncrypt, kDecrypt}, CKK_DES};

    // DES3
    case CKM_DES3_ECB:
    case CKM_DES3_CBC:
    case CKM_DES3_CBC_PAD:
      return {true, {kEncrypt, kDecrypt}, CKK_DES3};

    // AES
    case CKM_AES_ECB:
    case CKM_AES_CBC:
    case CKM_AES_CBC_PAD:
      return {true, {kEncrypt, kDecrypt}, CKK_AES};

    // RSA
    case CKM_RSA_PKCS:
      return {true, {kEncrypt, kDecrypt, kSign, kVerify}, CKK_RSA};
    case CKM_MD5_RSA_PKCS:
    case CKM_SHA1_RSA_PKCS:
    case CKM_SHA256_RSA_PKCS:
    case CKM_SHA384_RSA_PKCS:
    case CKM_SHA512_RSA_PKCS:
      return {true, {kSign, kVerify}, CKK_RSA};

    // HMAC
    case CKM_MD5_HMAC:
    case CKM_SHA_1_HMAC:
    case CKM_SHA256_HMAC:
    case CKM_SHA384_HMAC:
    case CKM_SHA512_HMAC:
      return {true, {kSign, kVerify}, CKK_GENERIC_SECRET};

    // Digest
    case CKM_MD5:
    case CKM_SHA_1:
    case CKM_SHA256:
    case CKM_SHA384:
    case CKM_SHA512:
      return {true, {kDigest}, CKK_INVALID_KEY_TYPE};

    default:
      return {false, {}, CKK_INVALID_KEY_TYPE};
  }
}

bool IsHMAC(CK_MECHANISM_TYPE mechanism) {
  return MechanismInfo(mechanism).IsForKeyType(CKK_GENERIC_SECRET);
}

// Returns true if the given block cipher (AES/DES) mechanism uses padding.
bool IsPaddingEnabled(CK_MECHANISM_TYPE mechanism) {
  switch (mechanism) {
    case CKM_DES_CBC_PAD:
    case CKM_DES3_CBC_PAD:
    case CKM_AES_CBC_PAD:
      return true;

    default:
      return false;
  }
}

bool IsRSA(CK_MECHANISM_TYPE mechanism) {
  return MechanismInfo(mechanism).IsForKeyType(CKK_RSA);
}

bool IsECC(CK_MECHANISM_TYPE mechanism) {
  return MechanismInfo(mechanism).IsForKeyType(CKK_EC);
}

bool IsMechanismValidForOperation(chaps::OperationType operation,
                                  CK_MECHANISM_TYPE mechanism) {
  return MechanismInfo(mechanism).IsOperationValid(operation);
}

CK_OBJECT_CLASS GetExpectedObjectClass(chaps::OperationType operation,
                                       CK_KEY_TYPE key_type) {
  bool use_private_key = operation == kSign || operation == kDecrypt;
  switch (key_type) {
    case CKK_DES:
    case CKK_DES3:
    case CKK_AES:
      return CKO_SECRET_KEY;
    case CKK_RSA:
    case CKK_EC:
      return use_private_key ? CKO_PRIVATE_KEY : CKO_PUBLIC_KEY;
    case CKK_GENERIC_SECRET:
      return CKO_SECRET_KEY;

    default:
      // Never used
      NOTREACHED();
      return -1;
  }
}

// Check |object_class| and |key_type| is valid for |mechanism| and |operation|
bool IsValidKeyType(chaps::OperationType operation,
                    CK_MECHANISM_TYPE mechanism,
                    CK_OBJECT_CLASS object_class,
                    CK_KEY_TYPE key_type) {
  return MechanismInfo(mechanism).IsForKeyType(key_type) &&
         object_class == GetExpectedObjectClass(operation, key_type);
}

// TODO(crbug/916023): Move OpenSSL conversion function to a standalone
// libarary.
//
// Helpers to map PKCS #11 <--> OpenSSL.
template <typename OpenSSLType,
          int (*openssl_func)(OpenSSLType*, unsigned char**)>
string ConvertOpenSSLObjectToString(OpenSSLType* type) {
  string output;

  int expected_size = openssl_func(type, nullptr);
  if (expected_size < 0) {
    return string();
  }

  output.resize(expected_size, '\0');

  unsigned char* buf = chaps::ConvertStringToByteBuffer(output.data());
  int real_size = openssl_func(type, &buf);
  CHECK_EQ(expected_size, real_size);

  return output;
}

// Both PKCS #11 and OpenSSL use big-endian binary representations of big
// integers.  To convert we can just use the OpenSSL converters.
string ConvertFromBIGNUM(const BIGNUM* bignum) {
  string big_integer(BN_num_bytes(bignum), 0);
  BN_bn2bin(bignum, chaps::ConvertStringToByteBuffer(big_integer.data()));
  return big_integer;
}

// Caller take the ownership.
// Returns nullptr if big_integer is empty.
BIGNUM* ConvertToBIGNUM(const string& big_integer) {
  if (big_integer.empty())
    return nullptr;
  BIGNUM* b = BN_bin2bn(chaps::ConvertStringToByteBuffer(big_integer.data()),
                        big_integer.length(), nullptr);
  CHECK(b);
  return b;
}

const EVP_CIPHER* GetOpenSSLCipher(CK_MECHANISM_TYPE mechanism,
                                   size_t key_size) {
  switch (mechanism) {
    case CKM_DES_ECB:
      return EVP_des_ecb();
    case CKM_DES_CBC:
    case CKM_DES_CBC_PAD:
      return EVP_des_cbc();
    case CKM_DES3_ECB:
      return EVP_des_ede3();
    case CKM_DES3_CBC:
    case CKM_DES3_CBC_PAD:
      return EVP_des_ede3_cbc();
    case CKM_AES_ECB:
      switch (key_size) {
        case 16:
          return EVP_aes_128_ecb();
        case 24:
          return EVP_aes_192_ecb();
        default:
          return EVP_aes_256_ecb();
      }
      break;
    case CKM_AES_CBC:
    case CKM_AES_CBC_PAD:
      switch (key_size) {
        case 16:
          return EVP_aes_128_cbc();
        case 24:
          return EVP_aes_192_cbc();
        default:
          return EVP_aes_256_cbc();
      }
      break;
  }
  return nullptr;
}

const EVP_MD* GetOpenSSLDigest(CK_MECHANISM_TYPE mechanism) {
  switch (mechanism) {
    case CKM_MD5:
    case CKM_MD5_HMAC:
    case CKM_MD5_RSA_PKCS:
      return EVP_md5();
    case CKM_SHA_1:
    case CKM_SHA_1_HMAC:
    case CKM_SHA1_RSA_PKCS:
    case CKM_ECDSA_SHA1:
      return EVP_sha1();
    case CKM_SHA256:
    case CKM_SHA256_HMAC:
    case CKM_SHA256_RSA_PKCS:
      return EVP_sha256();
    case CKM_SHA384:
    case CKM_SHA384_HMAC:
    case CKM_SHA384_RSA_PKCS:
      return EVP_sha384();
    case CKM_SHA512:
    case CKM_SHA512_HMAC:
    case CKM_SHA512_RSA_PKCS:
      return EVP_sha512();
  }
  return nullptr;
}

// In short, we can use d2i_ECParameters to parse CKA_EC_PARAMS and return
// EC_KEY*
//
// CKA_EC_PARAMS is DER-encoding of an ANSI X9.62 Parameters value.
// Parameters ::= CHOICE {
//    ecParameters   ECParameters,
//    namedCurve     CURVES.&id({CurveNames}),
//    implicitlyCA   NULL
// }
// which is as known as EcPKParameters in OpenSSL and RFC 3279.
// EcPKParameters ::= CHOICE {
//    ecParameters  ECParameters,
//    namedCurve    OBJECT IDENTIFIER,
//    implicitlyCA  NULL
// }
crypto::ScopedEC_KEY CreateECCKeyFromEC_PARAMS(const string& input) {
  const unsigned char* buf = chaps::ConvertStringToByteBuffer(input.data());
  crypto::ScopedEC_KEY key(d2i_ECParameters(nullptr, &buf, input.size()));
  return key;
}

string GetECParametersAsString(const crypto::ScopedEC_KEY& key) {
  return ConvertOpenSSLObjectToString<EC_KEY, i2d_ECParameters>(key.get());
}

crypto::ScopedEC_KEY CreateECCPublicKeyFromObject(const Object* key_object) {
  // Start parsing EC_PARAMS
  string ec_params = key_object->GetAttributeString(CKA_EC_PARAMS);
  crypto::ScopedEC_KEY key = CreateECCKeyFromEC_PARAMS(ec_params);
  if (key == nullptr)
    return nullptr;

  // Start parsing EC_POINT
  // DER decode EC_POINT to OCT_STRING
  string pub_data = key_object->GetAttributeString(CKA_EC_POINT);
  const unsigned char* buf = chaps::ConvertStringToByteBuffer(pub_data.data());
  ScopedASN1_OCTET_STRING os(
      d2i_ASN1_OCTET_STRING(nullptr, &buf, pub_data.size()));
  if (os == nullptr)
    return nullptr;

  // Convert OCT_STRING to *EC_KEY
  buf = os->data;
  EC_KEY* key_ptr = key.get();
  key_ptr = o2i_ECPublicKey(&key_ptr, &buf, os->length);
  if (key_ptr == nullptr)
    return nullptr;
  CHECK_EQ(key_ptr, key.get());
  return key;
}

crypto::ScopedEC_KEY CreateECCPrivateKeyFromObject(const Object* key_object) {
  // Parse EC_PARAMS
  string ec_params = key_object->GetAttributeString(CKA_EC_PARAMS);
  crypto::ScopedEC_KEY key = CreateECCKeyFromEC_PARAMS(ec_params);
  if (key == nullptr)
    return nullptr;

  crypto::ScopedBIGNUM d(
      ConvertToBIGNUM(key_object->GetAttributeString(CKA_VALUE)));
  if (d == nullptr)
    return nullptr;

  if (!EC_KEY_set_private_key(key.get(), d.get()))
    return nullptr;

  return key;
}

// CKA_EC_POINT is DER-encoding of ANSI X9.62 ECPoint value
// The format should be 04 LEN 04 X Y, where the first 04 is the octet string
// tag, LEN is the the content length, the second 04 identifies the uncompressed
// form, and X and Y are the point coordinates.
//
// i2o_ECPublicKey() returns only the content (04 X Y).
string GetECPointAsString(const crypto::ScopedEC_KEY& key) {
  // Convert EC_KEY* to OCT_STRING
  const string oct_string =
      ConvertOpenSSLObjectToString<EC_KEY, i2o_ECPublicKey>(key.get());
  if (oct_string.empty())
    return string();

  // Put OCT_STRING to ASN1_OCTET_STRING
  ScopedASN1_OCTET_STRING os(ASN1_OCTET_STRING_new());
  ASN1_OCTET_STRING_set(os.get(),
                        chaps::ConvertStringToByteBuffer(oct_string.data()),
                        oct_string.size());

  // DER encode ASN1_OCTET_STRING
  const string der_encoded =
      ConvertOpenSSLObjectToString<ASN1_OCTET_STRING, i2d_ASN1_OCTET_STRING>(
          os.get());

  return der_encoded;
}

// TODO(menghuan): Move Create*KeyFromObject to the member function of object.
// Always returns a non-NULL value.
crypto::ScopedRSA CreateRSAKeyFromObject(const chaps::Object* key_object) {
  crypto::ScopedRSA rsa(RSA_new());
  CHECK_NE(rsa, nullptr);
  if (key_object->GetObjectClass() == CKO_PUBLIC_KEY) {
    string e = key_object->GetAttributeString(CKA_PUBLIC_EXPONENT);
    rsa->e = ConvertToBIGNUM(e);
    string n = key_object->GetAttributeString(CKA_MODULUS);
    rsa->n = ConvertToBIGNUM(n);
  } else {  // key_object->GetObjectClass() == CKO_PRIVATE_KEY
    string n = key_object->GetAttributeString(CKA_MODULUS);
    rsa->n = ConvertToBIGNUM(n);
    string d = key_object->GetAttributeString(CKA_PRIVATE_EXPONENT);
    rsa->d = ConvertToBIGNUM(d);
    string p = key_object->GetAttributeString(CKA_PRIME_1);
    rsa->p = ConvertToBIGNUM(p);
    string q = key_object->GetAttributeString(CKA_PRIME_2);
    rsa->q = ConvertToBIGNUM(q);
    string dmp1 = key_object->GetAttributeString(CKA_EXPONENT_1);
    rsa->dmp1 = ConvertToBIGNUM(dmp1);
    string dmq1 = key_object->GetAttributeString(CKA_EXPONENT_2);
    rsa->dmq1 = ConvertToBIGNUM(dmq1);
    string iqmp = key_object->GetAttributeString(CKA_COEFFICIENT);
    rsa->iqmp = ConvertToBIGNUM(iqmp);
  }
  return rsa;
}

}  // namespace

namespace chaps {

SessionImpl::SessionImpl(int slot_id,
                         ObjectPool* token_object_pool,
                         TPMUtility* tpm_utility,
                         ChapsFactory* factory,
                         HandleGenerator* handle_generator,
                         bool is_read_only)
    : factory_(factory),
      find_results_valid_(false),
      is_read_only_(is_read_only),
      slot_id_(slot_id),
      token_object_pool_(token_object_pool),
      tpm_utility_(tpm_utility),
      is_legacy_loaded_(false),
      private_root_key_(0),
      public_root_key_(0) {
  CHECK(token_object_pool_);
  CHECK(tpm_utility_);
  CHECK(factory_);
  session_object_pool_.reset(
      factory_->CreateObjectPool(handle_generator, nullptr, nullptr));
  CHECK(session_object_pool_.get());
}

SessionImpl::~SessionImpl() {}

int SessionImpl::GetSlot() const {
  return slot_id_;
}

CK_STATE SessionImpl::GetState() const {
  return is_read_only_ ? CKS_RO_USER_FUNCTIONS : CKS_RW_USER_FUNCTIONS;
}

bool SessionImpl::IsReadOnly() const {
  return is_read_only_;
}

bool SessionImpl::IsOperationActive(OperationType type) const {
  CHECK(type < kNumOperationTypes);
  return operation_context_[type].is_valid_;
}

CK_RV SessionImpl::CreateObject(const CK_ATTRIBUTE_PTR attributes,
                                int num_attributes,
                                int* new_object_handle) {
  return CreateObjectInternal(attributes, num_attributes, nullptr,
                              new_object_handle);
}

CK_RV SessionImpl::CopyObject(const CK_ATTRIBUTE_PTR attributes,
                              int num_attributes,
                              int object_handle,
                              int* new_object_handle) {
  const Object* orig_object = nullptr;
  if (!GetObject(object_handle, &orig_object))
    return CKR_OBJECT_HANDLE_INVALID;
  CHECK(orig_object);
  return CreateObjectInternal(attributes, num_attributes, orig_object,
                              new_object_handle);
}

CK_RV SessionImpl::DestroyObject(int object_handle) {
  const Object* object = nullptr;
  if (!GetObject(object_handle, &object))
    return CKR_OBJECT_HANDLE_INVALID;
  CHECK(object);
  ObjectPool* pool =
      object->IsTokenObject() ? token_object_pool_ : session_object_pool_.get();
  return ResultToRV(pool->Delete(object), CKR_GENERAL_ERROR);
}

bool SessionImpl::GetObject(int object_handle, const Object** object) {
  CHECK(object);
  if (token_object_pool_->FindByHandle(object_handle, object) ==
      ObjectPool::Result::Success)
    return true;
  return session_object_pool_->FindByHandle(object_handle, object) ==
         ObjectPool::Result::Success;
}

bool SessionImpl::GetModifiableObject(int object_handle, Object** object) {
  CHECK(object);
  const Object* const_object;
  if (!GetObject(object_handle, &const_object))
    return false;
  ObjectPool* pool = const_object->IsTokenObject() ? token_object_pool_
                                                   : session_object_pool_.get();
  *object = pool->GetModifiableObject(const_object);
  return true;
}

CK_RV SessionImpl::FlushModifiableObject(Object* object) {
  CHECK(object);
  ObjectPool* pool =
      object->IsTokenObject() ? token_object_pool_ : session_object_pool_.get();
  return ResultToRV(pool->Flush(object), CKR_FUNCTION_FAILED);
}

CK_RV SessionImpl::FindObjectsInit(const CK_ATTRIBUTE_PTR attributes,
                                   int num_attributes) {
  if (find_results_valid_)
    return CKR_OPERATION_ACTIVE;
  std::unique_ptr<Object> search_template(factory_->CreateObject());
  CHECK(search_template.get());
  search_template->SetAttributes(attributes, num_attributes);
  vector<const Object*> objects;
  if (!search_template->IsAttributePresent(CKA_TOKEN) ||
      search_template->IsTokenObject()) {
    auto res = token_object_pool_->Find(search_template.get(), &objects);
    if (!IsSuccess(res))
      return ResultToRV(res, CKR_GENERAL_ERROR);
  }
  if (!search_template->IsAttributePresent(CKA_TOKEN) ||
      !search_template->IsTokenObject()) {
    auto res = session_object_pool_->Find(search_template.get(), &objects);
    if (!IsSuccess(res))
      return ResultToRV(res, CKR_GENERAL_ERROR);
  }
  find_results_.clear();
  find_results_offset_ = 0;
  find_results_valid_ = true;
  for (size_t i = 0; i < objects.size(); ++i) {
    find_results_.push_back(objects[i]->handle());
  }
  return CKR_OK;
}

CK_RV SessionImpl::FindObjects(int max_object_count,
                               vector<int>* object_handles) {
  CHECK(object_handles);
  if (!find_results_valid_)
    return CKR_OPERATION_NOT_INITIALIZED;
  size_t end_offset =
      find_results_offset_ + static_cast<size_t>(max_object_count);
  if (end_offset > find_results_.size())
    end_offset = find_results_.size();
  for (size_t i = find_results_offset_; i < end_offset; ++i) {
    object_handles->push_back(find_results_[i]);
  }
  find_results_offset_ += object_handles->size();
  return CKR_OK;
}

CK_RV SessionImpl::FindObjectsFinal() {
  if (!find_results_valid_)
    return CKR_OPERATION_NOT_INITIALIZED;
  find_results_valid_ = false;
  return CKR_OK;
}

CK_RV SessionImpl::OperationInit(OperationType operation,
                                 CK_MECHANISM_TYPE mechanism,
                                 const string& mechanism_parameter,
                                 const Object* key) {
  CHECK(operation < kNumOperationTypes);

  OperationContext* context = &operation_context_[operation];
  if (context->is_valid_) {
    LOG(ERROR) << "Operation is already active.";
    return CKR_OPERATION_ACTIVE;
  }

  context->Clear();
  context->mechanism_ = mechanism;
  context->parameter_ = mechanism_parameter;

  if (!IsMechanismValidForOperation(operation, mechanism)) {
    LOG(ERROR) << "Mechanism not supported: 0x" << hex << mechanism;
    return CKR_MECHANISM_INVALID;
  }

  if (operation == kSign || operation == kVerify || operation == kEncrypt ||
      operation == kDecrypt) {
    // Make sure the key is valid for the mechanism.
    CHECK(key);
    if (!IsValidKeyType(
            operation, mechanism, key->GetObjectClass(),
            key->GetAttributeInt(CKA_KEY_TYPE, CK_UNAVAILABLE_INFORMATION))) {
      LOG(ERROR) << "Key type mismatch.";
      return CKR_KEY_TYPE_INCONSISTENT;
    }
    if (!key->GetAttributeBool(GetRequiredKeyUsage(operation), false)) {
      LOG(ERROR) << "Key function not permitted.";
      return CKR_KEY_FUNCTION_NOT_PERMITTED;
    }
    if (IsRSA(mechanism)) {
      // Refuse to use RSA keys with unsupported sizes that may have been
      // created in an earlier version of chaps.
      int key_size = key->GetAttributeString(CKA_MODULUS).length() * 8;
      if (key_size < kMinRSAKeyBits || key_size > kMaxRSAKeyBits) {
        LOG(ERROR) << "Key size not supported: " << key_size;
        return CKR_KEY_SIZE_RANGE;
      }
    }
  }

  if (operation == kEncrypt || operation == kDecrypt) {
    if (mechanism == CKM_RSA_PKCS) {
      context->key_ = key;
      context->is_valid_ = true;
    } else {
      return CipherInit((operation == kEncrypt), mechanism, mechanism_parameter,
                        key);
    }
  } else if (operation == kSign || operation == kVerify ||
             operation == kDigest) {
    // It is valid for GetOpenSSLDigest to return NULL (e.g. CKM_RSA_PKCS).
    const EVP_MD* digest = GetOpenSSLDigest(mechanism);
    if (IsHMAC(mechanism)) {
      string key_material = key->GetAttributeString(CKA_VALUE);
      HMAC_CTX_init(&context->hmac_context_);
      HMAC_Init_ex(&context->hmac_context_, key_material.data(),
                   key_material.length(), digest, nullptr);
      context->is_hmac_ = true;
    } else if (digest) {
      EVP_DigestInit(&context->digest_context_, digest);
      context->is_digest_ = true;
    }
    if (IsRSA(mechanism) || IsECC(mechanism))
      context->key_ = key;
    context->is_valid_ = true;
  } else {
    NOTREACHED();
    return CKR_FUNCTION_FAILED;
  }
  return CKR_OK;
}

CK_RV SessionImpl::OperationUpdate(OperationType operation,
                                   const string& data_in,
                                   int* required_out_length,
                                   string* data_out) {
  CHECK(operation < kNumOperationTypes);
  OperationContext* context = &operation_context_[operation];
  if (!context->is_valid_) {
    LOG(ERROR) << "Operation is not initialized.";
    return CKR_OPERATION_NOT_INITIALIZED;
  }
  if (context->is_finished_) {
    LOG(ERROR) << "Operation is finished.";
    OperationCancel(operation);
    return CKR_OPERATION_ACTIVE;
  }
  context->is_incremental_ = true;
  return OperationUpdateInternal(operation, data_in, required_out_length,
                                 data_out);
}

CK_RV SessionImpl::OperationUpdateInternal(OperationType operation,
                                           const string& data_in,
                                           int* required_out_length,
                                           string* data_out) {
  CHECK(operation < kNumOperationTypes);
  OperationContext* context = &operation_context_[operation];
  if (context->is_cipher_) {
    CK_RV rv = CipherUpdate(context, data_in, required_out_length, data_out);
    if ((rv != CKR_OK) && (rv != CKR_BUFFER_TOO_SMALL))
      OperationCancel(operation);
    return rv;
  } else if (context->is_digest_) {
    EVP_DigestUpdate(&context->digest_context_, data_in.data(),
                     data_in.length());
  } else if (context->is_hmac_) {
    HMAC_Update(&context->hmac_context_,
                ConvertStringToByteBuffer(data_in.c_str()), data_in.length());
  } else {
    // We don't need to process now; just queue the data.
    context->data_ += data_in;
  }
  if (required_out_length)
    *required_out_length = 0;
  return CKR_OK;
}

void SessionImpl::OperationCancel(OperationType operation) {
  CHECK(operation < kNumOperationTypes);
  OperationContext* context = &operation_context_[operation];
  if (!context->is_valid_) {
    LOG(ERROR) << "Operation is not initialized.";
    return;
  }
  // Drop the context and any associated data.
  context->Clear();
}

CK_RV SessionImpl::OperationFinal(OperationType operation,
                                  int* required_out_length,
                                  string* data_out) {
  CHECK(required_out_length);
  CHECK(data_out);
  CHECK(operation < kNumOperationTypes);
  OperationContext* context = &operation_context_[operation];
  if (!context->is_valid_) {
    LOG(ERROR) << "Operation is not initialized.";
    return CKR_OPERATION_NOT_INITIALIZED;
  }
  if (!context->is_incremental_ && context->is_finished_) {
    LOG(ERROR) << "Operation is not incremental.";
    OperationCancel(operation);
    return CKR_OPERATION_ACTIVE;
  }
  context->is_incremental_ = true;
  return OperationFinalInternal(operation, required_out_length, data_out);
}

CK_RV SessionImpl::OperationFinalInternal(OperationType operation,
                                          int* required_out_length,
                                          string* data_out) {
  CHECK(operation < kNumOperationTypes);

  OperationContext* context = &operation_context_[operation];
  context->is_valid_ = false;

  // Complete the operation if it has not already been done.
  if (!context->is_finished_) {
    if (context->is_cipher_) {
      CK_RV result = CipherFinal(context);
      if (result != CKR_OK)
        return result;
    } else if (context->is_digest_) {
      unsigned char buffer[kMaxDigestOutputBytes];
      unsigned int out_length = 0;
      EVP_DigestFinal(&context->digest_context_, buffer, &out_length);
      context->data_ = string(reinterpret_cast<char*>(buffer), out_length);
    } else if (context->is_hmac_) {
      unsigned char buffer[kMaxDigestOutputBytes];
      unsigned int out_length = 0;
      HMAC_Final(&context->hmac_context_, buffer, &out_length);
      HMAC_CTX_cleanup(&context->hmac_context_);
      context->data_ = string(reinterpret_cast<char*>(buffer), out_length);
    }

    // Some RSA/ECC mechanisms use a digest so it's important to finish the
    // digest before finishing the RSA/ECC computation.
    if (IsRSA(context->mechanism_)) {
      if (operation == kEncrypt) {
        if (!RSAEncrypt(context))
          return CKR_FUNCTION_FAILED;
      } else if (operation == kDecrypt) {
        if (!RSADecrypt(context))
          return CKR_FUNCTION_FAILED;
      } else if (operation == kSign) {
        if (!RSASign(context))
          return CKR_FUNCTION_FAILED;
      }
    } else if (IsECC(context->mechanism_)) {
      if (operation == kSign) {
        if (!ECCSign(context))
          return CKR_FUNCTION_FAILED;
      }
    }
    context->is_finished_ = true;
  }
  CK_RV result = GetOperationOutput(context, required_out_length, data_out);
  if (result == CKR_BUFFER_TOO_SMALL) {
    // We'll keep the context valid so a subsequent call can pick up the data.
    context->is_valid_ = true;
  }
  return result;
}

CK_RV SessionImpl::VerifyFinal(const string& signature) {
  OperationContext* context = &operation_context_[kVerify];
  // Call the generic OperationFinal so any digest or HMAC computation gets
  // finalized.
  int max_out_length = std::numeric_limits<int>::max();
  string data_out;
  CK_RV result = OperationFinal(kVerify, &max_out_length, &data_out);
  if (result != CKR_OK)
    return result;

  // We only support 3 Verify mechanisms, HMAC, RSA and ECC.
  if (context->is_hmac_) {
    // The data_out contents will be the computed HMAC. To verify an HMAC, it is
    // recomputed and literally compared.
    if (signature.length() != data_out.length())
      return CKR_SIGNATURE_LEN_RANGE;

    if (0 != brillo::SecureMemcmp(signature.data(), data_out.data(),
                                  signature.length()))
      return CKR_SIGNATURE_INVALID;

    return CKR_OK;
  } else if (IsRSA(context->mechanism_)) {
    // The data_out contents will be the computed digest.
    return RSAVerify(context, data_out, signature);
  } else if (IsECC(context->mechanism_)) {
    // The data_out contents will be the computed digest.
    return ECCVerify(context, data_out, signature);
  } else {
    NOTREACHED();
    return false;
  }
}

CK_RV SessionImpl::OperationSinglePart(OperationType operation,
                                       const string& data_in,
                                       int* required_out_length,
                                       string* data_out) {
  CHECK(operation < kNumOperationTypes);
  OperationContext* context = &operation_context_[operation];
  if (!context->is_valid_) {
    LOG(ERROR) << "Operation is not initialized.";
    return CKR_OPERATION_NOT_INITIALIZED;
  }
  if (context->is_incremental_) {
    LOG(ERROR) << "Operation is incremental.";
    OperationCancel(operation);
    return CKR_OPERATION_ACTIVE;
  }
  CK_RV result = CKR_OK;
  if (!context->is_finished_) {
    string update, final;
    int max = std::numeric_limits<int>::max();
    result = OperationUpdateInternal(operation, data_in, &max, &update);
    if (result != CKR_OK)
      return result;
    max = std::numeric_limits<int>::max();
    result = OperationFinalInternal(operation, &max, &final);
    if (result != CKR_OK)
      return result;
    context->data_ = update + final;
    context->is_finished_ = true;
  }
  context->is_valid_ = false;
  result = GetOperationOutput(context, required_out_length, data_out);
  if (result == CKR_BUFFER_TOO_SMALL) {
    // We'll keep the context valid so a subsequent call can pick up the data.
    context->is_valid_ = true;
  }
  return result;
}

CK_RV SessionImpl::GenerateKey(CK_MECHANISM_TYPE mechanism,
                               const string& mechanism_parameter,
                               const CK_ATTRIBUTE_PTR attributes,
                               int num_attributes,
                               int* new_key_handle) {
  CHECK(new_key_handle);
  std::unique_ptr<Object> object(factory_->CreateObject());
  CHECK(object.get());
  CK_RV result = object->SetAttributes(attributes, num_attributes);
  if (result != CKR_OK)
    return result;
  CK_KEY_TYPE key_type = 0;
  string key_material;
  switch (mechanism) {
    case CKM_DES_KEY_GEN: {
      key_type = CKK_DES;
      if (!GenerateDESKey(&key_material))
        return CKR_FUNCTION_FAILED;
      break;
    }
    case CKM_DES3_KEY_GEN: {
      key_type = CKK_DES3;
      string des[3];
      for (int i = 0; i < 3; ++i) {
        if (!GenerateDESKey(&des[i]))
          return CKR_FUNCTION_FAILED;
      }
      key_material = des[0] + des[1] + des[2];
      break;
    }
    case CKM_AES_KEY_GEN: {
      key_type = CKK_AES;
      if (!object->IsAttributePresent(CKA_VALUE_LEN))
        return CKR_TEMPLATE_INCOMPLETE;
      CK_ULONG key_length = object->GetAttributeInt(CKA_VALUE_LEN, 0);
      if (key_length != 16 && key_length != 24 && key_length != 32)
        return CKR_KEY_SIZE_RANGE;
      key_material = GenerateRandomSoftware(key_length);
      break;
    }
    case CKM_GENERIC_SECRET_KEY_GEN: {
      key_type = CKK_GENERIC_SECRET;
      if (!object->IsAttributePresent(CKA_VALUE_LEN))
        return CKR_TEMPLATE_INCOMPLETE;
      CK_ULONG key_length = object->GetAttributeInt(CKA_VALUE_LEN, 0);
      if (key_length < 1)
        return CKR_KEY_SIZE_RANGE;
      key_material = GenerateRandomSoftware(key_length);
      break;
    }
    default: {
      LOG(ERROR) << "GenerateKey: Mechanism not supported: " << hex
                 << mechanism;
      return CKR_MECHANISM_INVALID;
    }
  }
  object->SetAttributeInt(CKA_CLASS, CKO_SECRET_KEY);
  object->SetAttributeInt(CKA_KEY_TYPE, key_type);
  object->SetAttributeString(CKA_VALUE, key_material);
  object->SetAttributeBool(CKA_LOCAL, true);
  object->SetAttributeInt(CKA_KEY_GEN_MECHANISM, mechanism);
  result = object->FinalizeNewObject();
  if (result != CKR_OK)
    return result;
  ObjectPool* pool =
      object->IsTokenObject() ? token_object_pool_ : session_object_pool_.get();
  auto pool_res = pool->Insert(object.get());
  if (!IsSuccess(pool_res))
    return ResultToRV(pool_res, CKR_FUNCTION_FAILED);
  *new_key_handle = object.release()->handle();
  return CKR_OK;
}

CK_RV SessionImpl::GenerateKeyPair(CK_MECHANISM_TYPE mechanism,
                                   const string& mechanism_parameter,
                                   const CK_ATTRIBUTE_PTR public_attributes,
                                   int num_public_attributes,
                                   const CK_ATTRIBUTE_PTR private_attributes,
                                   int num_private_attributes,
                                   int* new_public_key_handle,
                                   int* new_private_key_handle) {
  CHECK(new_public_key_handle);
  CHECK(new_private_key_handle);

  // Create public/private key objects
  std::unique_ptr<Object> public_object(factory_->CreateObject());
  CHECK(public_object.get());
  std::unique_ptr<Object> private_object(factory_->CreateObject());
  CHECK(private_object.get());

  // copy attributes
  // TODO(menghuan): don't copy the attribute that doesn't support
  CK_RV result =
      public_object->SetAttributes(public_attributes, num_public_attributes);
  if (result != CKR_OK)
    return result;
  result =
      private_object->SetAttributes(private_attributes, num_private_attributes);
  if (result != CKR_OK)
    return result;

  // Get the object pool
  ObjectPool* public_pool =
      (public_object->IsTokenObject() ? token_object_pool_
                                      : session_object_pool_.get());
  ObjectPool* private_pool =
      (private_object->IsTokenObject() ? token_object_pool_
                                       : session_object_pool_.get());

  switch (mechanism) {
    case CKM_RSA_PKCS_KEY_PAIR_GEN:
      result = GenerateRSAKeyPair(public_object.get(), private_object.get());
      break;
    case CKM_EC_KEY_PAIR_GEN:
      result = GenerateECCKeyPair(public_object.get(), private_object.get());
      break;
    default:
      LOG(ERROR) << __func__ << ": Mechanism not supported: " << hex
                 << mechanism;
      return CKR_MECHANISM_INVALID;
  }
  if (result != CKR_OK) {
    return result;
  }

  // Set the general attributes for public / private key
  public_object->SetAttributeInt(CKA_CLASS, CKO_PUBLIC_KEY);
  private_object->SetAttributeInt(CKA_CLASS, CKO_PRIVATE_KEY);

  // The CKA_KEY_GEN_MECHANISM attribute identifies the key generation mechanism
  // used to generate the key material. It contains a valid value only if the
  // CKA_LOCAL attribute has the value CK_TRUE. If CKA_LOCAL has the value
  // CK_FALSE, the value of the attribute is CK_UNAVAILABLE_INFORMATION.
  public_object->SetAttributeBool(CKA_LOCAL, true);
  private_object->SetAttributeBool(CKA_LOCAL, true);
  public_object->SetAttributeInt(CKA_KEY_GEN_MECHANISM, mechanism);
  private_object->SetAttributeInt(CKA_KEY_GEN_MECHANISM, mechanism);

  // Finalize the objects
  result = public_object->FinalizeNewObject();
  if (result != CKR_OK) {
    LOG(ERROR) << __func__ << ": Fail to finalize public object.";
    return result;
  }
  result = private_object->FinalizeNewObject();
  if (result != CKR_OK) {
    LOG(ERROR) << __func__ << ": Fail to finalize private object.";
    return result;
  }
  auto pool_res = public_pool->Insert(public_object.get());
  if (!IsSuccess(pool_res)) {
    LOG(ERROR) << __func__ << ": Fail to insert public object to public pool.";
    return ResultToRV(pool_res, CKR_FUNCTION_FAILED);
  }
  pool_res = private_pool->Insert(private_object.get());
  if (!IsSuccess(pool_res)) {
    LOG(ERROR) << __func__
               << ": Fail to insert private object to private pool.";
    // Remove inserted public object.
    // The object will be destroy in Delete(), we should release uniptr.
    public_pool->Delete(public_object.release());
    return ResultToRV(pool_res, CKR_FUNCTION_FAILED);
  }
  *new_public_key_handle = public_object.release()->handle();
  *new_private_key_handle = private_object.release()->handle();
  return CKR_OK;
}

CK_RV SessionImpl::SeedRandom(const string& seed) {
  RAND_seed(seed.data(), seed.length());
  return CKR_OK;
}

CK_RV SessionImpl::GenerateRandom(int num_bytes, string* random_data) {
  *random_data = GenerateRandomSoftware(num_bytes);
  return CKR_OK;
}

bool SessionImpl::IsPrivateLoaded() {
  return token_object_pool_->IsPrivateLoaded();
}

CK_RV SessionImpl::CipherInit(bool is_encrypt,
                              CK_MECHANISM_TYPE mechanism,
                              const string& mechanism_parameter,
                              const Object* key) {
  OperationType operation = is_encrypt ? kEncrypt : kDecrypt;
  EVP_CIPHER_CTX* context = &operation_context_[operation].cipher_context_;
  string key_material = key->GetAttributeString(CKA_VALUE);
  const EVP_CIPHER* cipher_type =
      GetOpenSSLCipher(mechanism, key_material.size());
  if (!cipher_type) {
    LOG(ERROR) << "Mechanism not supported: 0x" << hex << mechanism;
    return CKR_MECHANISM_INVALID;
  }
  // The mechanism parameter is the IV for cipher modes which require an IV,
  // otherwise it is expected to be empty.
  if (static_cast<int>(mechanism_parameter.size()) !=
      EVP_CIPHER_iv_length(cipher_type)) {
    LOG(ERROR) << "IV length is invalid: " << mechanism_parameter.size();
    return CKR_MECHANISM_PARAM_INVALID;
  }
  if (static_cast<int>(key_material.size()) !=
      EVP_CIPHER_key_length(cipher_type)) {
    LOG(ERROR) << "Key size not supported: " << key_material.size();
    return CKR_KEY_SIZE_RANGE;
  }
  if (!EVP_CipherInit(
          context, cipher_type, ConvertStringToByteBuffer(key_material.c_str()),
          ConvertStringToByteBuffer(mechanism_parameter.c_str()), is_encrypt)) {
    LOG(ERROR) << "EVP_CipherInit failed: " << GetOpenSSLError();
    return CKR_FUNCTION_FAILED;
  }
  EVP_CIPHER_CTX_set_padding(context, IsPaddingEnabled(mechanism));
  operation_context_[operation].is_valid_ = true;
  operation_context_[operation].is_cipher_ = true;
  return CKR_OK;
}

CK_RV SessionImpl::CipherUpdate(OperationContext* context,
                                const string& data_in,
                                int* required_out_length,
                                string* data_out) {
  CHECK(required_out_length);
  CHECK(data_out);
  // If we have output already waiting, we don't need to process input.
  if (context->data_.empty()) {
    int in_length = data_in.length();
    int out_length = in_length + kMaxCipherBlockBytes;
    context->data_.resize(out_length);
    if (!EVP_CipherUpdate(
            &context->cipher_context_,
            ConvertStringToByteBuffer(context->data_.c_str()), &out_length,
            ConvertStringToByteBuffer(data_in.c_str()), in_length)) {
      EVP_CIPHER_CTX_cleanup(&context->cipher_context_);
      context->is_valid_ = false;
      LOG(ERROR) << "EVP_CipherUpdate failed: " << GetOpenSSLError();
      return CKR_FUNCTION_FAILED;
    }
    context->data_.resize(out_length);
  }
  return GetOperationOutput(context, required_out_length, data_out);
}

CK_RV SessionImpl::CipherFinal(OperationContext* context) {
  if (context->data_.empty()) {
    int out_length = kMaxCipherBlockBytes * 2;
    context->data_.resize(out_length);
    if (!EVP_CipherFinal(&context->cipher_context_,
                         ConvertStringToByteBuffer(context->data_.c_str()),
                         &out_length)) {
      LOG(ERROR) << "EVP_CipherFinal failed: " << GetOpenSSLError();
      EVP_CIPHER_CTX_cleanup(&context->cipher_context_);
      return CKR_FUNCTION_FAILED;
    }
    EVP_CIPHER_CTX_cleanup(&context->cipher_context_);
    context->data_.resize(out_length);
  }
  return CKR_OK;
}

CK_RV SessionImpl::CreateObjectInternal(const CK_ATTRIBUTE_PTR attributes,
                                        int num_attributes,
                                        const Object* copy_from_object,
                                        int* new_object_handle) {
  CHECK(new_object_handle);
  CHECK(attributes || num_attributes == 0);
  std::unique_ptr<Object> object(factory_->CreateObject());
  CHECK(object.get());
  CK_RV result = CKR_OK;
  if (copy_from_object) {
    result = object->Copy(copy_from_object);
    if (result != CKR_OK)
      return result;
  }
  result = object->SetAttributes(attributes, num_attributes);
  if (result != CKR_OK)
    return result;
  if (!copy_from_object) {
    result = object->FinalizeNewObject();
    if (result != CKR_OK)
      return result;
  }
  ObjectPool* pool = token_object_pool_;
  if (object->IsTokenObject()) {
    result = WrapPrivateKey(object.get());
    if (result != CKR_OK)
      return result;
  } else {
    pool = session_object_pool_.get();
  }
  auto pool_res = pool->Insert(object.get());
  if (!IsSuccess(pool_res))
    return ResultToRV(pool_res, CKR_GENERAL_ERROR);
  *new_object_handle = object.release()->handle();
  return CKR_OK;
}

bool SessionImpl::GenerateDESKey(string* key_material) {
  static const int kDESKeySizeBytes = 8;
  bool done = false;
  while (!done) {
    string tmp = GenerateRandomSoftware(kDESKeySizeBytes);
    DES_cblock des;
    memcpy(&des, tmp.data(), kDESKeySizeBytes);
    if (!DES_is_weak_key(&des)) {
      DES_set_odd_parity(&des);
      *key_material = string(reinterpret_cast<char*>(des), kDESKeySizeBytes);
      done = true;
    }
  }
  return true;
}

CK_RV SessionImpl::GenerateRSAKeyPair(Object* public_object,
                                      Object* private_object) {
  // CKA_PUBLIC_EXPONENT is optional. The default is 65537 (0x10001).
  string public_exponent("\x01\x00\x01", 3);
  if (public_object->IsAttributePresent(CKA_PUBLIC_EXPONENT))
    public_exponent = public_object->GetAttributeString(CKA_PUBLIC_EXPONENT);
  public_object->SetAttributeString(CKA_PUBLIC_EXPONENT, public_exponent);
  private_object->SetAttributeString(CKA_PUBLIC_EXPONENT, public_exponent);

  // CKA_MODULUS_BITS is requried
  if (!public_object->IsAttributePresent(CKA_MODULUS_BITS))
    return CKR_TEMPLATE_INCOMPLETE;
  CK_ULONG modulus_bits = public_object->GetAttributeInt(CKA_MODULUS_BITS, 0);
  if (modulus_bits < kMinRSAKeyBits || modulus_bits > kMaxRSAKeyBits)
    return CKR_KEY_SIZE_RANGE;

  // Set CKA_KEY_TYPE
  public_object->SetAttributeInt(CKA_KEY_TYPE, CKK_RSA);
  private_object->SetAttributeInt(CKA_KEY_TYPE, CKK_RSA);

  // Check if we are able to back this key with the TPM.
  if (tpm_utility_->IsTPMAvailable() && private_object->IsTokenObject() &&
      modulus_bits >= tpm_utility_->MinRSAKeyBits() &&
      modulus_bits <= tpm_utility_->MaxRSAKeyBits()) {
    // Use TPM to generate RSA key
    if (!GenerateRSAKeyPairTPM(modulus_bits, public_exponent, public_object,
                               private_object))
      return CKR_FUNCTION_FAILED;
  } else {
    // Use software to generate RSA key
    if (!GenerateRSAKeyPairSoftware(modulus_bits, public_exponent,
                                    public_object, private_object))
      return CKR_FUNCTION_FAILED;
  }
  return CKR_OK;
}

bool SessionImpl::GenerateRSAKeyPairSoftware(int modulus_bits,
                                             const string& public_exponent,
                                             Object* public_object,
                                             Object* private_object) {
  if (public_exponent.length() > sizeof(uint32_t) || public_exponent.empty())
    return false;
  crypto::ScopedBIGNUM e(ConvertToBIGNUM(public_exponent));
  if (e == nullptr)
    return false;
  crypto::ScopedRSA key(
      RSA_generate_key(modulus_bits, BN_get_word(e.get()), nullptr, nullptr));
  if (key == nullptr)
    return false;
  string n = ConvertFromBIGNUM(key->n);
  string d = ConvertFromBIGNUM(key->d);
  string p = ConvertFromBIGNUM(key->p);
  string q = ConvertFromBIGNUM(key->q);
  string dmp1 = ConvertFromBIGNUM(key->dmp1);
  string dmq1 = ConvertFromBIGNUM(key->dmq1);
  string iqmp = ConvertFromBIGNUM(key->iqmp);
  public_object->SetAttributeString(CKA_MODULUS, n);
  private_object->SetAttributeString(CKA_MODULUS, n);
  private_object->SetAttributeString(CKA_PRIVATE_EXPONENT, d);
  private_object->SetAttributeString(CKA_PRIME_1, p);
  private_object->SetAttributeString(CKA_PRIME_2, q);
  private_object->SetAttributeString(CKA_EXPONENT_1, dmp1);
  private_object->SetAttributeString(CKA_EXPONENT_2, dmq1);
  private_object->SetAttributeString(CKA_COEFFICIENT, iqmp);
  return true;
}

bool SessionImpl::GenerateRSAKeyPairTPM(int modulus_bits,
                                        const string& public_exponent,
                                        Object* public_object,
                                        Object* private_object) {
  string auth_data = GenerateRandomSoftware(kDefaultAuthDataBytes);
  string key_blob;
  int tpm_key_handle;
  if (!tpm_utility_->GenerateKey(slot_id_, modulus_bits, public_exponent,
                                 SecureBlob(auth_data.begin(), auth_data.end()),
                                 &key_blob, &tpm_key_handle))
    return false;

  // Get public key information from TPM
  string modulus;
  string exponent;
  if (!tpm_utility_->GetPublicKey(tpm_key_handle, &exponent, &modulus))
    return false;

  public_object->SetAttributeString(CKA_MODULUS, modulus);
  private_object->SetAttributeString(CKA_MODULUS, modulus);
  private_object->SetAttributeString(kAuthDataAttribute, auth_data);
  private_object->SetAttributeString(kKeyBlobAttribute, key_blob);
  return true;
}

CK_RV SessionImpl::GenerateECCKeyPair(Object* public_object,
                                      Object* private_object) {
  // CKA_EC_PARAMS is requried
  if (!public_object->IsAttributePresent(CKA_EC_PARAMS))
    return CKR_TEMPLATE_INCOMPLETE;

  crypto::ScopedEC_KEY key = CreateECCKeyFromEC_PARAMS(
      public_object->GetAttributeString(CKA_EC_PARAMS));
  if (key == nullptr) {
    LOG(ERROR) << __func__ << ": CKA_EC_PARAMS parse fail.";
    return CKR_DOMAIN_PARAMS_INVALID;
  }

  // Set CKA_KEY_TYPE
  public_object->SetAttributeInt(CKA_KEY_TYPE, CKK_EC);
  private_object->SetAttributeInt(CKA_KEY_TYPE, CKK_EC);

  // reset CKA_EC_PARAMS for both key
  const string ec_params = GetECParametersAsString(key);
  if (ec_params.empty()) {
    LOG(ERROR) << __func__ << ": Fail to dump CKA_EC_PARAMS";
    return CKR_FUNCTION_FAILED;
  }
  public_object->SetAttributeString(CKA_EC_PARAMS, ec_params);
  private_object->SetAttributeString(CKA_EC_PARAMS, ec_params);

  // Software generate key
  if (!EC_KEY_generate_key(key.get())) {
    LOG(ERROR) << __func__
               << ": Software generate key fail. Perhaps it is not supported "
                  "by OpenSSL.";
    return CKR_DOMAIN_PARAMS_INVALID;
  }

  // Set CKA_EC_POINT for public key
  const string ec_point = GetECPointAsString(key);
  if (ec_point.empty()) {
    LOG(ERROR) << __func__ << ": Fail to dump EC_POINT.";
    return CKR_FUNCTION_FAILED;
  }
  public_object->SetAttributeString(CKA_EC_POINT, ec_point);

  // Set CKA_VALUE for private key
  const BIGNUM* privkey = EC_KEY_get0_private_key(key.get());
  private_object->SetAttributeString(CKA_VALUE, ConvertFromBIGNUM(privkey));

  return CKR_OK;
}

string SessionImpl::GenerateRandomSoftware(int num_bytes) {
  string random(num_bytes, 0);
  RAND_bytes(ConvertStringToByteBuffer(random.data()), num_bytes);
  return random;
}

string SessionImpl::GetDERDigestInfo(CK_MECHANISM_TYPE mechanism) {
  const EVP_MD* md = GetOpenSSLDigest(mechanism);
  if (md == EVP_md5()) {
    return GetDigestAlgorithmEncoding(DigestAlgorithm::MD5);
  } else if (md == EVP_sha1()) {
    return GetDigestAlgorithmEncoding(DigestAlgorithm::SHA1);
  } else if (md == EVP_sha256()) {
    return GetDigestAlgorithmEncoding(DigestAlgorithm::SHA256);
  } else if (md == EVP_sha384()) {
    return GetDigestAlgorithmEncoding(DigestAlgorithm::SHA384);
  } else if (md == EVP_sha512()) {
    return GetDigestAlgorithmEncoding(DigestAlgorithm::SHA512);
  }
  // This is valid in some cases (e.g. CKM_RSA_PKCS).
  return string();
}

CK_RV SessionImpl::GetOperationOutput(OperationContext* context,
                                      int* required_out_length,
                                      string* data_out) {
  int out_length = context->data_.length();
  int max_length = *required_out_length;
  *required_out_length = out_length;
  if (max_length < out_length)
    return CKR_BUFFER_TOO_SMALL;
  *data_out = context->data_;
  context->data_.clear();
  return CKR_OK;
}

CK_ATTRIBUTE_TYPE SessionImpl::GetRequiredKeyUsage(OperationType operation) {
  switch (operation) {
    case kEncrypt:
      return CKA_ENCRYPT;
    case kDecrypt:
      return CKA_DECRYPT;
    case kSign:
      return CKA_SIGN;
    case kVerify:
      return CKA_VERIFY;
    default:
      break;
  }
  return 0;
}

bool SessionImpl::GetTPMKeyHandle(const Object* key, int* key_handle) {
  map<const Object*, int>::iterator it = object_tpm_handle_map_.find(key);
  if (it == object_tpm_handle_map_.end()) {
    // Only private keys are loaded into the TPM. All public key operations do
    // not use the TPM (and use OpenSSL instead).
    if (key->GetObjectClass() == CKO_PRIVATE_KEY) {
      string auth_data = key->GetAttributeString(kAuthDataAttribute);
      if (key->GetAttributeBool(kLegacyAttribute, false)) {
        // This is a legacy key and it needs to be loaded with the legacy root
        // key.
        if (!LoadLegacyRootKeys())
          return false;
        bool is_private = key->GetAttributeBool(CKA_PRIVATE, true);
        int root_key_handle = is_private ? private_root_key_ : public_root_key_;
        if (!tpm_utility_->LoadKeyWithParent(
                slot_id_, key->GetAttributeString(kKeyBlobAttribute),
                SecureBlob(auth_data.begin(), auth_data.end()), root_key_handle,
                key_handle))
          return false;
      } else {
        if (!tpm_utility_->LoadKey(
                slot_id_, key->GetAttributeString(kKeyBlobAttribute),
                SecureBlob(auth_data.begin(), auth_data.end()), key_handle))
          return false;
      }
    } else {
      LOG(ERROR) << "Invalid object class for loading into TPM.";
      return false;
    }
    object_tpm_handle_map_[key] = *key_handle;
  } else {
    *key_handle = it->second;
  }
  return true;
}

bool SessionImpl::LoadLegacyRootKeys() {
  if (is_legacy_loaded_)
    return true;

  // Load the legacy root keys. See http://trousers.sourceforge.net/pkcs11.html
  // for details on where these come from.
  string private_blob;
  if (!token_object_pool_->GetInternalBlob(kLegacyPrivateRootKey,
                                           &private_blob)) {
    LOG(ERROR) << "Failed to read legacy private root key blob.";
    return false;
  }
  if (!tpm_utility_->LoadKey(slot_id_, private_blob, SecureBlob(),
                             &private_root_key_)) {
    LOG(ERROR) << "Failed to load legacy private root key.";
    return false;
  }
  string public_blob;
  if (!token_object_pool_->GetInternalBlob(kLegacyPublicRootKey,
                                           &public_blob)) {
    LOG(ERROR) << "Failed to read legacy public root key blob.";
    return false;
  }
  if (!tpm_utility_->LoadKey(slot_id_, public_blob, SecureBlob(),
                             &public_root_key_)) {
    LOG(ERROR) << "Failed to load legacy public root key.";
    return false;
  }
  is_legacy_loaded_ = true;
  return true;
}

bool SessionImpl::RSADecrypt(OperationContext* context) {
  if (context->key_->IsTokenObject() &&
      context->key_->IsAttributePresent(kKeyBlobAttribute)) {
    int tpm_key_handle = 0;
    if (!GetTPMKeyHandle(context->key_, &tpm_key_handle))
      return false;
    string encrypted_data = context->data_;
    context->data_.clear();
    if (!tpm_utility_->Unbind(tpm_key_handle, encrypted_data, &context->data_))
      return false;
  } else {
    crypto::ScopedRSA rsa = CreateRSAKeyFromObject(context->key_);
    uint8_t buffer[kMaxRSAOutputBytes];
    CHECK(RSA_size(rsa.get()) <= kMaxRSAOutputBytes);
    int length = RSA_private_decrypt(
        context->data_.length(),
        ConvertStringToByteBuffer(context->data_.data()), buffer, rsa.get(),
        RSA_PKCS1_PADDING);  // Strips PKCS #1 type 2 padding.
    if (length == -1) {
      LOG(ERROR) << "RSA_private_decrypt failed: " << GetOpenSSLError();
      return false;
    }
    context->data_ = ConvertByteBufferToString(buffer, length);
  }
  return true;
}

bool SessionImpl::RSAEncrypt(OperationContext* context) {
  crypto::ScopedRSA rsa = CreateRSAKeyFromObject(context->key_);
  uint8_t buffer[kMaxRSAOutputBytes];
  CHECK(RSA_size(rsa.get()) <= kMaxRSAOutputBytes);
  int length = RSA_public_encrypt(
      context->data_.length(), ConvertStringToByteBuffer(context->data_.data()),
      buffer, rsa.get(),
      RSA_PKCS1_PADDING);  // Adds PKCS #1 type 2 padding.
  if (length == -1) {
    LOG(ERROR) << "RSA_public_encrypt failed: " << GetOpenSSLError();
    return false;
  }
  context->data_ = ConvertByteBufferToString(buffer, length);
  return true;
}

bool SessionImpl::RSASign(OperationContext* context) {
  string data_to_sign = GetDERDigestInfo(context->mechanism_) + context->data_;
  string signature;
  if (context->key_->IsTokenObject() &&
      context->key_->IsAttributePresent(kKeyBlobAttribute)) {
    int tpm_key_handle = 0;
    if (!GetTPMKeyHandle(context->key_, &tpm_key_handle))
      return false;
    if (!tpm_utility_->Sign(tpm_key_handle, data_to_sign, &signature))
      return false;
  } else {
    crypto::ScopedRSA rsa = CreateRSAKeyFromObject(context->key_);
    CHECK(RSA_size(rsa.get()) <= kMaxRSAOutputBytes);
    uint8_t buffer[kMaxRSAOutputBytes];
    int length = RSA_private_encrypt(
        data_to_sign.length(), ConvertStringToByteBuffer(data_to_sign.data()),
        buffer, rsa.get(),
        RSA_PKCS1_PADDING);  // Adds PKCS #1 type 1 padding.
    if (length == -1) {
      LOG(ERROR) << "RSA_private_encrypt failed: " << GetOpenSSLError();
      return false;
    }
    signature = string(reinterpret_cast<char*>(buffer), length);
  }
  context->data_ = signature;
  return true;
}

CK_RV SessionImpl::RSAVerify(OperationContext* context,
                             const string& digest,
                             const string& signature) {
  if (context->key_->GetAttributeString(CKA_MODULUS).length() !=
      signature.length())
    return CKR_SIGNATURE_LEN_RANGE;
  crypto::ScopedRSA rsa = CreateRSAKeyFromObject(context->key_);
  CHECK(RSA_size(rsa.get()) <= kMaxRSAOutputBytes);
  uint8_t buffer[kMaxRSAOutputBytes];
  int length = RSA_public_decrypt(
      signature.length(), ConvertStringToByteBuffer(signature.data()), buffer,
      rsa.get(),
      RSA_PKCS1_PADDING);  // Strips PKCS #1 type 1 padding.
  if (length == -1) {
    LOG(ERROR) << "RSA_public_decrypt failed: " << GetOpenSSLError();
    return CKR_SIGNATURE_INVALID;
  }
  string signed_data = GetDERDigestInfo(context->mechanism_) + digest;
  if (static_cast<size_t>(length) != signed_data.length() ||
      0 != brillo::SecureMemcmp(buffer, signed_data.data(), length))
    return CKR_SIGNATURE_INVALID;
  return CKR_OK;
}

bool SessionImpl::ECCSign(OperationContext* context) {
  const string& data_to_sign = context->data_;

  // Software Sign with ECC key
  crypto::ScopedEC_KEY key = CreateECCPrivateKeyFromObject(context->key_);
  if (key == nullptr) {
    LOG(ERROR) << __func__ << ": Load key failed.";
    return false;
  }

  // We don't use ECDSA_sign here since the output format of PKCS#11 is
  // different from OpenSSL's.
  crypto::ScopedECDSA_SIG sig(
      ECDSA_do_sign(ConvertStringToByteBuffer(data_to_sign.data()),
                    data_to_sign.size(), key.get()));
  if (sig == nullptr) {
    LOG(ERROR) << __func__ << ": ECDSA failed: " << GetOpenSSLError();
    return false;
  }

  // The resulting signature is always of length 2 * nLen.
  // The first half of the signature is r and the second half is s
  const string& signature =
      ConvertFromBIGNUM(sig->r) + ConvertFromBIGNUM(sig->s);

  context->data_ = signature;
  return true;
}

CK_RV SessionImpl::ECCVerify(OperationContext* context,
                             const string& signed_data,
                             const string& signature) {
  // Software verify with ECC key
  crypto::ScopedEC_KEY key = CreateECCPublicKeyFromObject(context->key_);
  if (key == nullptr) {
    LOG(ERROR) << __func__ << ": Load key failed.";
    return CKR_FUNCTION_FAILED;
  }

  // Parse signature back to ECDSA_SIG
  int sign_size = signature.size();
  if (sign_size % 2 != 0) {
    return CKR_SIGNATURE_LEN_RANGE;
  }
  crypto::ScopedECDSA_SIG sig(ECDSA_SIG_new());

  // ECDSA_SIG_new populates ECDSA_SIG with two newly allocated BIGNUMs. We need
  // to free them before replacing them with new ones created by
  // ConvertToBIGNUM.
  // TODO(menghuan): use ECDSA_SIG_set0() after upgrading to OpenSSL 1.1.0
  BN_free(sig->r);
  BN_free(sig->s);
  sig->r = ConvertToBIGNUM(signature.substr(0, sign_size / 2));
  sig->s = ConvertToBIGNUM(signature.substr(sign_size / 2));

  // 1 for a valid signature, 0 for an invalid signature and -1 on error.
  int result = ECDSA_do_verify(ConvertStringToByteBuffer(signed_data.data()),
                               signed_data.size(), sig.get(), key.get());
  if (result < 0) {
    LOG(ERROR) << __func__ << ": ECDSA verify failed: " << GetOpenSSLError();
    return CKR_FUNCTION_FAILED;
  }
  return result ? CKR_OK : CKR_SIGNATURE_INVALID;
}

CK_RV SessionImpl::WrapPrivateKey(Object* object) {
  if (!tpm_utility_->IsTPMAvailable() ||
      object->GetObjectClass() != CKO_PRIVATE_KEY ||
      object->IsAttributePresent(kKeyBlobAttribute)) {
    // This object does not need to be wrapped.
    return CKR_OK;
  }
  if (!object->IsAttributePresent(CKA_PUBLIC_EXPONENT) ||
      !object->IsAttributePresent(CKA_MODULUS) ||
      !(object->IsAttributePresent(CKA_PRIME_1) ||
        object->IsAttributePresent(CKA_PRIME_2)))
    return CKR_TEMPLATE_INCOMPLETE;
  string prime;
  if (object->IsAttributePresent(CKA_PRIME_1)) {
    prime = object->GetAttributeString(CKA_PRIME_1);
  } else {
    prime = object->GetAttributeString(CKA_PRIME_2);
  }
  int key_size_bits = object->GetAttributeString(CKA_MODULUS).length() * 8;
  if (key_size_bits > tpm_utility_->MaxRSAKeyBits() ||
      key_size_bits < tpm_utility_->MinRSAKeyBits()) {
    LOG(WARNING) << "WARNING: " << key_size_bits
                 << "-bit private key cannot be wrapped by the TPM.";
    // Fall back to software.
    return CKR_OK;
  }
  string auth_data = GenerateRandomSoftware(kDefaultAuthDataBytes);
  string key_blob;
  int tpm_key_handle = 0;
  if (!tpm_utility_->WrapKey(slot_id_,
                             object->GetAttributeString(CKA_PUBLIC_EXPONENT),
                             object->GetAttributeString(CKA_MODULUS), prime,
                             SecureBlob(auth_data.begin(), auth_data.end()),
                             &key_blob, &tpm_key_handle))
    return CKR_FUNCTION_FAILED;
  object->SetAttributeString(kAuthDataAttribute, auth_data);
  object->SetAttributeString(kKeyBlobAttribute, key_blob);
  object->RemoveAttribute(CKA_PRIVATE_EXPONENT);
  object->RemoveAttribute(CKA_PRIME_1);
  object->RemoveAttribute(CKA_PRIME_2);
  object->RemoveAttribute(CKA_EXPONENT_1);
  object->RemoveAttribute(CKA_EXPONENT_2);
  object->RemoveAttribute(CKA_COEFFICIENT);
  return CKR_OK;
}

SessionImpl::OperationContext::OperationContext()
    : is_valid_(false),
      is_cipher_(false),
      is_digest_(false),
      is_hmac_(false),
      is_finished_(false),
      key_(nullptr) {}

SessionImpl::OperationContext::~OperationContext() {
  Clear();
}

void SessionImpl::OperationContext::Clear() {
  if (is_valid_ && !is_finished_) {
    if (is_cipher_) {
      EVP_CIPHER_CTX_cleanup(&cipher_context_);
    } else if (is_digest_) {
      EVP_MD_CTX_cleanup(&digest_context_);
    } else if (is_hmac_) {
      HMAC_CTX_cleanup(&hmac_context_);
    }
  }
  is_valid_ = false;
  is_cipher_ = false;
  is_digest_ = false;
  is_hmac_ = false;
  is_incremental_ = false;
  is_finished_ = false;
  key_ = nullptr;
  data_.clear();
  parameter_.clear();
}

}  // namespace chaps
