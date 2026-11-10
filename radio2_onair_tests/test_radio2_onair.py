"""On-air behavior tests for the repeater's second radio preset (radio2).

Bench: two Xiao S3 WIO companions on WiFi TCP and one Xiao S3 WIO observer
repeater, controlled through the companion on the primary preset. All group
traffic uses a private channel (`r2lab`), never Public or #test.

Run with the PlatformIO Python (it has the meshcore client deps):

    ~/.platformio/penv/bin/python radio2_onair_tests/test_radio2_onair.py --section all

Sections:
  cli          CLI contract: validation, status, policy, advert, RXPS conflict
  primary      primary-origin policy matrix, non-crossable traffic, adverts
  secondary    secondary-origin directions (statistical; live stays primary)
  active       secondary reception while a busy primary keeps active mode
  filter       filter rule applies on the radio2 path
  observer     observer capture + start ota on the LAN
  persistence  reboot keeps config, primary-change conflicts (destructive)
  inverted     deterministic scheduler/policy run with a quiet primary (destructive)

`destructive` sections reboot the repeater and/or move its primary preset; they
restore live primary and radio2 state when they finish.
"""
import argparse
import asyncio
import sys
import time

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (  # noqa: E402
    Companion, Repeater, XIAO_A, XIAO_B, PRIMARY, LAB_CHAN_NAME, LAB_CHAN_PSK,
    REPEATER_PREFIX, EventType, check, parse_status,
)

SECONDARY = (915.125, 125, 9, 5)
QUIET_PRIMARY = (916.0, 125, 9, 5)


def delta(a, b, key):
    if key in ("rx", "tx", "split"):
        return (b[key][0] - a[key][0], b[key][1] - a[key][1])
    return b.get(key, 0) - a.get(key, 0)


async def status(rep):
    return parse_status(await rep.status())


async def drain_tokens(c, token, timeout=3.0, channel_idx=None):
    def matches(p):
        return (token in str(p.get("text", "")) and
                (channel_idx is None or p.get("channel_idx") == channel_idx))

    out = [p for p in list(c.pending) if matches(p)]
    for p in out:
        c.pending.remove(p)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ev = await c.mc.commands.get_msg(timeout=1.0)
        if ev is None or ev.type == EventType.NO_MORE_MSGS:
            await asyncio.sleep(0.2)
            continue
        p = ev.payload
        if ev.type == EventType.CHANNEL_MSG_RECV and matches(p):
            out.append(p)
    return out


async def drain_contacts(c, token, timeout=3.0):
    def matches(p):
        return token in str(p.get("text", ""))

    out = [p for p in list(c.pending) if p.get("type") == "PRIV" and matches(p)]
    for p in out:
        c.pending.remove(p)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ev = await c.mc.commands.get_msg(timeout=1.0)
        if ev is None or ev.type == EventType.NO_MORE_MSGS:
            await asyncio.sleep(0.2)
            continue
        p = ev.payload
        if ev.type == EventType.CONTACT_MSG_RECV and matches(p):
            out.append(p)
    return out


async def add_contacts(a, b):
    """Both companions know the repeater and each other."""
    from harness import fetch_contact
    rc = await fetch_contact(REPEATER_PREFIX)
    await a.add_contact(rc)
    await b.add_contact(rc)
    await a.add_contact(b.make_contact())
    await b.add_contact(a.make_contact())
    await asyncio.sleep(2)


async def ensure_layout(a, b, rep, secondary, policy=None, advert=None):
    await a.set_radio(secondary)
    await b.set_radio(PRIMARY)
    await asyncio.sleep(1)
    if policy:
        await rep.cmd(f"radio2 policy {policy}")
    if advert:
        await rep.cmd(f"radio2 advert {advert}")
    await asyncio.sleep(1)


# ---------------------------------------------------------------- sections

