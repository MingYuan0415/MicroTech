# Device Link Contract Status

`contracts/device_link` is the authoritative source for the `device-link/v1`
freeze candidate. Its `protocol.yaml` defines the BLE GATT, security, MTU,
fixed-binary Wi-Fi messages, fixed unknown-opcode response, observable Wi-Fi
results, and operation recovery behavior. Golden vectors and the checker are
contract conformance data, not firmware acceptance evidence.

The Wi-Fi wire format and minimum observable command results remain part of the
BLE contract. Retry, automatic connection, persistence implementation, scan
selection, rollback, and internal failure recovery are not normative there.
Their current pending target is documented separately in
`layers/middleware/components/wifi_service/README.md`. The current
`wifi_service` and `connectivity_manager` implementations are not declared
conformant with this candidate.

Operation recovery requires ATT MTU 498. A retained record is queried with
`GET_OPERATION`, the current Wi-Fi snapshot is queried separately with
`GET_STATUS`, and a successful `ACK_OPERATION` removes the record only after its
response indication is confirmed.

The root `contracts/device_link.lock` records the checked-out candidate base and
the normalized worktree contract digest. Firmware and mobile-client conformance
are not claimed. Numeric Comparison, transactional bond replacement and
reconnect, ATT MTU 498, DLE, 495/496-byte boundaries, disconnect recovery, and
on-air interoperability remain unverified on hardware.
