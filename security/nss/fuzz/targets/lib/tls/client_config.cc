/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "client_config.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "nss_scoped_ptrs.h"
#include "nssb64.h"
#include "prio.h"
#include "prtypes.h"
#include "seccomon.h"
#include "ssl.h"
#include "sslexp.h"

#include "common.h"

const SSLCertificateCompressionAlgorithm kCompressionAlg = {
    0x1337, "fuzz", TlsCommon::DummyCompressionEncode,
    TlsCommon::DummyCompressionDecode};
const PRUint8 kPskIdentity[] = "fuzz-psk-identity";
#ifndef IS_DTLS_FUZZ
const char kEchConfigs[] =
    "AEX+"
    "DQBBcQAgACDh4IuiuhhInUcKZx5uYcehlG9PQ1ZlzhvVZyjJl7dscQAEAAEAAQASY2xvdWRmbG"
    "FyZS1lY2guY29tAAA=";
#endif  // IS_DTLS_FUZZ

static SECStatus AuthCertificateHook(void* arg, PRFileDesc* fd, PRBool checksig,
                                     PRBool isServer) {
  assert(!isServer);

  auto config = reinterpret_cast<TlsClient::Config*>(arg);
  if (config->FailCertificateAuthentication()) return SECFailure;

  return SECSuccess;
}

static SECStatus CanFalseStartCallback(PRFileDesc* fd, void* arg,
                                       PRBool* canFalseStart) {
  *canFalseStart = true;
  return SECSuccess;
}

namespace TlsClient {

// XOR 64-bit chunks of data to build a bitmap of config options derived from
// the fuzzing input. This seems the only way to fuzz various options while
// still maintaining compatibility with BoringSSL or OpenSSL fuzzers.
Config::Config(const uint8_t* data, size_t len) {
  union {
    uint64_t bitmap;
    struct {
      uint32_t config;
      uint16_t ssl_version_range_min;
      uint16_t ssl_version_range_max;
    };
  };

  for (size_t i = 0; i < len; i++) {
    bitmap ^= static_cast<uint64_t>(data[i]) << (8 * (i % 8));
  }

  // Map SSL version values to a valid range.
  ssl_version_range_min =
      SSL_VERSION_RANGE_MIN_VALID +
      (ssl_version_range_min %
       (1 + SSL_VERSION_RANGE_MAX_VALID - SSL_VERSION_RANGE_MIN_VALID));
  ssl_version_range_max =
      ssl_version_range_min +
      (ssl_version_range_max %
       (1 + SSL_VERSION_RANGE_MAX_VALID - ssl_version_range_min));

  config_ = config;
  ssl_version_range_ = {
      .min = ssl_version_range_min,
      .max = ssl_version_range_max,
  };
}

void Config::SetCallbacks(PRFileDesc* fd) {
  SECStatus rv = SSL_AuthCertificateHook(fd, AuthCertificateHook, this);
  assert(rv == SECSuccess);

  rv = SSL_SetCanFalseStartCallback(fd, CanFalseStartCallback, nullptr);
  assert(rv == SECSuccess);
}

void Config::SetSocketOptions(PRFileDesc* fd) {
  SECStatus rv = SSL_OptionSet(fd, SSL_ENABLE_EXTENDED_MASTER_SECRET,
                               this->EnableExtendedMasterSecret());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_REQUIRE_DH_NAMED_GROUPS,
                     this->RequireDhNamedGroups());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_FALSE_START, this->EnableFalseStart());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_DEFLATE, this->EnableDeflate());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_CBC_RANDOM_IV, this->CbcRandomIv());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_REQUIRE_SAFE_NEGOTIATION,
                     this->RequireSafeNegotiation());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_NO_CACHE, this->NoCache());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_GREASE, this->EnableGrease());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_CH_EXTENSION_PERMUTATION,
                     this->EnableCHExtensionPermutation());
  assert(rv == SECSuccess);

  if (this->SetCertificateCompressionAlgorithm()) {
    rv = SSL_SetCertificateCompressionAlgorithm(fd, kCompressionAlg);
    assert(rv == SECSuccess);
  }

  if (this->SetVersionRange()) {
    rv = SSL_VersionRangeSet(fd, &ssl_version_range_);
    assert(rv == SECSuccess);
  }

  if (this->AddExternalPsk()) {
    ScopedPK11SlotInfo slot(PK11_GetInternalSlot());
    assert(slot);

    ScopedPK11SymKey key(PK11_KeyGen(slot.get(), CKM_NSS_CHACHA20_POLY1305,
                                     nullptr, 32, nullptr));
    assert(key);

    rv = SSL_AddExternalPsk(fd, key.get(), kPskIdentity,
                            sizeof(kPskIdentity) - 1, this->PskHashType());
    assert(rv == SECSuccess);
  }

  rv = SSL_OptionSet(fd, SSL_ENABLE_POST_HANDSHAKE_AUTH,
                     this->EnablePostHandshakeAuth());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_0RTT_DATA, this->EnableZeroRtt());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_ALPN, this->EnableAlpn());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_FALLBACK_SCSV, this->EnableFallbackScsv());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_OCSP_STAPLING, this->EnableOcspStapling());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_SESSION_TICKETS,
                     this->EnableSessionTickets());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_TLS13_COMPAT_MODE,
                     this->EnableTls13CompatMode());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_NO_LOCKS, this->NoLocks());
  assert(rv == SECSuccess);

  rv = SSL_EnableTls13GreaseEch(fd, this->EnableTls13GreaseEch());
  assert(rv == SECSuccess);

  rv = SSL_SetDtls13VersionWorkaround(fd, this->SetDtls13VersionWorkaround());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_DELEGATED_CREDENTIALS,
                     this->EnableDelegatedCredentials());
  assert(rv == SECSuccess);

  rv = SSL_OptionSet(fd, SSL_ENABLE_DTLS_SHORT_HEADER,
                     this->EnableDtlsShortHeader());
  assert(rv == SECSuccess);