async def section_cli(a, b, rep):
    check("radio2 reports a state", await rep.radio2(), lambda s: s is not None)
    check("radio2 off keeps preset", await rep.cmd("radio2 off"), lambda s: s.startswith("OK"))
    check("radio2 off", await rep.radio2(), "off")
    check("timing survives off", await rep.timing(), lambda s: "primary=" in (s or ""))
    check("preset rejected while RXPS on",
          await _with_rxps(rep, "on", "radio2 915.125,125,9,5"),
          lambda s: "radio.rxps" in (s or ""))
    check("RXPS back off", await rep.cmd("set radio.rxps off"), lambda s: s.startswith("OK"))
    check("preset accepted", await rep.cmd("radio2 915.125,125,9,5"), lambda s: s.startswith("OK"))
    check("overlap refused", await rep.cmd("radio2 910.55,62.5,7,5"), lambda s: "overlaps" in (s or ""))
    check("near pair accepted", await rep.cmd("radio2 910.6,62.5,7,5"), lambda s: s.startswith("OK"))
    check("too-fast pair refused", await rep.cmd("radio2 915.125,250,5,5"),
          lambda s: "cannot be scanned" in (s or ""))
    check("bad tuple refused", await rep.cmd("radio2 915.125,125,9"), lambda s: "must be" in (s or ""))
    check("bad policy refused", await rep.cmd("radio2 policy nope"), lambda s: "must be" in (s or ""))
    check("bad advert refused", await rep.cmd("radio2 advert nope"), lambda s: "must be" in (s or ""))
    check("policy one word", await rep.cmd("radio2 policy both"), lambda s: "both" in (s or ""))
    check("policy two words", await rep.cmd("radio2 policy same other"),
          lambda s: "secondary=other" in (s or ""))
    check("advert both", await rep.cmd("radio2 advert both"), lambda s: s.startswith("OK"))
    await rep.cmd("radio2 915.125,125,9,5")
    await asyncio.sleep(3)
    check("running after apply", await rep.status(), lambda s: "at=primary" in (s or ""))
    check("timing names the pair", await rep.timing(), lambda s: "secondary=" in (s or ""))
    # tempradio pauses radio2 and refuses changes; same frequency keeps the
    # command channel reachable while paused.
    check("tempradio accepted", await rep.cmd("tempradio 910.525,62.5,7,5,1"),
          lambda s: s.startswith("OK"))
    await asyncio.sleep(1)
    check("paused status", await rep.radio2(), "off; paused")
    check("off refused while paused", await rep.cmd("radio2 off"), lambda s: "tempradio active" in (s or ""))
    check("preset refused while paused",
          await rep.cmd("radio2 915.125,125,9,5"), lambda s: "tempradio active" in (s or ""))
    print("      waiting 65 s for tempradio to revert...")
    await asyncio.sleep(65)
    check("resumes after tempradio", await rep.radio2(), lambda s: s.startswith("on;"))


async def _with_rxps(rep, value, command):
    await rep.cmd(f"set radio.rxps {value}")
    try:
        return await rep.cmd(command)
    finally:
        await rep.cmd("set radio.rxps off")


