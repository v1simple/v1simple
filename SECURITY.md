# Security

Report a suspected vulnerability privately. Use GitHub private vulnerability
reporting if this repository offers it. Otherwise, open a public issue asking
for private contact without including exploit details or sensitive data.

Include the firmware version from `include/config.h`, board, impact, and the
smallest safe reproduction. Do not attach credentials, network names, location
data, device dumps, or full logs until a private channel is established.

The repository declares no support window or response-time promise.

## Current device security model

The maintenance web/API interface is available only after physical entry into
maintenance boot and only through the device's WPA-protected access point;
requests arriving through the station/LAN interface are rejected. Read
endpoints rely on access-point membership and have no additional application
authentication. Mutating requests also require maintenance boot and the fixed
`X-V1Simple-Request` request-shape header, which is not authentication. Change
the published default access-point password during first setup. A maintenance
session expires after 10 minutes without UI activity and after 30 minutes at
most.

HTTP-downloaded backups omit access-point and saved-network passwords. Passwords
stored in NVS or local SD recovery data are reversibly obfuscated, not encrypted;
someone with physical access to those files or readable flash can recover them.
The shipped build does not enable Secure Boot, Flash Encryption, or NVS
encryption, so the device must not be treated as secure credential storage.
