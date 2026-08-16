#!/usr/bin/env python3
"""Device Link v2 hardware-matrix reference client (Bleak).

A host-side client for the on-device matrix in
doc/device-link-implementation.md. It implements the fixed wire (fragment
framing, application header, status), the Security 2 SRP-6a/AES-256-GCM
session, and the Core/Wi-Fi method procedures, all derived from the ESP-IDF
v6.0.2 protocomm Security 2 adapter (security2.c / esp_srp.c) and the
canonical contract tooling.

Validation status: the wire/framing/QR/link-state pieces reuse the
contract's reference codec (fixture-verified). The SRP/GCM module mirrors
the ESP-IDF adapter source but has NOT been exercised against hardware
yet: it is validated on-device together with matrix items 2-4.

Usage:
  python3 link_client.py scan
  python3 link_client.py matrix --item 1 --device AA:BB:CC:DD:EE:FF \
      [--pop HEX16 | --public-password] [--soak-seconds N] [--auto-confirm]
"""

import argparse
import asyncio
import hashlib
import json
import secrets
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

_REPO_ROOT = Path(__file__).resolve().parents[3]
_CONTRACT = _REPO_ROOT / "contracts" / "provisioning"
sys.path.insert(0, str(_CONTRACT))

# pylint: disable=wrong-import-position
from tooling.contractcheck.codec import decode_message, encode_message  # noqa: E402
from tooling.contractcheck.schema import load_contract  # noqa: E402
from tooling.contractcheck.wire import (  # noqa: E402
    APPLICATION_HEADER_BYTES, FRAGMENT_END, FRAGMENT_HEADER_BYTES,
    FRAGMENT_START, KIND_REQUEST, MODE_BINDABLE, RESPONSE_STATUS_BYTES,
    SRP_USERNAME, V2_SERVICE_UUID, ApplicationHeader, decode_fragment_header,
    decode_header, decode_link_state, decode_qr_payload, decode_service_data,
    decode_status, derive_public_srp_password, encode_fragment_header,
    encode_header, encode_status)

try:
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "bleak is required: pip install bleak (or use the contract venv)"
    ) from exc

LINK_STATE_UUID = "f91f51f0-b202-4f98-9114-a2003688cc35"
SESSION_RX_UUID = "1bbfaedb-60f5-48fe-8319-ee48508febf4"
SESSION_TX_UUID = "2cc65d38-571b-4f5a-83a2-c1a558693dec"
CONTROL_RX_UUID = "29dfcc56-cd1d-4113-85db-5e26a3b46748"
CONTROL_TX_UUID = "7c6ec06a-4f47-47a8-be90-b86431c84b79"

TRANSPORT_HANDSHAKE = 0x00
TRANSPORT_PROTECTED = 0x01

# Status values (LinkError, schemas/core/v2.yaml).
STATUS = {
    "OK": 1, "MALFORMED_FRAME": 2, "UNSUPPORTED_VERSION": 3,
    "UNSUPPORTED_OPERATION": 4, "UNSUPPORTED_CAPABILITY": 5,
    "UNAUTHENTICATED": 6, "PERMISSION_DENIED": 7,
    "CONFIRMATION_REQUIRED": 8, "INVALID_ARGUMENT": 9, "BUSY": 10,
    "NOT_FOUND": 11, "RESOURCE_EXHAUSTED": 12, "CONFLICT": 13,
    "UNAVAILABLE": 14, "STORAGE": 15, "INTERNAL": 16,
}
STATUS_NAMES = {value: key for key, value in STATUS.items()}

ALLOWED_STATUSES = {
    (0, 1): [1], (0, 2): [1], (0, 3): [1, 9, 10, 12, 14],
    (0, 4): [1, 8, 9, 11, 15, 16, 14], (0, 5): [1, 11, 15, 16, 14],
    (0, 6): [1, 11, 14], (0, 7): [1, 11, 10, 14],
    (1, 1): [1, 14, 15, 16], (1, 2): [1, 12, 10, 14, 16],
    (1, 3): [1, 11, 14, 16],
    (1, 4): [1, 9, 13, 12, 15, 16, 14],
    (1, 5): [1, 10, 14, 16], (1, 6): [1, 11, 10, 14, 16],
    (1, 7): [1, 11, 15, 16, 14], (1, 8): [1, 9, 14, 16],
}


# --------------------------------------------------------------------------
# Security 2: SRP-6a (RFC 5054, 3072-bit, SHA-512) + AES-256-GCM.
# Derived from components/protocomm/src/crypto/srp6a/esp_srp.c and
# components/protocomm/src/security/security2.c (ESP-IDF v6.0.2).
# --------------------------------------------------------------------------

