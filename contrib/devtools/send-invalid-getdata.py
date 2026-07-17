#!/usr/bin/env python3
# Copyright (c) 2026 TBCNODE developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Send a getdata request for an invalid block hash to a P2P peer."""

import argparse
import hashlib
import random
import socket
import struct
import time


NETWORK_MAGICS = {
    "mainnet": bytes.fromhex("e3e1f3e8"),
    "testnet3": bytes.fromhex("f4e5f3f4"),
    "stn": bytes.fromhex("fbcec4f9"),
    "regtest": bytes.fromhex("dab5bffa"),
}
DEFAULT_PROTOCOL_VERSION = 90015
DEFAULT_SUBVERSION = b"/tbc-p2p-getdata-tool:0.0.1/"
DEFAULT_HASH = "ff" * 32
MSG_BLOCK = 2
NODE_NETWORK = 1 << 0
NODE_BITCOIN_CASH = 1 << 5
DEFAULT_SERVICES = NODE_NETWORK | NODE_BITCOIN_CASH


def hash256(payload):
    return hashlib.sha256(hashlib.sha256(payload).digest()).digest()


def ser_compact_size(size):
    if size < 253:
        return struct.pack("<B", size)
    if size < 0x10000:
        return struct.pack("<BH", 253, size)
    if size < 0x100000000:
        return struct.pack("<BI", 254, size)
    return struct.pack("<BQ", 255, size)


def ser_string(data):
    return ser_compact_size(len(data)) + data


def ser_address_in_version(host, port, services):
    return b"".join((
        struct.pack("<Q", services),
        b"\x00" * 10 + b"\xff" * 2,
        socket.inet_aton(host),
        struct.pack(">H", port),
    ))


def ser_uint256_from_hex(value):
    value = value.removeprefix("0x").lower()
    if len(value) != 64:
        raise ValueError("hash must be exactly 32 bytes / 64 hex characters")
    return bytes.fromhex(value)[::-1]


def make_message(magic, command, payload=b""):
    command_bytes = command.encode("ascii")
    if len(command_bytes) > 12:
        raise ValueError("P2P command name is too long")
    return b"".join((
        magic,
        command_bytes + b"\x00" * (12 - len(command_bytes)),
        struct.pack("<I", len(payload)),
        hash256(payload)[:4],
        payload,
    ))


def make_version(host, port, version, services, subversion, starting_height):
    return b"".join((
        struct.pack("<i", version),
        struct.pack("<Q", services),
        struct.pack("<q", int(time.time())),
        ser_address_in_version(host, port, services),
        ser_address_in_version("0.0.0.0", 0, services),
        struct.pack("<Q", random.getrandbits(64)),
        ser_string(subversion.encode("utf-8")),
        struct.pack("<i", starting_height),
        struct.pack("<b", 1),
    ))


def make_protoconf(max_recv_payload_length):
    return ser_compact_size(1) + struct.pack("<i", max_recv_payload_length)


def make_getdata_block(block_hash):
    inv = struct.pack("<i", MSG_BLOCK) + ser_uint256_from_hex(block_hash)
    return ser_compact_size(1) + inv


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("peer closed the connection")
        data.extend(chunk)
    return bytes(data)


def read_message(sock, magic):
    header = read_exact(sock, 24)
    peer_magic, command_raw, payload_len, checksum = struct.unpack("<4s12sI4s", header)
    if peer_magic != magic:
        raise ValueError("received message with unexpected magic: {}".format(peer_magic.hex()))

    payload = read_exact(sock, payload_len)
    if hash256(payload)[:4] != checksum:
        raise ValueError("bad checksum for {}".format(command_raw.rstrip(b'\x00').decode("ascii", "replace")))

    command = command_raw.rstrip(b"\x00").decode("ascii")
    return command, payload


