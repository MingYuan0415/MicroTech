# Device Link Typed-TLV v2

This document describes the firmware implementation boundary. The authoritative
wire contract is the `contracts/provisioning` submodule during the local
repository rename transition. Provisioning v1,
Device Link v1, their UUIDs, QR identifiers, application protobuf schemas and
generated consumers are retired; the firmware does not run a compatibility
stack or downgrade an unknown request.

## Application wire

`layers/middleware/components/device_link` owns the handwritten Core v2 wire:

- a fixed 16-byte application header followed by bounded BLE fragments;
- strict Typed-TLV fields with explicit field IDs, wire types and size limits;
- rejection of truncation, duplicate singular fields, non-minimal integers,
  invalid ordering and malformed known fields;
- forward-compatible skipping of unknown fields only when their wire type is
  structurally safe;
- startup-frozen method descriptors. A descriptor binds domain/version,
  request and response schemas, permission, limits and an owner handler.

Core uses domain ID `0`. Wi-Fi, Cloud and Location reserve IDs `1`, `2` and
`3`, but their adapters are not registered and their capabilities are not
published until their persistence and recovery contracts are complete. The
manifest therefore advertises Core only. There is no runtime registration,
reflection, `google.protobuf.Any`, application protobuf schema or generated C.

The only protobuf-c types retained in middleware are the official ESP-IDF
Protocomm Security 2 `SessionData`/`Sec2Payload` types. They are an internal
cryptographic transport dependency, not the Device Link application protocol.

## BLE identity and ownership

`ble_runtime` owns NimBLE host lifecycle, static GATT, advertising, connection
facts and transport scheduling. `device_link_service` is the serialized owner
of ACL/session state, Security 2 epochs, authorization transactions, snapshots
and retained cleanup. Cross-thread operations carry immutable generation,
epoch, flow, token, kind and connection-handle identity. A stale generation or
reused handle is a no-op.

The v2 service and characteristics use the UUIDs in the contract profile. The
advertisement carries the v2 service UUID and version `2`; no v1 UUID or QR
identifier is emitted. Security 2 remains SC-only with one administrator and
the existing local-confirmation and recovery semantics.

The owner uses task notifications for lossless wakeups and absolute deadlines
for expiry/retry. TX credits, operation slots and retained cleanup slots are
fixed at initialization. Sensitive request, credential and password buffers are
zeroized on completion, error and disconnect. Replay keys contain boot ID,
connection generation, security epoch, domain, method, call ID, request length
and digest; only the response is retained.

## Wi-Fi silent synchronization

The `connectivity_manager` remains the only Wi-Fi state-machine owner. Its
`connectivity_manager_request_sync_profile()` API accepts a bounded credential
record, a non-zero `client_sync_id`, an auto-connect flag and an operation ID.

- Same sync ID and same credentials are idempotent; same ID with different
  credentials returns `CONFLICT`; a new ID creates a new operation.
- Existing permission `wifi.write` is checked at the Device Link boundary. Once
  granted, profile writes, auto-connect, reconnect, disconnect, forget, scan
  and operation recovery do not request another local confirmation.
- A new profile replaces the durable profile only after IPv4 acquisition and
  persistent storage both succeed. On storage failure or ambiguity the old
  durable profile remains active; the new credentials may exist only as a
  bounded RAM candidate and recovery returns `STORAGE` or `INTERNAL`.
- Passwords are never returned. Status exposes only SSID, connection state,
  profile revision and whether the sync ID was applied.

The contract registry intentionally marks Wi-Fi as not advertised until a
Device Link adapter, recovery behavior and storage-failure tests are complete.
Cloud and Location remain schema-only for the same reason. Cloud credentials
require a separate encrypted vault and HMAC/eFuse-derived key before any
capability can be published. Location requires permission, TTL,
quantized-persistence and revoke cleanup before publication.

## Security and reset boundaries

Security 2 Cmd0 creates one epoch; Cmd1 authenticates that same epoch. A
replacement Cmd0 retires the old session immediately, retaining only one exact
pending request while an indication is outstanding. Commit follows
`PREPARED -> COMMIT_PROBED -> LOCALLY_CONFIRMED -> COMMITTED`, with a non-zero
boot-scoped confirmation token. Commit, replacement, revoke, Recovery Query and
factory reset are serialized by the service owner.

Factory reset starts with a durable marker, pauses BLE and network startup,
clears the Wi-Fi profile and the complete Device Link store, drains ACL and
cleanup obligations, and clears the marker last. Any failure leaves the marker
and gates closed for idempotent retry after reboot.

## Validation boundary

Automated evidence includes Typed-TLV/router/operation tests, BLE runtime,
Security 2 and service host suites, connectivity sync tests, cross-layer tests,
normal/address/thread sanitizers, AStyle, `git diff --check` and the ESP-IDF
reconfigure/build/size checks. Generated application protobuf checks are
intentionally absent. `compatibility/verified.yaml` remains empty until
ESP32-S3/Android validation covers v2 UUID/QR discovery, binding and recovery,
Wi-Fi write/reboot/storage-failure behavior, Security 2, MTU boundaries,
factory reset and BLE soak. Automated tests do not claim that hardware
interoperability or power-loss behavior has passed.
