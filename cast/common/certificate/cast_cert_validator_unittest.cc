// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/common/certificate/cast_cert_validator.h"

#include <stdio.h>
#include <string.h>

#include "cast/common/certificate/date_time.h"
#include "cast/common/certificate/testing/test_helpers.h"
#include "cast/common/public/trust_store.h"
#include "gtest/gtest.h"
#include "openssl/pem.h"
#include "platform/test/byte_view_test_util.h"
#include "platform/test/paths.h"
#include "util/crypto/pem_helpers.h"

namespace openscreen {
namespace cast {
namespace {

enum TrustStoreDependency {
  // Uses the built-in trust store for Cast. This is how certificates are
  // verified in production.
  TRUST_STORE_BUILTIN,

  // Instead of using the built-in trust store, use root certificate in the
  // provided test chain as the trust anchor.
  //
  // This trust anchor is initialized with anchor constraints, similar to how
  // TrustAnchors in the built-in store are setup.
  TRUST_STORE_FROM_TEST_FILE,
};

// Reads a test chain from |certs_file_name|, and asserts that verifying it as
// a Cast device certificate yields |expected_result|.
//
// RunTest() also checks that the resulting device certificate does not
// incorrectly verify invalid signatures.
//
//  * |expected_policy| - The policy that should have been identified for the
//                        device certificate.
//  * |time| - The timestamp to use when verifying the certificate.
//  * |trust_store_dependency| - Which trust store to use when verifying (see
//                               enum's definition).
//  * |optional_signed_data_file_name| - optional path to a PEM file containing
//        a valid signature generated by the device certificate.
//
void RunTest(Error::Code expected_result,
             const std::string& expected_common_name,
             CastDeviceCertPolicy expected_policy,
             const std::string& certs_file_name,
             const DateTime& time,
             TrustStoreDependency trust_store_dependency,
             const std::string& optional_signed_data_file_name) {
  std::vector<std::string> certs = ReadCertificatesFromPemFile(certs_file_name);
  std::unique_ptr<TrustStore> trust_store;

  switch (trust_store_dependency) {
    case TRUST_STORE_BUILTIN:
      trust_store = CastTrustStore::Create();
      break;

    case TRUST_STORE_FROM_TEST_FILE: {
      ASSERT_FALSE(certs.empty());

      // Parse the root certificate of the chain.
      std::vector<uint8_t> data(certs.back().begin(), certs.back().end());
      certs.pop_back();
      trust_store = TrustStore::CreateInstanceForTest(data);
    }
  }

  std::unique_ptr<ParsedCertificate> target_cert;
  CastDeviceCertPolicy policy;

  Error result = VerifyDeviceCert(certs, time, &target_cert, &policy, nullptr,
                                  CRLPolicy::kCrlOptional, trust_store.get());

  ASSERT_EQ(expected_result, result.code());
  if (expected_result != Error::Code::kNone)
    return;

  EXPECT_EQ(expected_policy, policy);
  ASSERT_TRUE(target_cert);

  // Test that the target certificate is named as we expect.
  EXPECT_EQ(expected_common_name, target_cert->GetCommonName());

  // Test verification of some invalid signatures.
  EXPECT_FALSE(target_cert->VerifySignedData(
      DigestAlgorithm::kSha256, ByteViewFromLiteral("bogus data"),
      ByteViewFromLiteral("bogus signature")));
  EXPECT_FALSE(target_cert->VerifySignedData(
      DigestAlgorithm::kSha256, ByteViewFromLiteral("bogus data"), ByteView()));
  EXPECT_FALSE(target_cert->VerifySignedData(DigestAlgorithm::kSha256,
                                             ByteView(), ByteView()));

  // If valid signatures are known for this device certificate, test them.
  if (!optional_signed_data_file_name.empty()) {
    testing::SignatureTestData signatures =
        testing::ReadSignatureTestData(optional_signed_data_file_name);

    // Test verification of a valid SHA1 signature.
    EXPECT_TRUE(target_cert->VerifySignedData(
        DigestAlgorithm::kSha1, signatures.message, signatures.sha1));

    // Test verification of a valid SHA256 signature.
    EXPECT_TRUE(target_cert->VerifySignedData(
        DigestAlgorithm::kSha256, signatures.message, signatures.sha256));
  }
}

// Creates a time in UTC at midnight.
DateTime CreateDate(int year, int month, int day) {
  DateTime time = {};
  time.year = year;
  time.month = month;
  time.day = day;
  return time;
}

// Returns 2016-04-01 00:00:00 UTC.
//
// This is a time when most of the test certificate paths are valid.
DateTime AprilFirst2016() {
  return CreateDate(2016, 4, 1);
}

DateTime AprilFirst2020() {
  return CreateDate(2020, 4, 1);
}

// Returns 2015-01-01 00:00:00 UTC.
DateTime JanuaryFirst2015() {
  return CreateDate(2015, 1, 1);
}

// Returns 2037-03-01 00:00:00 UTC.
//
// This is so far in the future that the test chains in this unit-test should
// all be invalid.
DateTime MarchFirst2037() {
  return CreateDate(2037, 3, 1);
}

const std::string& GetSpecificTestDataPath() {
  static std::string data_path =
      GetTestDataPath() + "/cast/common/certificate/";
  return data_path;
}

// Tests verifying a valid certificate chain of length 2:
//
//   0: 2ZZBG9 FA8FCA3EF91A
//   1: Eureka Gen1 ICA
//
// Chains to trust anchor:
//   Eureka Root CA    (built-in trust store)
TEST(VerifyCastDeviceCertTest, ChromecastGen1) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "2ZZBG9 FA8FCA3EF91A",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/chromecast_gen1.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN,
          data_path + "signeddata/2ZZBG9_FA8FCA3EF91A.pem");
}

// Tests verifying a valid certificate chain of length 2:
//
//  0: 2ZZBG9 FA8FCA3EF91A
//  1: Eureka Gen1 ICA
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
TEST(VerifyCastDeviceCertTest, ChromecastGen1Reissue) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "2ZZBG9 FA8FCA3EF91A",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/chromecast_gen1_reissue.pem",
          AprilFirst2016(), TRUST_STORE_BUILTIN,
          data_path + "signeddata/2ZZBG9_FA8FCA3EF91A.pem");
}