def wait_for_handshake(sock, magic):
    saw_version = False
    saw_verack = False

    while not (saw_version and saw_verack):
        command, payload = read_message(sock, magic)
        print("<- {}".format(command))

        if command == "version":
            saw_version = True
            sock.sendall(make_message(magic, "verack"))
            print("-> verack")
        elif command == "verack":
            saw_verack = True
        elif command == "ping":
            sock.sendall(make_message(magic, "pong", payload))
            print("-> pong")
        elif command == "reject":
            raise RuntimeError("peer rejected our handshake: {}".format(payload.hex()))


def wait_for_pong(sock, magic, expected_nonce):
    while True:
        command, payload = read_message(sock, magic)
        print("<- {}".format(command))

        if command == "ping":
            sock.sendall(make_message(magic, "pong", payload))
            print("-> pong")
            continue

        if command == "pong" and payload == expected_nonce:
            return

        if command == "reject":
            raise RuntimeError("peer rejected a message: {}".format(payload.hex()))


def send_invalid_getdata(host, port, magic, block_hash, timeout, version, services, subversion,
                         starting_height, max_recv_payload_length, debug_version):
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)

        version_payload = make_version(host, port, version, services, subversion, starting_height)
        if debug_version:
            print("version payload: {}".format(version_payload.hex()))
        sock.sendall(make_message(magic, "version", version_payload))
        print("-> version")

        wait_for_handshake(sock, magic)

        sock.sendall(make_message(magic, "protoconf", make_protoconf(max_recv_payload_length)))
        print("-> protoconf")

        sock.sendall(make_message(magic, "getdata", make_getdata_block(block_hash)))
        print("-> getdata MSG_BLOCK {}".format(block_hash))

        nonce = struct.pack("<Q", random.getrandbits(64))
        sock.sendall(make_message(magic, "ping", nonce))
        print("-> ping")

        wait_for_pong(sock, magic, nonce)
        print("peer replied with pong; getdata was processed and the connection is alive")


def parse_magic(network, magic_hex):
    if magic_hex:
        magic_hex = magic_hex.removeprefix("0x")
        if len(magic_hex) != 8:
            raise ValueError("--magic must be exactly 4 bytes / 8 hex characters")
        return bytes.fromhex(magic_hex)
    return NETWORK_MAGICS[network]


def main():
    parser = argparse.ArgumentParser(
        description="Send a P2P getdata request for an invalid/unknown block hash.")
    parser.add_argument("--host", default="127.0.0.1", help="P2P peer host, default: %(default)s")
    parser.add_argument("--port", type=int, default=18444, help="P2P peer port, default: %(default)s")
    parser.add_argument("--network", choices=sorted(NETWORK_MAGICS), default="regtest",
                        help="network magic to use, default: %(default)s")
    parser.add_argument("--magic", help="override network magic as 4-byte hex, e.g. e3e1f3e8")
    parser.add_argument("--hash", default=DEFAULT_HASH, help="block hash hex to request, default: all ff")
    parser.add_argument("--timeout", type=float, default=10.0, help="socket timeout in seconds")
    parser.add_argument("--version", type=int, default=DEFAULT_PROTOCOL_VERSION, help="P2P protocol version")
    parser.add_argument("--services", type=lambda value: int(value, 0), default=DEFAULT_SERVICES,
                        help="version services bitfield, default: %(default)s")
    parser.add_argument("--subversion", default=DEFAULT_SUBVERSION.decode("utf-8"),
                        help="P2P user agent subversion")
    parser.add_argument("--starting-height", type=int, default=0,
                        help="version starting height, default: %(default)s")
    parser.add_argument("--max-recv-payload-length", type=int, default=2 * 1024 * 1024,
                        help="protoconf max_recv_payload_length")
    parser.add_argument("--debug-version", action="store_true",
                        help="print serialized version payload before sending it")
    args = parser.parse_args()
    magic = parse_magic(args.network, args.magic)

    send_invalid_getdata(
        args.host,
        args.port,
        magic,
        args.hash,
        args.timeout,
        args.version,
        args.services,
        args.subversion,
        args.starting_height,
        args.max_recv_payload_length,
        args.debug_version,
    )


if __name__ == "__main__":
    main()
