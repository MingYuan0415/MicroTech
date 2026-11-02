#!/usr/bin/env python3
"""Drive the simulator real-music backend through login and user flows.

The flow checks the account profile, sidebar identity, playlist load, playback,
pause/resume, next/previous, queue, and lyrics pages. Start a real-backend
simulator first (for example `python3 sim/dev.py`), then run:

    python3 sim/tools/music_flow.py --port 5002

The only manual step is scanning the QQ QR with a phone. The credential is
persisted in the simulator's NVS directory; later runs skip the login wait.
"""
import argparse
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from simctl import rpc  # noqa: E402

MUSIC_SERVICE_STATE_READY = 8
MUSIC_PLAYBACK_PLAYING = 2
MUSIC_PLAYBACK_PAUSED = 3
MUSIC_SERVICE_STATE_WAITING_NETWORK = 1
MUSIC_SERVICE_STATE_WAITING_TIME = 2


class SimClient:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)

    def close(self):
        self.sock.close()

    def call(self, method, params=None):
        reply = rpc(self.sock, method, params)
        if not reply.get("ok"):
            raise RuntimeError(reply.get("error", "sim request failed"))
        return reply.get("result", {})

    def wait_idle(self, timeout_ms=6000):
        return self.call("sim.wait_idle", {"timeout_ms": timeout_ms})

    def screenshot(self, name):
        return self.call("sim.screenshot",
                         {"name": name, "wait_idle": True}).get("path")

    def tree(self):
        return self.call("sim.tree").get("tree", {})

    def _step(self, ms):
        try:
            self.call("sim.step", {"ms": ms})
        except RuntimeError as exc:
            # A free-running dev session advances on its own; only CI sessions
            # accept explicit frame steps.
            if "not in --ci mode" not in str(exc):
                raise

    def touch(self, x, y):
        self.call("sim.touch", {"action": "down", "x": x, "y": y})
        self._step(33)
        self.call("sim.touch", {"action": "up", "x": x, "y": y})
        self._step(330)
        time.sleep(0.8)


def _walk(node):
    yield node
    for child in node.get("children", []):
        yield from _walk(child)


def _first_action_row(tree, min_y):
    candidates = []
    for node in _walk(tree):
        coords = node.get("coords", {})
        if (node.get("flags", {}).get("clickable") and
                coords.get("w", 0) > 300 and coords.get("y", 0) >= min_y):
            candidates.append(node)
    if not candidates:
        return None
    return min(candidates, key=lambda node: node["coords"]["y"])


def _tree_has_text(tree, text):
    return any(node.get("text") == text for node in _walk(tree))


def _find_text_node(tree, text):
    for node in _walk(tree):
        if node.get("text") == text:
            return node
    return None