#ifndef IS_DTLS_FUZZ
  rv =
      SSL_OptionSet(fd, SSL_ENABLE_RENEGOTIATION, SSL_RENEGOTIATE_UNRESTRICTED);
  assert(rv == SECSuccess);

  if (this->SetClientEchConfigs()) {
    ScopedSECItem echConfigsBin(NSSBase64_DecodeBuffer(
        nullptr, nullptr, kEchConfigs, sizeof(kEchConfigs)));
    assert(echConfigsBin);

    rv = SSL_SetClientEchConfigs(fd, echConfigsBin->data, echConfigsBin->len);
    assert(rv == SECSuccess);
  }
#endif  // IS_DTLS_FUZZ
}

std::ostream& operator<<(std::ostream& out, Config& config) {
  out << "============= ClientConfig ============="
      << "\n";
  out << "SSL_NO_CACHE:                           " << config.NoCache() << "\n";
  out << "SSL_ENABLE_EXTENDED_MASTER_SECRET:      "
      << config.EnableExtendedMasterSecret() << "\n";
  out << "SSL_REQUIRE_DH_NAMED_GROUPS:            "
      << config.RequireDhNamedGroups() << "\n";
  out << "SSL_ENABLE_FALSE_START:                 " << config.EnableFalseStart()
      << "\n";
  out << "SSL_ENABLE_DEFLATE:                     " << config.EnableDeflate()
      << "\n";
  out << "SSL_CBC_RANDOM_IV:                      " << config.CbcRandomIv()
      << "\n";
  out << "SSL_REQUIRE_SAFE_NEGOTIATION:           "
      << config.RequireSafeNegotiation() << "\n";
  out << "SSL_ENABLE_GREASE:                      " << config.EnableGrease()
      << "\n";
  out << "SSL_ENABLE_CH_EXTENSION_PERMUTATION:    "
      << config.EnableCHExtensionPermutation() << "\n";
  out << "SSL_SetCertificateCompressionAlgorithm: "
      << config.SetCertificateCompressionAlgorithm() << "\n";
  out << "SSL_VersionRangeSet:                    " << config.SetVersionRange()
      << "\n";
  out << "  Min:                                  "
      << config.SslVersionRange().min << "\n";
  out << "  Max:                                  "
      << config.SslVersionRange().max << "\n";
  out << "SSL_AddExternalPsk:                     " << config.AddExternalPsk()
      << "\n";
  out << "  Type:                                 " << config.PskHashType()
      << "\n";
  out << "SSL_ENABLE_POST_HANDSHAKE_AUTH:         "
      << config.EnablePostHandshakeAuth() << "\n";
  out << "SSL_ENABLE_0RTT_DATA:                   " << config.EnableZeroRtt()
      << "\n";
  out << "SSL_ENABLE_ALPN:                        " << config.EnableAlpn()
      << "\n";
  out << "SSL_ENABLE_FALLBACK_SCSV:               "
      << config.EnableFallbackScsv() << "\n";
  out << "SSL_ENABLE_OCSP_STAPLING:               "
      << config.EnableOcspStapling() << "\n";
  out << "SSL_ENABLE_SESSION_TICKETS:             "
      << config.EnableSessionTickets() << "\n";
  out << "SSL_ENABLE_TLS13_COMPAT_MODE:           "
      << config.EnableTls13CompatMode() << "\n";
  out << "SSL_NO_LOCKS:                           " << config.NoLocks() << "\n";
  out << "SSL_EnableTls13GreaseEch:               "
      << config.EnableTls13GreaseEch() << "\n";
  out << "SSL_SetDtls13VersionWorkaround:         "
      << config.SetDtls13VersionWorkaround() << "\n";
  out << "SSL_SetClientEchConfigs:                "
      << config.SetClientEchConfigs() << "\n";
  out << "========================================";

  return out;
}

}  // namespace TlsClient
