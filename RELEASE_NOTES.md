## Ham mode (opt-in)

A new `ham` feature gives licensed amateur operators two transmit powers:
`ham tx` for traffic anyone can read and `set tx` for encrypted traffic. The
repeater chooses the power per packet just before transmission. Packet contents
and on-air formats are unchanged, so stock nodes and apps see nothing different.

- Adverts, ACKs, traces, control packets and group traffic that verifies against
  the Public channel or one of your hashtag channels use `ham tx`. Direct
  traffic, returned paths, raw custom packets and unmatched group traffic stay
  at `set tx`.
- The callsign gate requires `ham on`, a configured callsign and that callsign
  as a whole word in the live node name. A 10-minute station-ID advert is
  scheduled on activation and every 10 minutes after that, carrying the call
  sign in the node name.
- Config persists in `/ham_cfg`. Ham mode is off by default, even after a
  config load failure, and is fully decoupled from the packet filter.

You must hold an amateur radio license and choose both power levels yourself
for your board and antenna. This fork enforces no power ceiling and claims no
compliance. See [HAM.md](./HAM.md) before enabling it.

## Documentation

- Added the HAM.md user guide and updated the README.
- Removed em dashes from the fork user guides.

## Release process

- Dev release notes now come from this file. Stable releases collect the notes
  from the dev releases since the previous stable release.
- Removed unused manual inputs from the release workflows.