// Tests verifying a valid certificate chain of length 2:
//
//   0: 3ZZAK6 FA8FCA3F0D35
//   1: Chromecast ICA 3
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
TEST(VerifyCastDeviceCertTest, ChromecastGen2) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "3ZZAK6 FA8FCA3F0D35",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/chromecast_gen2.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying a valid certificate chain of length 3:
//
//   0: -6394818897508095075
//   1: Asus fugu Cast ICA
//   2: Widevine Cast Subroot
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
TEST(VerifyCastDeviceCertTest, Fugu) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "-6394818897508095075",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/fugu.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying an invalid certificate chain of length 1:
//
//  0: Cast Test Untrusted Device
//
// Chains to:
//   Cast Test Untrusted ICA    (Not part of trust store)
//
// This is invalid because it does not chain to a trust anchor.
TEST(VerifyCastDeviceCertTest, Unchained) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kErrCertsVerifyUntrustedCert, "",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/unchained.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying one of the self-signed trust anchors (chain of length 1):
//
//  0: Cast Root CA
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
//
// Although this is a valid and trusted certificate (it is one of the
// trust anchors after all) it fails the test as it is not a *device
// certificate*.
TEST(VerifyCastDeviceCertTest, CastRootCa) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kErrCertsRestrictions, "",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/cast_root_ca.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying a valid certificate chain of length 2:
//
//  0: 4ZZDZJ FA8FCA7EFE3C
//  1: Chromecast ICA 4 (Audio)
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
//
// This device certificate has a policy that means it is valid only for audio
// devices.
TEST(VerifyCastDeviceCertTest, ChromecastAudio) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "4ZZDZJ FA8FCA7EFE3C",
          CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/chromecast_audio.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying a valid certificate chain of length 3:
//
//  0: MediaTek Audio Dev Test
//  1: MediaTek Audio Dev Model
//  2: Cast Audio Dev Root CA
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
//
// This device certificate has a policy that means it is valid only for audio
// devices.
TEST(VerifyCastDeviceCertTest, MtkAudioDev) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "MediaTek Audio Dev Test",
          CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/mtk_audio_dev.pem", JanuaryFirst2015(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying a valid certificate chain of length 2:
//
//  0: 9V0000VB FA8FCA784D01
//  1: Cast TV ICA (Vizio)
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
TEST(VerifyCastDeviceCertTest, Vizio) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "9V0000VB FA8FCA784D01",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/vizio.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying a valid certificate chain of length 2 using expired
// time points.
TEST(VerifyCastDeviceCertTest, ChromecastGen2InvalidTime) {
  const std::string certs_file =
      GetSpecificTestDataPath() + "certificates/chromecast_gen2.pem";

  // Control test - certificate should be valid at some time otherwise
  // this test is pointless.
  RunTest(Error::Code::kNone, "3ZZAK6 FA8FCA3F0D35",
          CastDeviceCertPolicy::kUnrestricted, certs_file, AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");

  // Use a time before notBefore.
  RunTest(Error::Code::kErrCertsDateInvalid, "",
          CastDeviceCertPolicy::kUnrestricted, certs_file, JanuaryFirst2015(),
          TRUST_STORE_BUILTIN, "");

  // Use a time after notAfter.
  RunTest(Error::Code::kErrCertsDateInvalid, "",
          CastDeviceCertPolicy::kUnrestricted, certs_file, MarchFirst2037(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying a valid certificate chain of length 3:
//
//  0: Audio Reference Dev Test
//  1: Audio Reference Dev Model
//  2: Cast Audio Dev Root CA
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
//
// This device certificate has a policy that means it is valid only for audio
// devices.
TEST(VerifyCastDeviceCertTest, AudioRefDevTestChain3) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Audio Reference Dev Test",
          CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/audio_ref_dev_test_chain_3.pem",
          AprilFirst2016(), TRUST_STORE_BUILTIN,
          data_path + "signeddata/AudioReferenceDevTest.pem");
}

// TODO(btolsch): This won't work by default with boringssl, so do we want to
// find a way to work around this or is it safe to enforce 20-octet length now?
// Previous TODO from eroman@ suggested 2017 or even sooner was safe to remove
// this.
#if 0
// Tests verifying a valid certificate chain of length 3. Note that the first
// intermediate has a serial number that is 21 octets long, which violates RFC
// 5280. However cast verification accepts this certificate for compatibility
// reasons.
//
//  0: 8C579B806FFC8A9DFFFF F8:8F:CA:6B:E6:DA
//  1: Sony so16vic CA
//  2: Cast Audio Sony CA
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
//
// This device certificate has a policy that means it is valid only for audio
// devices.
TEST(VerifyCastDeviceCertTest, IntermediateSerialNumberTooLong) {
  RunTest(Error::Code::kNone, "8C579B806FFC8A9DFFFF F8:8F:CA:6B:E6:DA",
          CastDeviceCertPolicy::AUDIO_ONLY,
          "certificates/intermediate_serialnumber_toolong.pem",
          AprilFirst2016(), TRUST_STORE_BUILTIN, "");
}
#endif

// Tests verifying a valid certificate chain of length 2 when the trust anchor
// is "expired". This is expected to work since expiration is not an enforced
// anchor constraint, even though it may appear in the root certificate.
//
//  0: CastDevice
//  1: CastIntermediate
//
// Chains to trust anchor:
//   Expired CastRoot     (provided by test data)
TEST(VerifyCastDeviceCertTest, ExpiredTrustAnchor) {
  std::string data_path = GetSpecificTestDataPath();
  // The root certificate is only valid in 2015, so validating with a time in
  // 2016 means it is expired.
  RunTest(Error::Code::kNone, "CastDevice", CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/expired_root.pem", AprilFirst2016(),
          TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain where the root certificate has a pathlen
// constraint which is violated by the chain. In this case Root has a pathlen=1
// constraint, however neither intermediate is constrained.
//
// The expectation is for pathlen constraints on trust anchors to be enforced,
// so this validation must fail.
//
//  0: Target
//  1: Intermediate2
//  2: Intermediate1
//
// Chains to trust anchor:
//   Root     (provided by test data; has pathlen=1 constraint)
TEST(VerifyCastDeviceCertTest, ViolatesPathlenTrustAnchorConstraint) {
  std::string data_path = GetSpecificTestDataPath();
  // Test that the chain verification fails due to the pathlen constraint.
  RunTest(Error::Code::kErrCertsPathlen, "Target",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/violates_root_pathlen_constraint.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain with the policies:
//
//  Root:           policies={}
//  Intermediate:   policies={anyPolicy}
//  Leaf:           policies={anyPolicy}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAnypolicyLeafAnypolicy) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/policies_ica_anypolicy_leaf_anypolicy.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={anyPolicy}
//   Leaf:           policies={audioOnly}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAnypolicyLeafAudioonly) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/policies_ica_anypolicy_leaf_audioonly.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={anyPolicy}
//   Leaf:           policies={foo}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAnypolicyLeafFoo) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/policies_ica_anypolicy_leaf_foo.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={anyPolicy}
//   Leaf:           policies={}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAnypolicyLeafNone) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/policies_ica_anypolicy_leaf_none.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={audioOnly}
//   Leaf:           policies={anyPolicy}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAudioonlyLeafAnypolicy) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/policies_ica_audioonly_leaf_anypolicy.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={audioOnly}
//   Leaf:           policies={audioOnly}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAudioonlyLeafAudioonly) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/policies_ica_audioonly_leaf_audioonly.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={audioOnly}
//   Leaf:           policies={foo}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAudioonlyLeafFoo) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/policies_ica_audioonly_leaf_foo.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={audioOnly}
//   Leaf:           policies={}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAudioonlyLeafNone) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/policies_ica_audioonly_leaf_none.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={}
//   Leaf:           policies={anyPolicy}
TEST(VerifyCastDeviceCertTest, PoliciesIcaNoneLeafAnypolicy) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/policies_ica_none_leaf_anypolicy.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={}
//   Leaf:           policies={audioOnly}
TEST(VerifyCastDeviceCertTest, PoliciesIcaNoneLeafAudioonly) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          data_path + "certificates/policies_ica_none_leaf_audioonly.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={}
//   Leaf:           policies={foo}
TEST(VerifyCastDeviceCertTest, PoliciesIcaNoneLeafFoo) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/policies_ica_none_leaf_foo.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={}
//   Leaf:           policies={}
TEST(VerifyCastDeviceCertTest, PoliciesIcaNoneLeafNone) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/policies_ica_none_leaf_none.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain where the leaf certificate has a
// 1024-bit RSA key. Verification should fail since the target's key is
// too weak.
TEST(VerifyCastDeviceCertTest, DeviceCertHas1024BitRsaKey) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kErrCertsVerifyGeneric, "RSA 1024 Device Cert",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/rsa1024_device_cert.pem", AprilFirst2016(),
          TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain where the leaf certificate has a
// 2048-bit RSA key, and then verifying signed data (both SHA1 and SHA256)
// for it.
TEST(VerifyCastDeviceCertTest, DeviceCertHas2048BitRsaKey) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "RSA 2048 Device Cert",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/rsa2048_device_cert.pem", AprilFirst2016(),
          TRUST_STORE_FROM_TEST_FILE,
          data_path + "signeddata/rsa2048_device_cert_data.pem");
}

// Tests verifying a certificate chain where an intermediate certificate has a
// nameConstraints extension but the leaf certificate is still permitted under
// these constraints.
TEST(VerifyCastDeviceCertTest, NameConstraintsObeyed) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kNone, "Device", CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/nc.pem", AprilFirst2020(),
          TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain where an intermediate certificate has a
// nameConstraints extension and the leaf certificate is not permitted under
// these constraints.
TEST(VerifyCastDeviceCertTest, NameConstraintsViolated) {
  std::string data_path = GetSpecificTestDataPath();
  RunTest(Error::Code::kErrCertsVerifyGeneric, "Device",
          CastDeviceCertPolicy::kUnrestricted,
          data_path + "certificates/nc_fail.pem", AprilFirst2020(),
          TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests reversibility between DateTimeToSeconds and DateTimeFromSeconds
TEST(VerifyCastDeviceCertTest, TimeDateConversionValidate) {
  DateTime org_date = AprilFirst2020();
  DateTime converted_date = {};
  std::chrono::seconds seconds = DateTimeToSeconds(org_date);
  DateTimeFromSeconds(seconds.count(), &converted_date);

  EXPECT_EQ(org_date.second, converted_date.second);
  EXPECT_EQ(org_date.minute, converted_date.minute);
  EXPECT_EQ(org_date.hour, converted_date.hour);
  EXPECT_EQ(org_date.day, converted_date.day);
  EXPECT_EQ(org_date.month, converted_date.month);
  EXPECT_EQ(org_date.year, converted_date.year);
}

}  // namespace
}  // namespace cast
}  // namespace openscreen