async def section_primary(a, b, rep):
    await add_contacts(a, b)
    ia = await a.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 2)
    ib = await b.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 1)
    await ensure_layout(a, b, rep, SECONDARY, policy="same same", advert="primary")
    await rep.cmd("radio2 915.125,125,9,5")
    await asyncio.sleep(3)

    # primary=same -> repeats only on primary
    s0 = await status(rep)
    tok = f"PS{int(time.time())}"
    for i in range(8):
        await b.send_chan(ib, f"{tok}-{i}")
        await asyncio.sleep(0.9)
    await asyncio.sleep(3)
    s1 = await status(rep)
    got = await drain_tokens(a, tok, 2, ia)
    check("primary=same TX stays primary", delta(s0, s1, "tx"), lambda d: d[0] >= 3 and d[1] == 0)
    check("primary=same not seen on secondary", got, [])

    # primary=other -> crosses to secondary
    await rep.cmd("radio2 policy other same")
    await asyncio.sleep(1)
    s0 = await status(rep)
    tok = f"PO{int(time.time())}"
    for i in range(8):
        await b.send_chan(ib, f"{tok}-{i}")
        await asyncio.sleep(0.9)
    await asyncio.sleep(3)
    s1 = await status(rep)
    got = await drain_tokens(a, tok, 2, ia)
    check("primary=other TX goes secondary", delta(s0, s1, "tx"), lambda d: d[1] >= 3)
    check("primary=other seen on secondary", got, lambda g: len(g) >= 3)

    # primary=both -> one copy each way
    await rep.cmd("radio2 policy both same")
    await asyncio.sleep(1)
    s0 = await status(rep)
    tok = f"PB{int(time.time())}"
    for i in range(6):
        await b.send_chan(ib, f"{tok}-{i}")
        await asyncio.sleep(0.9)
    await asyncio.sleep(3)
    s1 = await status(rep)
    got = await drain_tokens(a, tok, 2, ia)
    check("primary=both duplicates", delta(s0, s1, "both"), lambda d: d >= 3)
    check("primary=both seen on secondary", got, lambda g: len(g) >= 3)

    # primary=drop -> no repeat at all
    await rep.cmd("radio2 policy drop same")
    await asyncio.sleep(1)
    s0 = await status(rep)
    for i in range(6):
        await b.send_chan(ib, f"PD{int(time.time())}-{i}")
        await asyncio.sleep(0.9)
    await asyncio.sleep(3)
    s1 = await status(rep)
    check("primary=drop sends nothing on primary", delta(s0, s1, "tx")[0], lambda d: d <= 1)

    # non-crossable: a direct message to a secondary-preset peer is never sent
    # to the secondary, so that peer never decrypts it.
    await rep.cmd("radio2 policy other same")
    await asyncio.sleep(1)
    nc3 = None
    for pk, v in (await b.get_contacts()).items():
        if pk.startswith(a.mc.self_info["public_key"][:8]):
            nc3 = v
    check("nc3 contact present", nc3, lambda x: x is not None)
    tok = f"DM{int(time.time())}"
    for i in range(6):
        await b.mc.commands.send_msg(nc3, f"{tok}-{i}")
        await asyncio.sleep(1.0)
    got = await drain_contacts(a, tok, 3)
    check("direct message never crosses", got, [])


async def section_secondary(a, b, rep):
    ia = await a.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 2)
    ib = await b.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 1)
    await a.set_radio(SECONDARY)
    await b.set_radio(PRIMARY)
    await rep.cmd("radio2 915.125,125,9,5")
    await asyncio.sleep(2)
    await rep.cmd("radio2 policy same same")
    await asyncio.sleep(1)

    # secondary=same: in idle windows the node repeats on the secondary and the
    # primary receiver must not see it.
    s0 = await status(rep)
    tok = f"SS{int(time.time())}"
    t0 = time.time()
    i = 0
    while time.time() - t0 < 120:
        await a.send_chan(ia, f"{tok}-{i}")
        i += 1
        await asyncio.sleep(1.0)
    await asyncio.sleep(4)
    s1 = await status(rep)
    rx = delta(s0, s1, "rx")
    tx = delta(s0, s1, "tx")
    got = await drain_tokens(b, tok, 2, ib)
    check("secondary traffic is received in idle windows", rx[1], lambda n: n > 0)
    check("secondary=same repeats on secondary", tx[1], lambda n: n > 0)
    check("secondary=same not seen on primary", got, [])

    await rep.cmd("radio2 policy same other")
    await asyncio.sleep(1)
    s0 = await status(rep)
    tok = f"SO{int(time.time())}"
    t0 = time.time()
    i = 0
    while time.time() - t0 < 120:
        await a.send_chan(ia, f"{tok}-{i}")
        i += 1
        await asyncio.sleep(1.0)
    await asyncio.sleep(4)
    s1 = await status(rep)
    got = await drain_tokens(b, tok, 3, ib)
    check("secondary=other receives", delta(s0, s1, "rx")[1], lambda n: n > 0)
    check("secondary=other crosses to primary", got, lambda g: len(g) > 0)