N_3072 = int.from_bytes(bytes.fromhex(
    "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd1"
    "29024e088a67cc74020bbea63b139b22514a08798e3404dd"
    "ef9519b3cd3a431b302b0a6df25f14374fe1356d6d51c245"
    "e485b576625e7ec6f44c42e9a637ed6b0bff5cb6f406b7ed"
    "ee386bfb5a899fa5ae9f24117c4b1fe649286651ece45b3d"
    "c2007cb8a163bf0598da48361c55d39a69163fa8fd24cf5f"
    "83655d23dca3ad961c62f356208552bb9ed529077096966d"
    "670c354e4abc9804f1746c08ca18217c32905e462e36ce3b"
    "e39e772c180e86039b2783a2ec07a28fb5c55df06f4c52c9"
    "de2bcbf6955817183995497cea956ae515d2261898fa0510"
    "15728e5a8aaac42dad33170d04507a33a85521abdf1cba64"
    "ecfb850458dbef0a8aea71575d060c7db3970f85a6e1e4c7"
    "abf5ae8cdb0933d71e8c94e04a25619dcee3d2261ad2ee6b"
    "f12ffa06d98a0864d87602733ec86a64521f2b18177b200c"
    "bbe117577a615d6c770988c0bad946e208e24fa074e5ab31"
    "43db5bfce0fd108e4b82d120a93ad2caffffffffffffffff"), "big")
G = 5
N_BYTES = 384
SHA512 = lambda data: hashlib.sha512(data).digest()


def _minimal_be(number: int) -> bytes:
    length = max(1, (number.bit_length() + 7) // 8)
    return number.to_bytes(length, "big")


def _padded(data: bytes, length: int) -> bytes:
    return data.rjust(length, b"\x00")


def _mpi(data: bytes) -> int:
    return int.from_bytes(data, "big")


class Sec2Session:
    """SRP-6a client session producing the AES-256-GCM protected channel."""

    def __init__(self, username: bytes, password: bytes):
        self.username = username
        self.password = password
        self.a = secrets.randbelow(N_3072 - 1) + 1
        self.public_a = pow(G, self.a, N_3072)
        self._key: bytes | None = None
        self._counter = 0

    def proof_material(self, salt: bytes, device_public: int) -> None:
        """Complete the client-side SRP computation after Resp0."""
        if device_public % N_3072 == 0:
            raise ValueError("device public key is invalid (B mod N == 0)")
        inner = SHA512(self.username + b":" + self.password)
        x = _mpi(SHA512(salt + inner))
        k = _mpi(SHA512(_padded(N_3072.to_bytes(N_BYTES, "big"), N_BYTES)
                        + _padded(b"\x05", N_BYTES)))
        u = _mpi(SHA512(_padded(self.public_a.to_bytes(N_BYTES, "big"),
                                N_BYTES)
                        + _padded(device_public.to_bytes(N_BYTES, "big"),
                                  N_BYTES)))
        base = (device_public - k * pow(G, x, N_3072)) % N_3072
        exponent = (self.a + u * x) % N_3072
        shared = pow(base, exponent, N_3072)
        self._key = SHA512(_minimal_be(shared))

    @property
    def aes_key(self) -> bytes:
        if self._key is None:
            raise RuntimeError("session not established")
        return self._key[:32]

    def client_proof(self, salt: bytes, device_public: int) -> bytes:
        hash_n_xor_g = bytes(
            a ^ b for a, b in zip(
                SHA512(N_3072.to_bytes(N_BYTES, "big")),
                SHA512(_padded(b"\x05", N_BYTES))))
        return SHA512(
            hash_n_xor_g + SHA512(self.username) + salt
            + _minimal_be(self.public_a) + _minimal_be(device_public)
            + self._key)

    def verify_device_proof(self, salt: bytes, device_public: int,
                            device_proof: bytes) -> bool:
        expected = SHA512(_minimal_be(self.public_a)
                          + self.client_proof(salt, device_public)
                          + self._key)
        return secrets.compare_digest(expected, device_proof)

    def seal(self, device_nonce: bytes) -> "Sec2Cipher":
        if len(device_nonce) != 12:
            raise ValueError("device nonce must be 12 bytes")
        return Sec2Cipher(self.aes_key, device_nonce)


class Sec2Cipher:
    """AES-256-GCM with the adapter's IV/counter convention.

    IV = device_nonce(12) with the trailing 4-byte big-endian counter,
    starting at 1 and incremented after every operation (the adapter uses
    one strictly ordered counter for both directions). The protected
    payload is ciphertext || 16-byte tag with no prefix.
    """

    def __init__(self, key: bytes, device_nonce: bytes):
        self._gcm = AESGCM(key)
        self._nonce_prefix = device_nonce[:8]
        self._counter = 1

    def _iv(self) -> bytes:
        return self._nonce_prefix + self._counter.to_bytes(4, "big")

    def _bump(self) -> None:
        self._counter += 1
        if self._counter > 0xffffffff:
            raise RuntimeError("session counter exhausted")

    def protect(self, plain: bytes) -> bytes:
        out = self._gcm.encrypt(self._iv(), plain, None)
        self._bump()
        return out

    def unprotect(self, blob: bytes) -> bytes:
        out = self._gcm.decrypt(self._iv(), blob, None)
        self._bump()
        return out


# --------------------------------------------------------------------------
# Security 2 protobuf messages (hand-rolled wire format, sec2.proto /
# session.proto field numbers).
# --------------------------------------------------------------------------


def _pb_varint(value: int) -> bytes:
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7f) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def _pb_bytes(field: int, data: bytes) -> bytes:
    return _pb_varint(field << 3 | 2) + _pb_varint(len(data)) + data


