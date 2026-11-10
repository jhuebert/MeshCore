"""Shared harness for radio2 on-air tests.

Two Xiao S3 WIO companions (nc3/nc4) and one Xiao S3 WIO observer repeater.
The companions are driven over their WiFi TCP interface; the repeater's remote
CLI is driven through one companion that stays on the primary preset.

Endpoints, the repeater admin password and the private-channel PSK can be
overridden with environment variables; the defaults match Jason's bench.
"""
import asyncio
import json
import os
import sys
import time

_MC_SRC = os.environ.get("MESHCORE_PY_SRC", "/home/jason/dev/meshcore/meshcore_py/src")
if _MC_SRC not in sys.path:
    sys.path.insert(0, _MC_SRC)

from meshcore import MeshCore, EventType  # noqa: E402

# ---- bench topology -------------------------------------------------------
PRIMARY = (910.525, 62.5, 7, 8)      # the live mesh preset
XIAO_A = ("192.168.1.165", 5000)     # nc3: moved to the secondary preset
XIAO_B = ("192.168.1.154", 5000)     # nc4: stays on primary, also the CLI bridge
OPENHOP = ("192.168.1.55", 9508)     # full contact list source (read-only)

REPEATER_PREFIX = os.environ.get("RADIO2_REPEATER_PREFIX", "3d3bc06e")
REPEATER_PW = os.environ.get("RADIO2_REPEATER_PW", "password")

# Deliberately not a well-known name: the PSK is pinned so every run is the
# same channel, and nothing here is ever posted to Public/#test.
LAB_CHAN_NAME = "r2lab"
LAB_CHAN_PSK = bytes.fromhex(os.environ.get(
    "RADIO2_LAB_PSK", "5a2b7c1e9f0348d6a1c2b3d4e5f60718"))

PASS, FAIL = [], []


def check(desc, got, want):
    try:
        ok = want(got) if callable(want) else (got == want)
    except Exception as e:   # a None reply must fail the check, not crash the run
        ok = False
        got = f"{got!r} ({type(e).__name__}: {e})"
    (PASS if ok else FAIL).append((desc, got))
    print(("PASS  " if ok else "FAIL  ") + desc + ("" if ok else f"\n      got: {got!r}"))
    return ok


def summary(name):
    print(f"\n== {name}: {len(PASS)} passed, {len(FAIL)} failed ==")
    for d, g in FAIL:
        print(f"  FAIL: {d} -> {g!r}")
    return len(FAIL) == 0


def parse_status(line):
    """Parse a `radio2 status` reply into a dict of typed fields."""
    out = {}
    if not line:
        return out
    for part in line.split(";"):
        part = part.strip()
        if not part:
            continue
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
        else:
            out.setdefault("flags", []).append(part)
    for k in ("sw", "swf", "both", "bdrop", "drop", "abrt", "nf"):
        if k in out:
            try:
                out[k] = int(out[k])
            except ValueError:
                pass
    for k in ("rx", "tx", "split"):
        if k in out and "/" in str(out[k]):
            a, b = str(out[k]).split("/", 1)
            out[k] = (int(a), int(b))
    return out