async def section_active(a, b, rep):
    """The regression the scheduler fix targets: secondary packets must be
    received while a busy primary keeps the node in active mode."""
    ia = await a.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 2)
    ib = await b.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 1)
    await a.set_radio(SECONDARY)
    await b.set_radio(PRIMARY)
    await rep.cmd("radio2 915.125,125,9,5")
    await asyncio.sleep(2)
    await rep.cmd("radio2 policy same other")
    await asyncio.sleep(1)
    s0 = await status(rep)
    token = f"AC{int(time.time())}"

    async def primary_load():
        for i in range(20):
            await b.send_chan(ib, f"LOAD{int(time.time())}-{i}")
            await asyncio.sleep(1.5)

    async def secondary_load():
        for i in range(20):
            await a.send_chan(ia, f"{token}-{i}")
            await asyncio.sleep(1.0)

    await asyncio.gather(primary_load(), secondary_load())
    await asyncio.sleep(3)
    s1 = await status(rep)
    got = await drain_tokens(b, token, 3, ib)
    check("mode is active during the run", s1.get("mode"), "active")
    check("secondary received while active", delta(s0, s1, "rx")[1], lambda n: n > 0)
    check("secondary crosses while active", got, lambda g: len(g) > 0)
    check("started secondary packets are not aborted", delta(s0, s1, "abrt"), lambda n: n <= 1)


async def section_filter(a, b, rep):
    ia = await a.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 2)
    ib = await b.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 1)
    await a.set_radio(SECONDARY)
    await b.set_radio(PRIMARY)
    await rep.cmd("radio2 915.125,125,9,5")
    await asyncio.sleep(2)
    await rep.cmd("radio2 policy same same")
    await rep.cmd(f"filter chan add {LAB_CHAN_NAME} {LAB_CHAN_PSK.hex()}")
    try:
        await rep.cmd(f"filter add chan={LAB_CHAN_NAME} action=drop")
        check("filter rule listed", await rep.cmd("filter list"), lambda s: f"chan={LAB_CHAN_NAME}" in (s or ""))
        for i in range(6):
            await b.send_chan(ib, f"FD{int(time.time())}-{i}")
            await asyncio.sleep(0.9)
        await asyncio.sleep(3)
        stats = await rep.cmd("filter stats")
        check("filter counted the drops", stats,
              lambda s: "hits: 0:" in (s or "") and "hits: 0:0" not in (s or ""))
    finally:
        await rep.cmd("filter clear")
        await rep.cmd(f"filter chan del {LAB_CHAN_NAME}")


async def section_observer(a, b, rep):
    ib = await b.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 1)
    await b.set_radio(PRIMARY)
    await rep.cmd("radio2 915.125,125,9,5")
    await asyncio.sleep(2)
    await rep.cmd("radio2 policy same same")
    before = await rep.cmd("get mqtt.status")
    for i in range(5):
        await b.send_chan(ib, f"OB{int(time.time())}-{i}")
        await asyncio.sleep(0.9)
    await asyncio.sleep(3)
    after = await rep.cmd("get mqtt.status")
    check("observer captured primary traffic", before, lambda s: s is not None)
    check("observer rx advanced", after, lambda s: s != before)
    reply = await rep.cmd("start ota", timeout=20)
    check("start ota reports a URL", reply, lambda s: "http://" in (s or ""))
    ip = (reply or "").split("http://")[-1].split("/")[0]
    print(f"      OTA page: http://{ip}/update")
    check("OTA page answers", _http_ok(f"http://{ip}/update"), True)
    await rep.cmd("reboot", timeout=4, retries=0)
    await asyncio.sleep(10)
    for _ in range(8):
        rep.logged_in = False
        if await rep.login():
            break
        await asyncio.sleep(4)
    check("radio2 survives reboot", await rep.radio2(), lambda s: s.startswith("on;"))