def _pb_varint_field(field: int, value: int) -> bytes:
    return _pb_varint(field << 3) + _pb_varint(value)


def _pb_read(data: bytes, offset: int):
    value = 0
    shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7


def _pb_fields(data: bytes):
    offset = 0
    fields = []
    while offset < len(data):
        key, offset = _pb_read(data, offset)
        field_number, wire = key >> 3, key & 7
        if wire == 0:
            value, offset = _pb_read(data, offset)
        elif wire == 2:
            length, offset = _pb_read(data, offset)
            value = data[offset:offset + length]
            offset += length
        else:
            raise ValueError(f"unsupported protobuf wire type {wire}")
        fields.append((field_number, value))
    return fields


def _pb_get(fields, number, kind):
    matches = [value for field, value in fields if field == number]
    if not matches or not isinstance(matches[0], kind):
        raise ValueError(f"missing/invalid protobuf field {number}")
    return matches[0]


def encode_cmd0(username: bytes, public_a: int) -> bytes:
    payload = (_pb_bytes(1, username)
               + _pb_bytes(2, _minimal_be(public_a)))
    sec2 = _pb_varint_field(1, 0) + _pb_bytes(20, payload)
    return _pb_varint_field(2, 2) + _pb_bytes(12, sec2)


def encode_cmd1(client_proof: bytes) -> bytes:
    sec2 = _pb_varint_field(1, 2) + _pb_bytes(22, _pb_bytes(1, client_proof))
    return _pb_varint_field(2, 2) + _pb_bytes(12, sec2)


def decode_resp0(data: bytes):
    session_fields = _pb_fields(data)
    sec2 = _pb_get(session_fields, 12, bytes)
    payload_fields = _pb_fields(sec2)
    msg = _pb_get(payload_fields, 1, int)
    if msg != 1:
        raise ValueError(f"unexpected Sec2 message type {msg}")
    resp0 = _pb_get(payload_fields, 21, bytes)
    fields = _pb_fields(resp0)
    status = _pb_get(fields, 1, int)
    salt = _pb_get(fields, 3, bytes)
    device_public = _mpi(_pb_get(fields, 2, bytes))
    return status, salt, device_public


def decode_resp1(data: bytes):
    session_fields = _pb_fields(data)
    sec2 = _pb_get(session_fields, 12, bytes)
    payload_fields = _pb_fields(sec2)
    if _pb_get(payload_fields, 1, int) != 3:
        raise ValueError("unexpected Sec2 message type")
    resp1 = _pb_get(payload_fields, 23, bytes)
    fields = _pb_fields(resp1)
    status = _pb_get(fields, 1, int)
    device_proof = _pb_get(fields, 2, bytes)
    device_nonce = _pb_get(fields, 3, bytes)
    return status, device_proof, device_nonce


# --------------------------------------------------------------------------
# BLE framing: send / reassemble per framing.md.
# --------------------------------------------------------------------------

IDLE_DEADLINE_SECONDS = 5.0


@dataclass
class _RxFrame:
    started: bool = False
    frame_id: int = 0
    total: int = 0
    received: int = 0
    buffer: bytearray = field(default_factory=bytearray)
    last: bytes | None = None
    tombstone_valid: bool = False
    last_seen: float = 0.0


