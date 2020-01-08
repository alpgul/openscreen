// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/common/certificate/cast_cert_validator.h"

#include <stdio.h>
#include <string.h>

#include "cast/common/certificate/cast_cert_validator_internal.h"
#include "cast/common/certificate/testing/test_helpers.h"
#include "gtest/gtest.h"
#include "openssl/pem.h"

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
// RunTest() also checks that the resulting CertVerificationContext does not
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
  std::vector<std::string> certs =
      testing::ReadCertificatesFromPemFile(certs_file_name);
  TrustStore* trust_store;
  std::unique_ptr<TrustStore> fake_trust_store;

  switch (trust_store_dependency) {
    case TRUST_STORE_BUILTIN:
      trust_store = nullptr;
      break;

    case TRUST_STORE_FROM_TEST_FILE: {
      ASSERT_FALSE(certs.empty());

      // Parse the root certificate of the chain.
      const uint8_t* data = (const uint8_t*)certs.back().data();
      X509* fake_root = d2i_X509(nullptr, &data, certs.back().size());
      ASSERT_TRUE(fake_root);
      certs.pop_back();

      // Add a trust anchor and enforce constraints on it (regular mode for
      // built-in Cast roots).
      fake_trust_store = std::make_unique<TrustStore>();
      fake_trust_store->certs.emplace_back(fake_root);
      trust_store = fake_trust_store.get();
    }
  }

  std::unique_ptr<CertVerificationContext> context;
  CastDeviceCertPolicy policy;

  Error result = VerifyDeviceCert(certs, time, &context, &policy, nullptr,
                                  CRLPolicy::kCrlOptional, trust_store);

  ASSERT_EQ(expected_result, result.code());
  if (expected_result != Error::Code::kNone)
    return;

  EXPECT_EQ(expected_policy, policy);
  ASSERT_TRUE(context);

  // Test that the context is good.
  EXPECT_EQ(expected_common_name, context->GetCommonName());

