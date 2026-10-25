# pitchride-core

The online part of [PitchRide](https://github.com/b2b-sessions/pitchride), the
app for DJing back to back with a friend over the internet.

Two computers share one clock over an ordinary home connection, and every
move one DJ makes (a track starting, a fader, an effect) is played at exactly
the right moment on the other, so both hear the same mix. On top of that:

- **Join codes.** One app asks the connection server for a short code; the
  other joins with it. The two then connect directly (hole punching), or
  through the server's relay when a direct connection isn't possible.
- **Encryption.** Everything between the two apps is encrypted end to end,
  including when it goes through the relay.
- **Track sharing.** If one DJ plays a track the other doesn't have, it's sent
  over the same connection as a reliable stream, behind the sync traffic so
  it never makes the mix late.

It knows nothing about the DJ app itself: PitchRide connects it through a thin
adapter (`src/b2b/` in the app). The core needs only C++17 and the system's
sockets; `qt/` is an optional Qt module.

## Build and test

```bash
sudo apt install build-essential cmake python3   # Debian/Ubuntu
cmake -S . -B build
cmake --build build -j
./build/b2b_tests
```

`tools/run_local_test.sh` runs two copies on one computer over a simulated bad
connection, and `tools/analyse.py` shows how closely they stayed together.

## In PitchRide

The app expects this repository at `lib/b2b-core`:

```bash
git clone https://github.com/b2b-sessions/pitchride-core.git lib/b2b-core
```

## This repository

This is the source code of each released version. We develop in private and
publish each version here when it's released. We don't take pull requests here
yet: email [pitchrideenquiries@gmail.com](mailto:pitchrideenquiries@gmail.com).

## Licence

Copyright (C) 2026 PitchRide.

Free software under the GNU General Public License, version 2 or (at your
option) any later version: see [LICENSE](LICENSE). It includes
[Monocypher](https://monocypher.org) (BSD-2-Clause / CC0) and
[KCP](https://github.com/skywind3000/kcp) (MIT) in `third_party/`.