class FrameRx:
    """Fragment reassembler mirroring docs/core/v2/framing.md."""

    def __init__(self, capacity: int):
        self.capacity = capacity
        self._state = _RxFrame()

    def feed(self, value: bytes) -> bytes | None:
        header = decode_fragment_header(value)
        payload = value[FRAGMENT_HEADER_BYTES:]
        now = time.monotonic()
        start = bool(header.flags & FRAGMENT_START)
        end = bool(header.flags & FRAGMENT_END)
        if (header.frame_id == 0 or header.total_length == 0
                or not payload
                or header.offset > header.total_length
                or len(payload) > header.total_length - header.offset):
            raise ValueError("malformed fragment")
        state = self._state
        if (not state.started and state.tombstone_valid
                and self._is_duplicate(state, header, payload)):
            return None  # tombstone: late duplicate, no re-delivery
        if not state.started:
            if not start or header.offset != 0:
                raise ValueError("fragment does not start a frame")
            if header.total_length > self.capacity:
                raise ValueError("frame exceeds capacity")
            state.started = True
            state.frame_id = header.frame_id
            state.total = header.total_length
            state.received = 0
            state.buffer = bytearray(header.total_length)
            state.tombstone_valid = False
        elif (header.frame_id != state.frame_id
              or header.total_length != state.total_length):
            raise ValueError("frame identity changed mid-frame")
        if state.started and header.offset < state.received:
            if self._is_duplicate(state, header, payload):
                return None
            raise ValueError("overlapping fragment")
        if header.offset > state.received or (state.received and start):
            raise ValueError("fragment gap or unexpected START")
        if header.offset == state.received:
            state.buffer[header.offset:header.offset + len(payload)] = payload
            state.received += len(payload)
        state.last = value
        state.last_seen = now
        if state.received == state.total_length:
            if not end:
                raise ValueError("frame reached total without END")
            message = bytes(state.buffer)
            state.started = False
            state.tombstone_valid = True
            return message
        if end:
            raise ValueError("END before total")
        return None

    def idle_expired(self) -> bool:
        state = self._state
        return state.started and \
            time.monotonic() - state.last_seen > IDLE_DEADLINE_SECONDS

    def reset(self) -> None:
        self._state = _RxFrame()

    @staticmethod
    def _is_duplicate(state, header, payload):
        return (header.frame_id == state.frame_id
                and header.total_length == state.total
                and state.last is not None
                and state.last[:FRAGMENT_HEADER_BYTES]
                == encode_fragment_header(header.version, header.flags,
                                          header.frame_id,
                                          header.total_length,
                                          header.offset)[:FRAGMENT_HEADER_BYTES]
                and state.last[FRAGMENT_HEADER_BYTES:] == payload)


async def send_frame(client: BleakClient, characteristic: str, payload: bytes,
                     mtu: int, frame_id: int) -> None:
    """Send one complete message (transport byte + payload) as fragments."""
    message = payload
    total = len(message)
    max_chunk = mtu - FRAGMENT_HEADER_BYTES - 3  # ATT opcode overhead
    if max_chunk <= 0:
        raise ValueError("MTU too small for framing")
    for offset in range(0, total, max_chunk):
        chunk = message[offset:offset + max_chunk]
        flags = 0
        if offset == 0:
            flags |= FRAGMENT_START
        if offset + len(chunk) == total:
            flags |= FRAGMENT_END
        header = encode_fragment_header(1, flags, frame_id, total, offset)
        await client.write_gatt_char(characteristic, header + chunk,
                                     response=True)


# --------------------------------------------------------------------------
# BLE link client.
# --------------------------------------------------------------------------