def _http_ok(url):
    import subprocess
    r = subprocess.run(["bash", "-lc", f"curl -s --max-time 8 -o /dev/null -w '%{{http_code}}' {url}"],
                       capture_output=True, text=True)
    return r.stdout.strip() == "200"


async def section_persistence(a, b, rep):
    print("      setting config and rebooting")
    await rep.cmd("radio2 915.125,125,9,5")
    await rep.cmd("radio2 policy both other")
    await rep.cmd("radio2 advert both")
    await asyncio.sleep(3)
    await rep.cmd("reboot", timeout=4, retries=0)
    await asyncio.sleep(10)
    for _ in range(8):
        rep.logged_in = False
        if await rep.login():
            break
        await asyncio.sleep(4)
    check("preset survives reboot", await rep.radio2(), lambda s: s.startswith("on;"))
    check("policy survives reboot", await rep.cmd("radio2 policy"),
          lambda s: "primary=both" in (s or "") and "secondary=other" in (s or ""))
    check("advert survives reboot", await rep.cmd("radio2 advert"), lambda s: "both" in (s or ""))
    # put live back in a known state
    await rep.cmd("radio2 policy same same")
    await rep.cmd("radio2 advert primary")


async def section_inverted(a, b, rep):
    """Quiet primary => permanent idle; deterministic secondary-origin matrix.

    The repeater's primary preset is moved to QUIET_PRIMARY and its radio2
    preset to the live mesh, so a receiver on QUIET_PRIMARY observes every
    cross-preset copy. Live primary is restored at the end.
    """
    ia = await a.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 2)
    ib = await b.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 1)
    print("      moving repeater primary to quiet preset")
    await rep.cmd(f"set radio {QUIET_PRIMARY[0]},{QUIET_PRIMARY[1]},{QUIET_PRIMARY[2]},{QUIET_PRIMARY[3]}")
    await rep.cmd("reboot", timeout=4, retries=0)
    await asyncio.sleep(1)
    await b.set_radio(QUIET_PRIMARY)   # bridge stays reachable
    await asyncio.sleep(10)
    for _ in range(8):
        rep.logged_in = False
        if await rep.login():
            break
        await asyncio.sleep(4)
    await rep.cmd("radio2 910.525,62.5,7,8")
    await a.set_radio(PRIMARY)          # live mesh = secondary
    await asyncio.sleep(2)
    check("quiet primary starts radio2", await rep.radio2(), lambda s: s.startswith("on;"))

    async def matrix(pol):
        async def attempt():
            await rep.cmd(f"radio2 policy {pol} {pol}")
            s0 = await status(rep)
            # The policy command and status query are primary packets; wait out
            # the 30 s active hold so the scheduler visits the secondary.
            await asyncio.sleep(34)
            token = f"IV{pol[:2].upper()}{int(time.time())}"
            for i in range(8):
                await a.send_chan(ia, f"{token}-{i}")
                await asyncio.sleep(0.7)
            await asyncio.sleep(3)
            s1 = await status(rep)
            got = await drain_tokens(b, token, 3, ib)
            return delta(s0, s1, "tx"), got

        tx, got = await attempt()
        # One RF-lost batch should not fail the run; a real regression fails
        # both attempts.
        need_retry = (pol in ("other", "both") and not got) or \
                     (pol == "same" and tx[1] == 0)
        if need_retry:
            print(f"      {pol}: no traffic in the first window, retrying")
            tx, got = await attempt()
        return tx, got

    tx, got = await matrix("same")
    check("inverted secondary=same repeats on secondary", tx[1], lambda n: n >= 4)
    check("inverted secondary=same does not cross", got, [])
    tx, got = await matrix("other")
    check("inverted secondary=other crosses", got, lambda g: len(g) >= 1)
    tx, got = await matrix("both")
    check("inverted secondary=both duplicates", tx[0], lambda n: n >= 4)
    check("inverted secondary=both crosses", got, lambda g: len(g) >= 1)
    tx, got = await matrix("drop")
    # The status query that ends the window is itself a primary TX (and a
    # retried CLI reply can add one more); no *data* may be transmitted.
    check("inverted secondary=drop sends no data", tx[0] + tx[1], lambda n: n <= 2)
    check("inverted secondary=drop does not cross", got, [])

    # restore live primary; radio2's preset must not overlap it
    print("      restoring live primary")
    await rep.cmd("radio2 915.125,125,9,5")
    await asyncio.sleep(3)
    await rep.cmd(f"set radio {PRIMARY[0]},{PRIMARY[1]},{PRIMARY[2]},{PRIMARY[3]}")
    await rep.cmd("reboot", timeout=4, retries=0)
    await asyncio.sleep(1)
    await b.set_radio(PRIMARY)
    await asyncio.sleep(10)
    for _ in range(8):
        rep.logged_in = False
        if await rep.login():
            break
        await asyncio.sleep(4)
    check("live primary restored", await rep.cmd("get radio"), lambda s: "910.525" in (s or ""))
    check("radio2 back on live primary", await rep.radio2(), lambda s: s.startswith("on;"))


