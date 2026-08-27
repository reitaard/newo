#pragma once

// Copy this file to newo_secrets.h on the local development machine.
// Never commit the real newo_secrets.h file.
//
// DEVICE_SECRET must match NEWO_DEVICE_SECRET in the VPS server/.env.
// CLOUD_CA_CERT is public trust material, not a private key. Use the root CA
// certificate(s) that validate the public TLS endpoint seen by the ESP32.

namespace NewoSecrets {

constexpr char DEVICE_ID[] = "newo-01";
constexpr char DEVICE_SECRET[] = "replace-with-the-secret-from-the-vps-env";

constexpr char CLOUD_CA_CERT[] = R"PEM(
-----BEGIN CERTIFICATE-----
replace-with-trusted-root-ca-pem
-----END CERTIFICATE-----
)PEM";

}  // namespace NewoSecrets