def _tap_node(client, node):
    coords = node.get("coords", {})
    client.touch(coords.get("x", 0) + coords.get("w", 0) // 2,
                 coords.get("y", 0) + coords.get("h", 0) // 2)


def _wait_for_status(client, predicate, timeout, label):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = client.call("sim.music", {"action": "status"})
        if predicate(last):
            return last
        time.sleep(0.5)
    raise RuntimeError("%s timed out; last status=%s" %
                       (label, json.dumps(last, ensure_ascii=False)))


def _wait_for_tree_text(client, text, timeout):
    deadline = time.time() + timeout
    tree = None
    while time.time() < deadline:
        tree = client.tree()
        if _tree_has_text(tree, text):
            return tree
        time.sleep(0.5)
        client.wait_idle()
    raise RuntimeError("timed out waiting for %r" % text)


def _open_sidebar(client):
    for _ in range(3):
        client.touch(42, 25)
        client.wait_idle()
        deadline = time.time() + 3.0
        while time.time() < deadline:
            tree = client.tree()
            if _tree_has_text(tree, "个性电台"):
                return tree
            time.sleep(0.3)
    raise RuntimeError("sidebar did not open")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=5002)
    parser.add_argument("--login-timeout", type=float, default=300.0,
                        help="seconds to wait for the phone scan")
    parser.add_argument("--qr-path", default=None)
    args = parser.parse_args()

    client = SimClient(args.port)
    try:
        status = client.call("sim.music", {"action": "status"})
        print("music status: state=%s logged_in=%s" %
              (status.get("state"), status.get("logged_in")))
        if status.get("state") in (MUSIC_SERVICE_STATE_WAITING_NETWORK,
                                   MUSIC_SERVICE_STATE_WAITING_TIME):
            status = _wait_for_status(
                client,
                lambda s: s.get("state") not in
                (MUSIC_SERVICE_STATE_WAITING_NETWORK,
                 MUSIC_SERVICE_STATE_WAITING_TIME),
                10.0, "music service time/network")
        if status.get("state") != MUSIC_SERVICE_STATE_READY or not status.get("logged_in"):
            client.call("sim.music", {"action": "login", "type": "qq"})
            _wait_for_status(client,
                             lambda s: s.get("qr_available"),
                             10.0, "QR ready")
            try:
                qr_params = {"action": "qr_save"}
                if args.qr_path is not None:
                    qr_params["path"] = args.qr_path
                print("QR saved:", client.call("sim.music",
                                               qr_params).get("path"))
            except RuntimeError as exc:
                print("QR save skipped:", exc)
            print("Scan the QR with QQ; waiting up to %.0f s ..." %
                  args.login_timeout)
            status = _wait_for_status(
                client,
                lambda s: s.get("state") == MUSIC_SERVICE_STATE_READY and
                s.get("logged_in"),
                args.login_timeout, "QQ login")
            print("login ready: user=%s" % status.get("user_name"))

        client.call("sim.music", {"action": "refresh"})
        status = _wait_for_status(
            client,
            lambda s: s.get("logged_in") and s.get("user_name") and
            s.get("user_avatar"),
            30.0, "account profile")
        print("profile: user=%s avatar=%s" %
              (status.get("user_name"), status.get("user_avatar")))

        client.call("sim.navigate", {"app": "music"})
        client.wait_idle()
        print("player:", client.screenshot("music_flow_home"))
        tree = _open_sidebar(client)
        for expected in (status.get("user_name"), "每日推荐", "喜欢", "自建歌单"):
            if not _tree_has_text(tree, expected):
                raise RuntimeError("sidebar missing %r" % expected)
        print("sidebar:", client.screenshot("music_flow_sidebar"))
        daily = _find_text_node(tree, "每日推荐")
        if daily is None:
            raise RuntimeError("no daily recommendation shortcut")
        _tap_node(client, daily)
        client.wait_idle()
        tree = _wait_for_tree_text(client, "歌单", 20.0)
        print("playlist:", client.screenshot("music_flow_playlist"))

        row = _first_action_row(tree, 150)
        if row is None:
            raise RuntimeError("no song row in playlist")
        _tap_node(client, row)
        client.wait_idle()
        player = _wait_for_status(
            client,
            lambda s: s.get("player_state") == MUSIC_PLAYBACK_PLAYING and
            s.get("position_ms", 0) > 0,
            90.0, "CDN playback")
        print("player:", json.dumps(player, ensure_ascii=False))
        print("player screenshot:", client.screenshot("music_flow_player"))

        first_title = player.get("title")
        client.call("sim.music", {"action": "play_pause"})
        paused = _wait_for_status(
            client, lambda s: s.get("player_state") == MUSIC_PLAYBACK_PAUSED,
            10.0, "pause")
        print("pause: position=%s" % paused.get("position_ms"))
        client.call("sim.music", {"action": "play_pause"})
        _wait_for_status(
            client, lambda s: s.get("player_state") == MUSIC_PLAYBACK_PLAYING,
            20.0, "resume")

        client.call("sim.music", {"action": "next"})
        next_status = _wait_for_status(
            client,
            lambda s: s.get("title") and s.get("title") != first_title and
            s.get("player_state") == MUSIC_PLAYBACK_PLAYING,
            90.0, "next track")
        print("next: %s" % next_status.get("title"))
        client.call("sim.music", {"action": "prev"})
        _wait_for_status(
            client,
            lambda s: s.get("title") == first_title and
            s.get("player_state") == MUSIC_PLAYBACK_PLAYING,
            90.0, "previous track")

        client.call("sim.navigate", {"app": "music", "page": "queue"})
        client.wait_idle()
        _wait_for_tree_text(client, "播放列表", 20.0)
        print("queue:", client.screenshot("music_flow_queue"))

        client.call("sim.navigate", {"app": "music", "page": "lyrics"})
        client.wait_idle()
        _wait_for_tree_text(client, "歌词", 20.0)
        print("lyrics:", client.screenshot("music_flow_lyrics"))
        print("PASS")
        return 0
    except (OSError, RuntimeError, ValueError) as exc:
        print("FAIL:", exc, file=sys.stderr)
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