class LinkClient:
    def __init__(self, address: str):
        self.address = address
        self.client: BleakClient | None = None
        self.mtu = 23
        self.call_id = 0
        self.frame_id = 1
        self.session_rx = FrameRx(1024)
        self.control_rx = FrameRx(4096)
        self.cipher: Sec2Cipher | None = None
        self._pending: asyncio.Future | None = None
        self._rx_stream = bytearray()

    async def connect(self) -> None:
        self.client = BleakClient(self.address)
        await self.client.connect()
        await self.client.pair()  # SC Just Works; local confirm on the App
        await self.client.start_notify(
            SESSION_TX_UUID, self._on_indication_session)
        await self.client.start_notify(
            CONTROL_TX_UUID, self._on_indication_control)
        self.mtu = self._negotiate_mtu()

    def _negotiate_mtu(self) -> int:
        # Bleak exposes the connection's negotiated ATT MTU after connect.
        # The BlueZ-side ATT MTU follows the platform default unless the
        # OS is configured otherwise; matrix item 1 records the real value.
        return int(getattr(self.client, "mtu_size", 23))

    def _on_indication_session(self, _handle: int, data: bytearray) -> None:
        self._on_indication(self.session_rx, data)

    def _on_indication_control(self, _handle: int, data: bytearray) -> None:
        self._on_indication(self.control_rx, data)

    def _on_indication(self, rx: FrameRx, data: bytearray) -> None:
        try:
            message = rx.feed(bytes(data))
        except ValueError:
            rx.reset()
            return
        if message is not None and self._pending is not None \
                and not self._pending.done():
            self._pending.set_result(message)

    async def _roundtrip(self, rx: FrameRx, tx_uuid: str, payload: bytes,
                         timeout: float = 6.0) -> bytes:
        if self._pending is not None and not self._pending.done():
            self._pending.cancel()
        self._pending = asyncio.get_event_loop().create_future()
        await send_frame(self.client, tx_uuid, payload, self.mtu,
                         self.frame_id)
        self.frame_id += 1
        if self.frame_id == 0:
            self.frame_id = 1
        try:
            return await asyncio.wait_for(self._pending, timeout)
        finally:
            self._pending = None

    def next_call_id(self) -> int:
        self.call_id += 1
        return self.call_id

    def _encode_request(self, domain: int, major: int, method: int,
                        call_id: int, boot_id: int, body: bytes,
                        recovery: bool = False) -> bytes:
        header = ApplicationHeader(KIND_REQUEST, recovery, domain, major,
                                   method, call_id, boot_id)
        return encode_header(header) + body

    async def protected_request(self, domain: int, major: int, method: int,
                                boot_id: int, body: bytes,
                                channel: str = "session",
                                recovery: bool = False) -> tuple[int, bytes]:
        assert self.cipher is not None
        request = self._encode_request(domain, major, method,
                                       self.next_call_id(), boot_id, body,
                                       recovery=recovery)
        blob = self.cipher.protect(request)
        rx = self.session_rx if channel == "session" else self.control_rx
        tx = SESSION_RX_UUID if channel == "session" else CONTROL_RX_UUID
        response = await self._roundtrip(
            rx, tx, bytes([TRANSPORT_PROTECTED]) + blob)
        if response[0] != TRANSPORT_PROTECTED:
            raise ValueError("unexpected transport type in response")
        plain = self.cipher.unprotect(response[1:])
        status = decode_status(plain[APPLICATION_HEADER_BYTES:
                                     APPLICATION_HEADER_BYTES
                                     + RESPONSE_STATUS_BYTES])
        return status, plain[APPLICATION_HEADER_BYTES
                             + RESPONSE_STATUS_BYTES:]

    async def handshake(self, password: bytes, boot_id: int) -> None:
        """Security 2 SRP handshake over the session channel."""
        session = Sec2Session(SRP_USERNAME.encode(), password)
        cmd0 = encode_cmd0(SRP_USERNAME.encode(), session.public_a)
        response = await self._roundtrip(
            self.session_rx, SESSION_RX_UUID,
            bytes([TRANSPORT_HANDSHAKE]) + cmd0)
        if response[0] != TRANSPORT_HANDSHAKE:
            raise ValueError("unexpected transport type in Resp0")
        status, salt, device_public = decode_resp0(response[1:])
        if status != 0:
            raise RuntimeError(f"Resp0 failed with status {status}")
        session.proof_material(salt, device_public)
        cmd1 = encode_cmd1(session.client_proof(salt, device_public))
        response = await self._roundtrip(
            self.session_rx, SESSION_RX_UUID,
            bytes([TRANSPORT_HANDSHAKE]) + cmd1)
        if response[0] != TRANSPORT_HANDSHAKE:
            raise ValueError("unexpected transport type in Resp1")
        status, device_proof, device_nonce = decode_resp1(response[1:])
        if status != 0:
            raise RuntimeError(f"Resp1 failed with status {status}")
        if not session.verify_device_proof(salt, device_public, device_proof):
            raise RuntimeError("device proof mismatch")
        self.cipher = session.seal(device_nonce)

    async def read_link_state(self) -> dict:
        value = await self.client.read_gatt_char(LINK_STATE_UUID)
        return decode_link_state(bytes(value))

    async def disconnect(self) -> None:
        if self.client is not None:
            await self.client.disconnect()
        self.cipher = None


# --------------------------------------------------------------------------
# Matrix procedures.
# --------------------------------------------------------------------------


async def matrix_item_1(client: LinkClient) -> None:
    """Discovery facts, link_state read/notify, MTU 498/495 boundary."""
    print(json.dumps({"item": 1, "mtu": client.mtu,
                      "link_state": await client.read_link_state()}))
    # 495-byte ATT value accepted; 496 rejected by the ATT layer.
    filler = bytes(495)
    await client.client.write_gatt_char(SESSION_RX_UUID, filler,
                                        response=True)
    print(json.dumps({"item": 1, "write_495": "accepted"}))
    try:
        await client.client.write_gatt_char(SESSION_RX_UUID, bytes(496),
                                            response=True)
        print(json.dumps({"item": 1, "write_496": "ACCEPTED-UNEXPECTED"}))
    except BleakError as error:
        print(json.dumps({"item": 1, "write_496": "rejected",
                          "error": str(error)}))


