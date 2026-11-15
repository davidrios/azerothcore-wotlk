# mod-wowvr

Server side of the WowVR multiplayer protocol extension (v1): WowVR clients see each other's VR
bodies, including full body tracker joints (hips, chest, feet, knees, elbows). The server checks poses and relays them to the players who can see the sender; it never
interprets them. Stock 3.3.5a clients never receive any of it.

The contract is `docs/vr-multiplayer-protocol.md` in the WowVR repository.

## Requirements

The core patch that adds `CMSG/SMSG_WOWVR_HELLO`, `_STATE` and `_POSE` (`0x521`-`0x526`) to
`Opcodes.h`/`Opcodes.cpp` and raises `NUM_MSG_TYPES` to `0x530`. Without it a stock core
disconnects any client sending these opcodes.

## Install

1. Keep this directory in `modules/` and re-run CMake.
2. Copy `conf/mod_wowvr.conf.dist` to your config directory as `mod_wowvr.conf`.
3. The SQL in `data/sql/db-world/` is applied by the DB updater (AntiDOS rows and help for
   `.wowvr status`).

## Behaviour

- On login the server whispers the player an addon message from themselves: `WOWVR\t1`.
- `CMSG_WOWVR_HELLO` negotiates the version and capabilities; the answer carries the rates.
- `CMSG_WOWVR_POSE` is checked (length, part bits, `WowVR.MaxOffset`, `WowVR.MaxReceiveRate`) and
  only the newest is kept. Each VR player's pose goes out at `WowVR.SendRate` to the players who see
  it within `WowVR.PoseRange` and asked for poses, as `SMSG_WOWVR_POSE` (packed GUID + payload).
- `CMSG_WOWVR_STATE` is relayed at once as `SMSG_WOWVR_STATE`; leaving VR forgets the stored pose.
- Players coming into view get the VR player's state, and its last pose if it's newer than
  `WowVR.PoseTimeoutMs`.
- `.wowvr status` (GM) lists the negotiated sessions.
