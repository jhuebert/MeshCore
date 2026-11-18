# Chat Bot

The chat bot is an opt-in `simple_repeater` feature that answers common `#test`
chatter from the repeater itself. It needs no companion, no internet and no
contact table: it uses only the path of the packet it just received. It is
**off by default** — a node that has not enabled it behaves exactly like stock
firmware.

The reply is an ordinary flooded group text, so every receiver on the channel
sees it. That is why there should be **one designated responder per channel**.
Only one repeater should have the bot enabled on a given key.

## What it replies to

Matching is case-insensitive and a leading `!` is ignored. Multi-word triggers
are matched as *substrings*, so the common aliases used with meshcore-bot are
caught without listing them all (`multitest`, `mtest`, `atest`, `testing 123`,
`multipath`, … all work). Single-letter aliases (`t`, `p`, `m`, `h`) must be the
whole message.

| Message contains | Reply |
|---|---|
| `test`, `path`, `mt` (or is exactly `t`, `p`, `m`) | mention line + the received path |
| `ping` | mention line only |
| `help` (or exactly `h`) | `Commands: ping, test` |
| anything else | no reply |

Note that `p` is a *path/test* trigger, not a ping alias. `ping` and `help` are
checked first, so a mixed message like `ping test` gets the ping reply.

Example replies, with `bot name TestBot` and a sender named Alice whose packet
arrived over two 2-byte path hashes:

```
TestBot: @[Alice]
1234 5438 A8B4
```

```
TestBot: @[Alice]
```

```
Commands: ping, test
```

A direct (0-hop) request prints `direct` in place of path bytes. The path bytes
are grouped by the *sender's* Path Hash Size setting — the same grouping ncbot
uses — so a 1-byte-path sender gets `1A 2B 3C` and a 2-byte-path sender gets
`1A2B 3C4D`.

There is deliberately **no time, hop count or SNR** in the reply. The repeater's
RTC is UTC (confusing on a local mesh), the hop count is just the number of path
groups you can see, and the SNR is only the last leg of the route — not an
end-to-end measurement.

The reply is scoped like the request (a scoped request gets a scoped reply), is
sent on the radio profile the request arrived on, and is delayed 1–3 s so it
does not collide with the original packet's relay burst.

## Bot etiquette

Bots share the same limited airtime as everyone else. These guidelines from the
local mesh keep it healthy:

- ✅ **Be reactive, not proactive** — only respond when messaged; no unsolicited
  broadcasts.
- ✅ **Provide real utility** — make sure it helps the wider community, not just
  a narrow case.
- ✅ **Avoid duplicates** — watch the mesh for a few days first; don't repeat
  what another bot does.
- ✅ **One bot, one channel** — run in a channel named after the bot (e.g.
  `#nmb`). `#test` is reserved and needs admin approval.
- ✅ **Prefer direct messages** — DMs use routed paths and keep mesh-wide traffic
  down.
- 🚫 **No broadcast content** — don't post forecasts or updates unprompted;
  it's noise for most of the state.
- ⚠️ **Limit internet dependence** — the mesh matters most when the internet is
  down. Prefer local data and fail gracefully.
- ⚠️ **Consider backup power** — if it could matter in an outage, run it on
  battery and solar.

This bot is reactive by construction: no matching message, no transmission. It
has no network, database or scheduled messages, so there are no unsolicited
broadcasts to configure and nothing that depends on the internet.

Replies are **flooded group texts, not DMs** — a repeater has no contact table,
and DMs to it are the admin CLI surface. The "prefer direct messages" guideline
therefore applies to other bots or services you run, not to this reply path.
The "one bot, one channel" rule is supported directly by `bot chan #<name>`,
which lets you name the channel after the bot. Using `#test` without admin
approval is discouraged here too.

## Setup

1. Set the channel — either a hashtag name (the PSK is derived from it exactly
   as `filter add chan=#name` derives it) or a raw key:

   ```
   bot chan #nmb
   bot chan 0123456789ABCDEF0123456789ABCDEF
   ```

2. Optionally give the bot a name (1–16 characters). Without one it uses the
   repeater's node name, or `bot` if the node has no name:

   ```
   bot name TestBot
   ```

3. Turn it on:

   ```
   bot on
   ```

`bot chan off` clears the key but keeps the name and the on/off setting. The
channel name is not stored — the status line shows the 1-byte channel hash.

## CLI reference

```
bot                            status line
bot on | off                   enable/disable replies (config preserved)
bot name <text> | bot name     set reply name (1..16 chars; bare = node name)
bot chan #<name>               derive the key from a hashtag channel name
bot chan <psk-hex>             set the key from 32 or 64 hex digits
bot chan off                   clear the channel (receipt disabled)
```

Status:

```
bot: on, name TestBot, chan h=1A
bot: off, name (node), chan (none)
```

Errors:

```
Err - usage: on|off|name|chan
Err - name must be 1..16 chars
Err - usage: chan #<name>|<psk-hex> | chan off
Err - chan name must be 1..15 chars
Err - psk must be 32 or 64 hex digits
```

## Rate limiting and persistence

There is a single global gate: at most one reply every 5 seconds (`BOT_MIN_INTERVAL_MS`,
a build flag). A second trigger inside that window gets silence. There are no
per-user cooldowns, no hourly cap and no rate-limit messages. A reply is roughly
50–80 bytes of text; the interval is the only bound on this node's airtime.

The config lives in `/bot_cfg` (magic `BOTC`, version 1, CRC-16, saved lazily a
few seconds after an edit). The rate timer is RAM-only: a reboot clears it, so a
trigger immediately after boot can be answered. No other state is persisted.

## Interactions

- **Battery gate** — while the repeater is suspended for low battery, the bot
  sends nothing.
- **Packet filter** — a channel the filter drops by content rule is not one the
  bot answers on.
- **Fleet** — fleet scripts and acks are ordinary chat to the bot. Do not share
  the fleet channel key with the bot: a fleet payload containing `test` or
  `path` can otherwise trigger a reply.
- **radio2** — replies go out on the profile the request arrived on.
- **MQTT observer** — replies are TX packets and appear in the feed like any
  other transmission.
- **Powersaving** — no new wake source; the bot only reacts to a packet that
  already woke the node.

## Limits

One channel, fixed reply text, flooded replies only. No DMs, no contact-name
lookups, no editable messages, no telemetry or weather commands, and no
scheduled or unsolicited messages. Sender names are unauthenticated display
labels — they are not identities.

## Testing

```
pio test -e native_bot
pio test -e native_bot_san
```

Firmware compile checks (persistence and hook changes touch every filesystem
family):

```
bash build.sh build-firmware Xiao_S3_WIO_repeater
bash build.sh build-firmware RAK_4631_repeater
bash build.sh build-firmware PicoW_repeater
bash build.sh build-firmware wio-e5_kiss_modem
```