async def matrix_item_2(client: LinkClient, args) -> None:
    """Public or QR bootstrap: SRP handshake then GetManifest."""
    if args.pop:
        pop = bytes.fromhex(args.pop)
        if len(pop) != 16:
            raise SystemExit("--pop must be 16 bytes of hex")
        password = pop
    elif args.public_password:
        instance = args.public_password  # 3-byte hex advertisement id
        password = derive_public_srp_password(V2_SERVICE_UUID,
                                              bytes.fromhex(instance))
    else:
        raise SystemExit("item 2 needs --pop or --public-password")
    await client.handshake(password, args.boot_id)
    status, body = await client.protected_request(0, 2, 1, args.boot_id, b"")
    print(json.dumps({"item": 2, "get_manifest": STATUS_NAMES.get(status)}))
    assert status == STATUS["OK"] and body


async def matrix_item_3(client: LinkClient, args) -> None:
    """Binding: Prepare -> Commit probe -> local confirm -> Commit ->
    Recovery Query, persisting the long-term credential for the
    after-reboot restore."""
    contract = load_contract(_CONTRACT)
    requested = [0x0001, 0x0002, 0x0003, 0x0101, 0x0102, 0x0103]
    body = encode_message(contract, "core.v2.AuthorizePrepareRequest",
                          {"requested_permissions": requested})
    status, resp = await client.protected_request(0, 2, 3, args.boot_id, body)
    assert status == STATUS["OK"], f"prepare failed: {STATUS_NAMES[status]}"
    value = decode_message(contract, "core.v2.AuthorizePrepareResponse", resp)
    txn_id = int(value["authorization_txn_id"])
    credential = bytes.fromhex(value["credential_id"])
    app_password = bytes.fromhex(value["application_password"])
    commit_body = encode_message(
        contract, "core.v2.AuthorizeCommitRequest",
        {"authorization_txn_id": txn_id, "credential_id": credential.hex()})
    status, resp = await client.protected_request(0, 2, 4, args.boot_id,
                                                  commit_body)
    assert status == STATUS["CONFIRMATION_REQUIRED"], \
        f"probe failed: {STATUS_NAMES[status]}"
    result = decode_message(contract, "core.v2.AuthorizationResult", resp)
    assert result["state"] == "CONFIRMATION_PENDING"
    token = int(result["confirmation_token"])
    print(json.dumps({"item": 3, "phase": "probe",
                      "status": "CONFIRMATION_REQUIRED",
                      "confirmation_token": token}))
    input("Accept the binding on the DEVICE, then press Enter...")
    status, resp = await client.protected_request(0, 2, 4, args.boot_id,
                                                  commit_body)
    assert status == STATUS["OK"], f"commit failed: {STATUS_NAMES[status]}"
    result = decode_message(contract, "core.v2.AuthorizationResult", resp)
    print(json.dumps({"item": 3, "phase": "commit",
                      "state": result["state"],
                      "grants": result["granted_permissions"]}))
    assert result["state"] == "AUTHORIZED"
    recovery_body = encode_message(
        contract, "core.v2.GetAuthorizationRequest",
        {"credential_id": credential.hex()})
    status, resp = await client.protected_request(0, 2, 5, args.boot_id,
                                                  recovery_body,
                                                  recovery=True)
    assert status == STATUS["OK"], \
        f"recovery query failed: {STATUS_NAMES[status]}"
    recovered = decode_message(contract, "core.v2.AuthorizationResult", resp)
    print(json.dumps({"item": 3, "phase": "recovery_query",
                      "state": recovered["state"]}))
    with open(args.state_file, "w", encoding="utf-8") as handle:
        json.dump({"credential": credential.hex(),
                   "application_password": app_password.hex()}, handle)
    print(json.dumps({"item": 3, "state_file": args.state_file}))