class Companion:
    """One MeshCore companion over TCP."""

    def __init__(self, name, endpoint=None):
        self.name = name
        self.endpoint = endpoint
        self.mc = None
        self.channels = {}
        # Messages the remote-CLI poller sees but does not need are kept here
        # so a later channel assertion can still find them.
        self.pending = []

    async def connect(self, retries=20, delay=2.0):
        host, port = self.endpoint
        last = None
        for _ in range(retries):
            try:
                self.mc = await MeshCore.create_tcp(host, port, only_error=True,
                                                    default_timeout=10)
                if self.mc is not None:
                    await self.drain()
                    await self.load_channels()
                    return self
            except Exception as e:   # device rebooting / wifi reconnecting
                last = e
            await asyncio.sleep(delay)
        raise RuntimeError(f"{self.name}: connect failed: {last}")

    async def close(self):
        if self.mc:
            try:
                await self.mc.disconnect()
            except Exception:
                pass
            self.mc = None

    async def drain(self, cap=200):
        """Discard queued messages; returns the ones seen."""
        seen = []
        self.pending.clear()
        for _ in range(cap):
            ev = await self.mc.commands.get_msg(timeout=1.0)
            if ev is None or ev.type == EventType.NO_MORE_MSGS:
                break
            seen.append(ev.payload)
        return seen

    async def load_channels(self):
        self.channels = {}
        for i in range(40):
            try:
                ev = await self.mc.commands.get_channel(i)
            except Exception:
                continue
            p = ev.payload if ev else None
            if isinstance(p, dict) and p.get("channel_name"):
                self.channels[p["channel_name"]] = i

    async def ensure_channel(self, name, psk, preferred):
        await self.load_channels()
        if name in self.channels:
            return self.channels[name]
        ev = await self.mc.commands.set_channel(preferred, name, psk)
        if ev is None or ev.type == EventType.ERROR:
            # find a free slot
            for i in range(40):
                if i in self.channels.values():
                    continue
                ev = await self.mc.commands.set_channel(i, name, psk)
                if ev is not None and ev.type != EventType.ERROR:
                    preferred = i
                    break
            else:
                raise RuntimeError(f"{self.name}: no free channel slot")
        self.channels[name] = preferred
        return preferred

    async def remove_channel(self, idx):
        ev = await self.mc.commands.set_channel(idx, "", bytes(16))
        return ev

    async def set_radio(self, preset):
        freq, bw, sf, cr = preset
        ev = await self.mc.commands.set_radio(freq, bw, sf, cr)
        await asyncio.sleep(0.5)
        return ev

    async def radio(self):
        ev = await self.mc.commands.send_appstart()
        p = ev.payload
        return (p["radio_freq"], p["radio_bw"], p["radio_sf"], p["radio_cr"])

    def make_contact(self):
        info = self.mc.self_info
        return {
            "public_key": info["public_key"],
            "type": info["adv_type"],
            "flags": 0,
            "out_path": "",
            "out_path_len": -1,
            "out_path_hash_mode": 0,
            "adv_name": info["name"],
            "last_advert": int(time.time()),
            "adv_lat": info.get("adv_lat", 0.0),
            "adv_lon": info.get("adv_lon", 0.0),
        }

    async def send_chan(self, idx, text):
        return await self.mc.commands.send_chan_msg(idx, text)

    async def wait_chan(self, token, timeout=20.0, channel_idx=None):
        """Wait until a channel message containing `token` is delivered."""
        def matches(p):
            return (token in (p.get("text") or "") and
                    (channel_idx is None or p.get("channel_idx") == channel_idx))

        for p in list(self.pending):
            if matches(p):
                self.pending.remove(p)
                return p
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ev = await self.mc.commands.get_msg(
                timeout=min(4.0, max(0.5, deadline - time.monotonic())))
            if ev is None or ev.type == EventType.NO_MORE_MSGS:
                await asyncio.sleep(0.3)
                continue
            if ev.type != EventType.CHANNEL_MSG_RECV:
                continue
            p = ev.payload
            if matches(p):
                return p
        return None

    async def get_contacts(self):
        ev = await self.mc.commands.get_contacts(timeout=8)
        return ev.payload if ev else {}

    async def contact_prefix(self, prefix):
        for pk, c in (await self.get_contacts()).items():
            if pk.startswith(prefix):
                return c
        return None

    async def add_contact(self, contact):
        return await self.mc.commands.add_contact(contact)

    async def advert(self, flood=True):
        return await self.mc.commands.send_advert(flood=flood)


async def fetch_contact(prefix, source=OPENHOP, cache="/tmp/opencode/repcontact.json"):
    try:
        with open(cache) as f:
            c = json.load(f)
            if c["public_key"].startswith(prefix):
                return c
    except Exception:
        pass
    mc = await MeshCore.create_tcp(source[0], source[1], only_error=True,
                                   default_timeout=10)
    try:
        ev = await mc.commands.get_contacts(timeout=8)
        for pk, c in (ev.payload or {}).items():
            if pk.startswith(prefix):
                with open(cache, "w") as f:
                    json.dump(c, f, indent=1)
                return c
    finally:
        await mc.disconnect()
    raise RuntimeError("repeater contact not found in source list")


class Repeater:
    """Remote CLI for the repeater, bridged through a primary-preset companion."""

    def __init__(self, bridge: Companion, prefix=REPEATER_PREFIX, password=REPEATER_PW):
        self.bridge = bridge
        self.prefix = prefix
        self.password = password
        self.contact = None
        self.logged_in = False

    async def login(self):
        self.contact = await fetch_contact(self.prefix)
        await self.bridge.add_contact(self.contact)
        await self.bridge.drain()
        ev = await self.bridge.mc.commands.send_login_sync(
            self.contact, self.password, min_timeout=25)
        self.logged_in = ev is not None and ev.type == EventType.LOGIN_SUCCESS
        return self.logged_in

    async def cmd(self, text, timeout=25.0, retries=2):
        for attempt in range(retries + 1):
            if not self.logged_in and not await self.login():
                continue
            res = await self.bridge.mc.commands.send_cmd(self.contact, text)
            if res is None or res.type == EventType.ERROR:
                continue
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                ev = await self.bridge.mc.commands.get_msg(
                    timeout=min(4.0, max(0.5, deadline - time.monotonic())))
                if ev is None or ev.type == EventType.NO_MORE_MSGS:
                    await asyncio.sleep(0.3)
                    continue
                p = ev.payload
                if ev.type == EventType.CONTACT_MSG_RECV and \
                        str(p.get("pubkey_prefix", "")).startswith(self.prefix):
                    return p.get("text")
                # Keep channel traffic for the channel assertions; the CLI
                # poller must not swallow it.
                self.bridge.pending.append(p)
            # a lost reply may mean the session was forgotten (reboot): re-login
            if attempt < retries:
                self.logged_in = False
        return None

    async def radio2(self):
        return await self.cmd("radio2")

    async def status(self):
        return await self.cmd("radio2 status")

    async def timing(self):
        return await self.cmd("radio2 timing")