#define DATA_SPAN_FROM_LITERAL(s) ConstDataSpan{(uint8_t*)s, sizeof(s) - 1}
  // Test verification of some invalid signatures.
  EXPECT_FALSE(context->VerifySignatureOverData(
      DATA_SPAN_FROM_LITERAL("bogus signature"),
      DATA_SPAN_FROM_LITERAL("bogus data"), DigestAlgorithm::kSha256));
  EXPECT_FALSE(context->VerifySignatureOverData(
      DATA_SPAN_FROM_LITERAL(""), DATA_SPAN_FROM_LITERAL("bogus data"),
      DigestAlgorithm::kSha256));
  EXPECT_FALSE(context->VerifySignatureOverData(DATA_SPAN_FROM_LITERAL(""),
                                                DATA_SPAN_FROM_LITERAL(""),
                                                DigestAlgorithm::kSha256));

  // If valid signatures are known for this device certificate, test them.
  if (!optional_signed_data_file_name.empty()) {
    testing::SignatureTestData signatures =
        testing::ReadSignatureTestData(optional_signed_data_file_name);

    // Test verification of a valid SHA1 signature.
    EXPECT_TRUE(context->VerifySignatureOverData(
        signatures.sha1, signatures.message, DigestAlgorithm::kSha1));

    // Test verification of a valid SHA256 signature.
    EXPECT_TRUE(context->VerifySignatureOverData(
        signatures.sha256, signatures.message, DigestAlgorithm::kSha256));
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

#define TEST_DATA_PREFIX OPENSCREEN_TEST_DATA_DIR "/cast/common/certificate/"

// Tests verifying a valid certificate chain of length 2:
//
//   0: 2ZZBG9 FA8FCA3EF91A
//   1: Eureka Gen1 ICA
//
// Chains to trust anchor:
//   Eureka Root CA    (built-in trust store)
TEST(VerifyCastDeviceCertTest, ChromecastGen1) {
  RunTest(Error::Code::kNone, "2ZZBG9 FA8FCA3EF91A",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/chromecast_gen1.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN,
          TEST_DATA_PREFIX "signeddata/2ZZBG9_FA8FCA3EF91A.pem");
}

// Tests verifying a valid certificate chain of length 2:
//
//  0: 2ZZBG9 FA8FCA3EF91A
//  1: Eureka Gen1 ICA
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
TEST(VerifyCastDeviceCertTest, ChromecastGen1Reissue) {
  RunTest(Error::Code::kNone, "2ZZBG9 FA8FCA3EF91A",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/chromecast_gen1_reissue.pem",
          AprilFirst2016(), TRUST_STORE_BUILTIN,
          TEST_DATA_PREFIX "signeddata/2ZZBG9_FA8FCA3EF91A.pem");
}

// Tests verifying a valid certificate chain of length 2:
//
//   0: 3ZZAK6 FA8FCA3F0D35
//   1: Chromecast ICA 3
//
// Chains to trust anchor:
//   Cast Root CA     (built-in trust store)
TEST(VerifyCastDeviceCertTest, ChromecastGen2) {
  RunTest(Error::Code::kNone, "3ZZAK6 FA8FCA3F0D35",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/chromecast_gen2.pem", AprilFirst2016(),
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
  RunTest(Error::Code::kNone, "-6394818897508095075",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/fugu.pem", AprilFirst2016(),
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
  RunTest(Error::Code::kErrCertsVerifyGeneric, "",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/unchained.pem", AprilFirst2016(),
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
  RunTest(Error::Code::kErrCertsRestrictions, "",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/cast_root_ca.pem", AprilFirst2016(),
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
  RunTest(Error::Code::kNone, "4ZZDZJ FA8FCA7EFE3C",
          CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX "certificates/chromecast_audio.pem",
          AprilFirst2016(), TRUST_STORE_BUILTIN, "");
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
  RunTest(Error::Code::kNone, "MediaTek Audio Dev Test",
          CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX "certificates/mtk_audio_dev.pem", JanuaryFirst2015(),
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
  RunTest(Error::Code::kNone, "9V0000VB FA8FCA784D01",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/vizio.pem", AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");
}

// Tests verifying a valid certificate chain of length 2 using expired
// time points.
TEST(VerifyCastDeviceCertTest, ChromecastGen2InvalidTime) {
  const char* kCertsFile = TEST_DATA_PREFIX "certificates/chromecast_gen2.pem";

  // Control test - certificate should be valid at some time otherwise
  // this test is pointless.
  RunTest(Error::Code::kNone, "3ZZAK6 FA8FCA3F0D35",
          CastDeviceCertPolicy::kUnrestricted, kCertsFile, AprilFirst2016(),
          TRUST_STORE_BUILTIN, "");

  // Use a time before notBefore.
  RunTest(Error::Code::kErrCertsDateInvalid, "",
          CastDeviceCertPolicy::kUnrestricted, kCertsFile, JanuaryFirst2015(),
          TRUST_STORE_BUILTIN, "");

  // Use a time after notAfter.
  RunTest(Error::Code::kErrCertsDateInvalid, "",
          CastDeviceCertPolicy::kUnrestricted, kCertsFile, MarchFirst2037(),
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
  RunTest(Error::Code::kNone, "Audio Reference Dev Test",
          CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX "certificates/audio_ref_dev_test_chain_3.pem",
          AprilFirst2016(), TRUST_STORE_BUILTIN,
          TEST_DATA_PREFIX "signeddata/AudioReferenceDevTest.pem");
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
  // The root certificate is only valid in 2015, so validating with a time in
  // 2016 means it is expired.
  RunTest(Error::Code::kNone, "CastDevice", CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/expired_root.pem", AprilFirst2016(),
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
  // Test that the chain verification fails due to the pathlen constraint.
  RunTest(Error::Code::kErrCertsPathlen, "Target",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/violates_root_pathlen_constraint.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain with the policies:
//
//  Root:           policies={}
//  Intermediate:   policies={anyPolicy}
//  Leaf:           policies={anyPolicy}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAnypolicyLeafAnypolicy) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX
          "certificates/policies_ica_anypolicy_leaf_anypolicy.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={anyPolicy}
//   Leaf:           policies={audioOnly}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAnypolicyLeafAudioonly) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX
          "certificates/policies_ica_anypolicy_leaf_audioonly.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={anyPolicy}
//   Leaf:           policies={foo}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAnypolicyLeafFoo) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/policies_ica_anypolicy_leaf_foo.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={anyPolicy}
//   Leaf:           policies={}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAnypolicyLeafNone) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/policies_ica_anypolicy_leaf_none.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={audioOnly}
//   Leaf:           policies={anyPolicy}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAudioonlyLeafAnypolicy) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX
          "certificates/policies_ica_audioonly_leaf_anypolicy.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={audioOnly}
//   Leaf:           policies={audioOnly}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAudioonlyLeafAudioonly) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX
          "certificates/policies_ica_audioonly_leaf_audioonly.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={audioOnly}
//   Leaf:           policies={foo}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAudioonlyLeafFoo) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX "certificates/policies_ica_audioonly_leaf_foo.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={audioOnly}
//   Leaf:           policies={}
TEST(VerifyCastDeviceCertTest, PoliciesIcaAudioonlyLeafNone) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX "certificates/policies_ica_audioonly_leaf_none.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={}
//   Leaf:           policies={anyPolicy}
TEST(VerifyCastDeviceCertTest, PoliciesIcaNoneLeafAnypolicy) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/policies_ica_none_leaf_anypolicy.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={}
//   Leaf:           policies={audioOnly}
TEST(VerifyCastDeviceCertTest, PoliciesIcaNoneLeafAudioonly) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kAudioOnly,
          TEST_DATA_PREFIX "certificates/policies_ica_none_leaf_audioonly.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={}
//   Leaf:           policies={foo}
TEST(VerifyCastDeviceCertTest, PoliciesIcaNoneLeafFoo) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/policies_ica_none_leaf_foo.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Test verifying a certificate chain with the policies:
//
//   Root:           policies={}
//   Intermediate:   policies={}
//   Leaf:           policies={}
TEST(VerifyCastDeviceCertTest, PoliciesIcaNoneLeafNone) {
  RunTest(Error::Code::kNone, "Leaf", CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/policies_ica_none_leaf_none.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain where the leaf certificate has a
// 1024-bit RSA key. Verification should fail since the target's key is
// too weak.
TEST(VerifyCastDeviceCertTest, DeviceCertHas1024BitRsaKey) {
  RunTest(Error::Code::kErrCertsVerifyGeneric, "RSA 1024 Device Cert",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/rsa1024_device_cert.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain where the leaf certificate has a
// 2048-bit RSA key, and then verifying signed data (both SHA1 and SHA256)
// for it.
TEST(VerifyCastDeviceCertTest, DeviceCertHas2048BitRsaKey) {
  RunTest(Error::Code::kNone, "RSA 2048 Device Cert",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/rsa2048_device_cert.pem",
          AprilFirst2016(), TRUST_STORE_FROM_TEST_FILE,
          TEST_DATA_PREFIX "signeddata/rsa2048_device_cert_data.pem");
}

// Tests verifying a certificate chain where an intermediate certificate has a
// nameConstraints extension but the leaf certificate is still permitted under
// these constraints.
TEST(VerifyCastDeviceCertTest, NameConstraintsObeyed) {
  RunTest(Error::Code::kNone, "Device", CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/nc.pem", AprilFirst2020(),
          TRUST_STORE_FROM_TEST_FILE, "");
}

// Tests verifying a certificate chain where an intermediate certificate has a
// nameConstraints extension and the leaf certificate is not permitted under
// these constraints.
TEST(VerifyCastDeviceCertTest, NameConstraintsViolated) {
  RunTest(Error::Code::kErrCertsVerifyGeneric, "Device",
          CastDeviceCertPolicy::kUnrestricted,
          TEST_DATA_PREFIX "certificates/nc_fail.pem", AprilFirst2020(),
          TRUST_STORE_FROM_TEST_FILE, "");
}

}  // namespace
}  // namespace cast
}  // namespace openscreen
