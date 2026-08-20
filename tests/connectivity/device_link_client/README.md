# Legacy Device Link Client

`link_client.py` targets the removed Device Link v2/provisioning profile. It is
not a `device-link/v1` conformance client and must not be used as evidence for
the current contract.

The current BLE profile is defined by
`contracts/device_link/protocol.yaml`. A conforming client must support
LE Secure Connections Numeric Comparison, ATT MTU 498, the fixed-binary v1
messages, boot-scoped operation IDs, and `GET_OPERATION`/`GET_STATUS` followed
by confirmed `ACK_OPERATION` recovery. No such client or hardware-interoperation
result is present in this directory today.
