# Audio console test checklist

Short pass/fail check for the console audio path
(`runtime/native/audio_ps5_driver.cpp` + `audio_ps5.cpp`). It needs the real
guest, so leave `--frames` and `--pipeline-test` alone for this: those never
start audio because no render client registers.

## Before booting

- Own dump in `/data/homebrew/PPSA99301/Darkness/default.xex`.
- TV / soundbar volume up, console output set to the main port (the title opens
  the system user's main port, not a headset port).
- Keep the title's stdout/stderr visible (klog, netcat, or however you read the
  payload's log). Every audio line starts with `[Audio]`.

## What a good boot looks like

Within a second of the guest starting, two lines:

```
[Audio] SceAudioOut port N: 48000 Hz, 2 ch, 256-frame grains
[Audio] render client registered (console) callback=0x8....... token=0x4440....
```

The port number is whatever the firmware assigns. The token starts at
`0x44400001` and increments by one per registration in that run — a changed
token is normal, a missing second line is not. Ten seconds later the first
`[Audio] health:` line should appear, and one every ten seconds after that
while audio runs.

## What to listen for

1. **Menus.** Move the selection: one click/tick per move, menu music or
   ambience audible and centred.
2. **Intro / any movie.** Speech in the centre, music on both sides, no buzz,
   no chipmunk/slow-motion pitch (that would mean the rate is wrong).
3. **In game.** Turn the camera: sounds should stay placed left/right as they
   pass, not collapse to one side. Footsteps, weapons and voices should not
   sound thin.
4. **Pause, then resume.** Audio stops and comes back; it does not stay silent
   for the rest of the session.
5. **A level load or cutscene, then back.** No permanent silence afterwards and
   no growing delay between an on-screen action and its sound.

## Counters

While audio is playing the driver prints one line every 10 seconds:

```
[Audio] health: callbacks=1875 starved=0 dropped=0 queued=0
```

- `callbacks` should be about **187 per second** of registered audio
  (48,000 ÷ 256), even in a quiet scene: the driver asks the guest for a grain
  whether or not the mix is silent. Each health line should add ~1,875 to the
  previous one. A count that stops rising while the game still runs is the
  important failure.
- `starved` should be small next to `callbacks` — a handful around a level load
  is normal, a large fraction means the guest is not keeping up.
- `dropped` should be 0 (or a few) unless the scene is spamming submits.
- `queued` is 0–4. A steady 4 means the guest is ahead and the driver is
  holding it back, which is fine; it should not stay there while sounds are
  late.

Quitting still prints the final totals, if you want them:

```
[Audio] render client unregistered token=0x4440.... (callbacks=N starved=N dropped=N)
[Audio] SceAudioOut stopped
```

## If it fails

| Line | Meaning |
| --- | --- |
| `SceAudioOut init failed (0x........)` | Audio service refused init. Reboot the console; if it repeats, note the code. |
| `SceAudioOut open failed (0x........)` | Port or format refused — another title may still hold the output. Close it and retry. |
| `no console output; the game will be silent` | Same failure as above, but the game keeps running. |
| `SceAudioOut output failed` | The port died mid-run; audio stops, video continues, and the health line stops with it. Note what was playing and the code. |
| No `[Audio]` lines at all | The guest never registered its render client. Look at the last `[Kernel]` / `[UNSUPPORTED]` lines instead. |
| Sound but heavy distortion | Note whether it is bass-heavy (expected: LFE is dropped in the stereo fold) or crackling (check `starved`). |

Not covered here, because they are not wired: rumble and per-category volume.
