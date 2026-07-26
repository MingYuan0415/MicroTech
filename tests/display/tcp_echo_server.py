#!/usr/bin/env python3
"""Threaded TCP echo endpoint for the ESP32-S3 display benchmark."""

import argparse
import socketserver
import threading
import time


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        connection_id = self.server.next_connection_id()
        started = time.monotonic()
        received_bytes = 0
        transmitted_bytes = 0
        outcome = "peer_closed"
        print(
            f"TCP echo connection={connection_id} peer={self.client_address[0]}:"
            f"{self.client_address[1]} opened",
            flush=True,
        )
        try:
            while True:
                data = self.request.recv(64 * 1024)
                if not data:
                    break
                received_bytes += len(data)
                offset = 0
                while offset < len(data):
                    sent = self.request.send(data[offset:])
                    if sent == 0:
                        raise ConnectionResetError("zero-byte TCP send")
                    offset += sent
                    transmitted_bytes += sent
        except ConnectionResetError as error:
            outcome = f"reset:{error}"
        except BrokenPipeError as error:
            outcome = f"broken_pipe:{error}"
        except OSError as error:
            outcome = f"socket_error:{error}"
        finally:
            duration = time.monotonic() - started
            print(
                f"TCP echo connection={connection_id} closed duration_s="
                f"{duration:.3f} rx_bytes={received_bytes} "
                f"tx_bytes={transmitted_bytes} outcome={outcome}",
                flush=True,
            )


class EchoServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, server_address, handler_class):
        super().__init__(server_address, handler_class)
        self._connection_id = 0
        self._connection_lock = threading.Lock()

    def next_connection_id(self):
        with self._connection_lock:
            self._connection_id += 1
            return self._connection_id


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5001)
    args = parser.parse_args()
    with EchoServer((args.host, args.port), EchoHandler) as server:
        print(f"TCP echo listening on {args.host}:{args.port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print("TCP echo stopping", flush=True)


if __name__ == "__main__":
    main()
