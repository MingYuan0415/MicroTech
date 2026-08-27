# Device Link Contract Status

`contracts/device_link` is the authoritative source for the `device-link/v1`
freeze candidate. Its `protocol.yaml` defines the BLE GATT, security, MTU,
fixed-binary Wi-Fi messages, fixed unknown-opcode response, observable Wi-Fi
results, and operation slot behavior. Golden vectors and the checker are
contract conformance data, not firmware acceptance evidence.

Firmware now speaks this profile: one service, `command_rx` / `server_tx`,
LE Secure Connections Numeric Comparison, ATT MTU 498, a single operation
slot, and ACK-cleared terminal records. The device does not enforce a
reconnect command sequence. After reconnect the bonded client should
negotiate MTU 498, then read `GET_OPERATION` and `GET_STATUS` and ACK a
terminal record if one exists.

The Wi-Fi wire format and minimum observable command results remain part of
the BLE contract. Retry, automatic connection, scan selection, rollback, and
internal failure recovery are not normative there. Their pending target is
documented in `layers/middleware/components/wifi_service/README.md`.
`SET_CREDENTIALS` now persists without starting a connection;
`wifi_service` is still not declared fully policy-conformant. Firmware
always installs the v1 Wi-Fi owner ops. Bond replacement while the pairing
window is open deletes the conflicting bond and returns
`BLE_GAP_REPEAT_PAIRING_RETRY`.

The root `contracts/device_link.lock` records the checked-out candidate and
the normalized worktree contract digest. Numeric Comparison, local bond
clear and reconnect, ATT MTU 498, DLE, 495/496-byte boundaries, disconnect
query recovery, finite operation timeouts, and on-air interoperability have
not yet been validated on hardware and a mobile client.