SECTIONS = {
    "cli": section_cli,
    "primary": section_primary,
    "secondary": section_secondary,
    "active": section_active,
    "filter": section_filter,
    "observer": section_observer,
    "persistence": section_persistence,
    "inverted": section_inverted,
}


async def restore(a, b, rep):
    """Best-effort return to the bench baseline: live radios, radio2 off,
    RXPS back on at the level the bench used before these tests."""
    try:
        await rep.cmd("radio2 policy same same")
        await rep.cmd("radio2 advert primary")
        await rep.cmd("radio2 off")
        await asyncio.sleep(3)   # let the deferred apply clear the radio2 gate
        await rep.cmd("set radio.rxps level 6 preamble 16")
    except Exception:
        pass
    try:
        await a.set_radio(PRIMARY)
        await b.set_radio(PRIMARY)
    except Exception:
        pass
    for c in (a, b):
        idx = c.channels.get(LAB_CHAN_NAME)
        if idx is not None:
            try:
                await c.remove_channel(idx)
            except Exception:
                pass


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--section", default="all",
                    choices=["all"] + list(SECTIONS))
    args = ap.parse_args()

    a = await Companion("nc3", XIAO_A).connect()
    b = await Companion("nc4", XIAO_B).connect()
    try:
        rep = Repeater(b)
        if not await rep.login():
            print("login failed"); return 1
        # start from a known state: private channel only, live radio
        ia = await a.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 2)
        ib = await b.ensure_channel(LAB_CHAN_NAME, LAB_CHAN_PSK, 1)
        await a.set_radio(SECONDARY)
        await b.set_radio(PRIMARY)
        await rep.cmd("set radio.rxps off")
        await asyncio.sleep(1)
        names = list(SECTIONS) if args.section == "all" else [args.section]
        for name in names:
            print(f"\n######## {name}")
            await SECTIONS[name](a, b, rep)
            print(f"== {name}: {len([1 for _ in PASS])} passed so far, {len(FAIL)} failed")
        await restore(a, b, rep)
    finally:
        await a.close()
        await b.close()
    print(f"\nTOTAL: {len(PASS)} passed, {len(FAIL)} failed")
    for d, g in FAIL:
        print(f"  FAIL: {d} -> {g!r}")
    return 0 if not FAIL else 1


# harness's check() appends to these lists
from harness import PASS, FAIL  # noqa: E402

if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
