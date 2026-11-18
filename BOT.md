# Chat Bot

**Think twice before enabling a bot.** Every reply costs airtime for everyone on
the mesh, and most channels do not need one. If you do enable it, give the bot
its own channel with no other bots on it — two responders to the same message
just add noise.

The chat bot is an opt-in `simple_repeater` feature that answers common `#test`
chatter from the repeater itself. It needs no companion, no internet and no
contact table: it uses only the path of the packet it just received. It is
**off by default** — a node that has not enabled it behaves exactly like stock
firmware.

## What it replies to

Matching is case-insensitive. Multi-word triggers are matched as *substrings*,
so common variants are caught without listing them all (`multitest`, `mtest`,
`atest`, `testing 123`, `multipath`, … all work). Single-letter triggers
(`t`, `p`, `m`, `h`) must be the whole message.

| Message contains | Reply |
|---|---|
| `test`, `path`, `mt` (or is exactly `t`, `p`, `m`) | mention line + the received path |
| `ping` | mention line only |
| `help` (or exactly `h`) | `Commands: ping, test` |
| anything else | no reply |

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
are grouped by the *sender's* Path Hash Size setting: a 1-byte-path sender gets
`1A 2B 3C` and a 2-byte-path sender gets `1A2B 3C4D`.

There is deliberately **no time, hop count or SNR** in the reply. The repeater's
RTC is UTC (confusing on a local mesh), the hop count is just the number of path
groups you can see, and the SNR is only the last leg of the route — not an
end-to-end measurement.

The reply is scoped like any other reply from this repeater: it uses the
incoming packet's region when that is known, otherwise the repeater's default
region. It goes out on the radio profile the request arrived on, delayed 1–3 s
so it does not collide with the original packet's relay burst.

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

There is a single global gate: at most one reply every 5 seconds. A second
trigger inside that window gets silence. There are no per-user cooldowns, no
hourly cap and no rate-limit messages.

The config is saved to `/bot_cfg` a few seconds after an edit. The rate timer is
RAM-only: a reboot clears it, so a trigger immediately after boot can be
answered. No other state is persisted.

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