async def _wifi_operation(client: LinkClient, args, method: int,
                          body: bytes) -> dict:
    status, resp = await client.protected_request(1, 1, method, args.boot_id,
                                                  body, "control")
    assert status == STATUS["OK"], \
        f"wifi method {method} admission failed: {STATUS_NAMES[status]}"
    contract = load_contract(_CONTRACT)
    accepted = decode_message(contract, "core.v2.OperationAccepted", resp)
    operation_id = int(accepted["operation_id"])
    for _ in range(30):
        request = encode_message(contract, "core.v2.OperationRequest",
                                 {"operation_id": operation_id})
        status, resp = await client.protected_request(0, 2, 6, args.boot_id,
                                                      request, "control")
        assert status == STATUS["OK"]
        operation = decode_message(contract, "core.v2.OperationStatus", resp)
        if operation["state"] in ("SUCCEEDED", "FAILED", "CANCELED"):
            return operation
        await asyncio.sleep(1.0)
    raise TimeoutError(f"wifi method {method} did not reach a terminal state")


async def matrix_item_4(client: LinkClient, args) -> None:
    """Wi-Fi domain: get_status, start_scan + paged results, set_credentials,
    reconnect/forget/auto_connect, and a cancel path."""
    contract = load_contract(_CONTRACT)
    status, resp = await client.protected_request(1, 1, 1, args.boot_id,
                                                  b"", "control")
    assert status == STATUS["OK"]
    wifi_status = decode_message(contract, "wifi.v1.WifiStatus", resp)
    print(json.dumps({"item": 4, "method": "get_status",
                      "state": wifi_status["state"]}))
    scan = await _wifi_operation(client, args, 2, b"")
    print(json.dumps({"item": 4, "method": "start_scan",
                      "state": scan["state"]}))
    for page in (0, 1, 31):
        request = encode_message(
            contract, "wifi.v1.GetScanResultsRequest",
            {"generation": int(wifi_status["generation"]), "page": page})
        status, resp = await client.protected_request(1, 1, 3, args.boot_id,
                                                      request, "control")
        print(json.dumps({"item": 4, "method": "get_scan_results",
                          "page": page, "status": STATUS_NAMES[status]}))
    credentials = encode_message(
        contract, "wifi.v1.SetCredentialsRequest",
        {"credentials": {"ssid": args.wifi_ssid.encode().hex(),
                         "password": args.wifi_password.encode().hex(),
                         "security": "PERSONAL"},
         "client_sync_id": 1, "auto_connect": True})
    provision = await _wifi_operation(client, args, 4, credentials)
    print(json.dumps({"item": 4, "method": "set_credentials",
                      "state": provision["state"],
                      "error": provision["error"]}))
    assert provision["state"] == "SUCCEEDED"
    reconnect = await _wifi_operation(client, args, 6, b"")
    print(json.dumps({"item": 4, "method": "reconnect_saved",
                      "state": reconnect["state"]}))
    auto = await _wifi_operation(client, args, 8,
                                 encode_message(
                                     contract, "wifi.v1.SetAutoConnectRequest",
                                     {"enabled": False}))
    print(json.dumps({"item": 4, "method": "set_auto_connect",
                      "state": auto["state"]}))
    forget = await _wifi_operation(client, args, 7, b"")
    print(json.dumps({"item": 4, "method": "forget_saved",
                      "state": forget["state"]}))
    disconnect = await _wifi_operation(client, args, 5, b"")
    print(json.dumps({"item": 4, "method": "disconnect",
                      "state": disconnect["state"]}))


async def matrix_item_5(client: LinkClient, args) -> None:
    """Cold-start gate-open: boot-instant calls stay inside allowed sets."""
    for domain, method in [(0, 1), (0, 2), (1, 1), (1, 2), (1, 3)]:
        status, _ = await client.protected_request(
            domain, 2 if domain == 0 else 1, method, args.boot_id, b"")
        allowed = STATUS_NAMES.get(status)
        print(json.dumps({"item": 5, "method": f"{domain}.{method}",
                          "status": allowed,
                          "allowed": status in ALLOWED_STATUSES[(domain,
                                                                method)]}))


async def matrix_soak(client: LinkClient, args) -> None:
    deadline = time.monotonic() + args.soak_seconds
    iteration = 0
    while time.monotonic() < deadline:
        status, _ = await client.protected_request(0, 2, 2, args.boot_id, b"")
        print(json.dumps({"item": 8, "iteration": iteration,
                          "get_link_snapshot": STATUS_NAMES.get(status)}))
        assert status == STATUS["OK"]
        iteration += 1
        await asyncio.sleep(5.0)


async def run_matrix(args) -> None:
    client = LinkClient(args.device)
    await client.connect()
    try:
        if args.item == 1:
            await matrix_item_1(client)
        elif args.item == 2:
            await matrix_item_2(client, args)
        elif args.item == 3:
            await matrix_item_3(client, args)
        elif args.item == 4:
            await matrix_item_4(client, args)
        elif args.item == 5:
            await matrix_item_5(client, args)
        elif args.item == 8:
            await matrix_soak(client, args)
        else:
            raise SystemExit(
                "items 6 and 7 are operator-driven (fault-instrumented "
                "debug build / factory reset); see README")
    finally:
        await client.disconnect()


async def scan(args) -> None:
    def matches(_device, adv):
        return V2_SERVICE_UUID in (adv.service_uuids or [])

    devices = await BleakScanner.discover(timeout=args.timeout)
    for device in devices:
        if not device.metadata:
            continue
        uuids = device.metadata.get("uuids", [])
        service_data = device.metadata.get("service_data", {})
        sd = service_data.get(V2_SERVICE_UUID, b"")
        if V2_SERVICE_UUID in uuids or sd:
            mode, identifier = ("?", b"")
            if len(sd) == 5:
                _, identifier = decode_service_data(sd)
                mode = MODE_BINDABLE if sd[1] & 1 else "public"
            print(json.dumps({"address": device.address,
                              "name": device.name, "mode": mode,
                              "identifier": identifier.hex()}))


def self_test() -> None:
    """Offline SRP round-trip: an independent Python device-side
    implementation derives the same session key and verifies the client
    proof. Validates internal consistency of both readings of the IDF
    algorithm; hardware interop is proven by matrix item 2."""
    username = b"microtech"
    password = b"self-test-password"
    salt = secrets.token_bytes(16)
    inner = SHA512(username + b":" + password)
    x = _mpi(SHA512(salt + inner))
    verifier = pow(G, x, N_3072)
    device_b = secrets.randbelow(N_3072 - 1) + 1
    k = _mpi(SHA512(N_3072.to_bytes(N_BYTES, "big")
                    + _padded(b"\x05", N_BYTES)))
    device_public = (k * verifier + pow(G, device_b, N_3072)) % N_3072

    client = Sec2Session(username, password)
    client.proof_material(salt, device_public)
    u = _mpi(SHA512(_padded(_minimal_be(client.public_a), N_BYTES)
                    + _padded(_minimal_be(device_public), N_BYTES)))
    device_shared = pow(client.public_a * pow(verifier, u, N_3072),
                        device_b, N_3072)
    device_key = SHA512(_minimal_be(device_shared))
    assert client._key == device_key, "session keys diverge"
    proof = client.client_proof(salt, device_public)
    hash_n_xor_g = bytes(
        a ^ b for a, b in zip(SHA512(N_3072.to_bytes(N_BYTES, "big")),
                              SHA512(_padded(b"\x05", N_BYTES))))
    expected_m1 = SHA512(hash_n_xor_g + SHA512(username) + salt
                         + _minimal_be(client.public_a)
                         + _minimal_be(device_public) + device_key)
    assert secrets.compare_digest(proof, expected_m1), "client proof diverges"
    device_m2 = SHA512(_minimal_be(client.public_a) + proof + device_key)
    assert client.verify_device_proof(salt, device_public, device_m2), \
        "device proof not verified"
    # Cipher counter convention: IV = nonce[:8] + counter(4, BE, from 1).
    cipher = client.seal(b"\x01" * 8 + b"\x00\x00\x00\x01")
    blob = cipher.protect(b"hello")
    cipher2 = Sec2Cipher(client.aes_key, b"\x01" * 8 + b"\x00\x00\x00\x01")
    assert cipher2.unprotect(blob) == b"hello", "cipher round-trip failed"
    print("sec2 self-test passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    self_parser = sub.add_parser("self-test")
    self_parser.set_defaults(func=lambda _args: self_test())
    scan_parser = sub.add_parser("scan")
    scan_parser.add_argument("--timeout", type=float, default=10.0)
    matrix_parser = sub.add_parser("matrix")
    matrix_parser.add_argument("--item", type=int, required=True,
                               choices=[1, 2, 3, 4, 5, 8])
    matrix_parser.add_argument("--device", required=True)
    matrix_parser.add_argument("--boot-id", type=lambda v: int(v, 0),
                               required=True)
    matrix_parser.add_argument("--pop", metavar="HEX16")
    matrix_parser.add_argument("--public-password", metavar="HEX3")
    matrix_parser.add_argument("--state-file", default="device-link-state.json")
    matrix_parser.add_argument("--wifi-ssid", default="MatrixAP")
    matrix_parser.add_argument("--wifi-password", default="matrix-password")
    matrix_parser.add_argument("--soak-seconds", type=int, default=7200)
    args = parser.parse_args()
    if args.command == "self-test":
        self_test()
    elif args.command == "scan":
        asyncio.run(scan(args))
    else:
        asyncio.run(run_matrix(args))


if __name__ == "__main__":
    main()
