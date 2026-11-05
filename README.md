# Velocity Scale

A MIDI FX for [Schwung](https://github.com/charlesvestal/schwung): rescales,
curves and monitors incoming note velocities.

## Why

When you play a sound generator from a physical keyboard, how hard you hit the
keys and how the keyboard turns that into MIDI velocity together decide which
velocities actually arrive. Often that range is narrow or lopsided: the softest
note you can comfortably play may already arrive at 40, or no hit ever goes
above 100. The part of the range you can't reach is part of the sound you can't
play.

Velocity-sensitive sound generators change more than volume with velocity:
brightness, attack, layer and sample switching. To get all of that, Velocity
Scale stretches the range your keyboard really sends (**Input Min / Input
Max**) over the full output range, shapes it with a curve, and lets you see the
incoming velocities so you can set the input range from what you measure
instead of guessing.

## Parameters

Velocity goes through three stages in this order:

1. **Input Min / Input Max** (1–127): the range your keyboard really sends. It
   is stretched to cover the whole range. Hits below Input Min count as Input
   Min, and hits above Input Max count as Input Max. If both are the same value,
   that value acts as a threshold: softer hits give Min, the rest give Max.
2. **Curve** (−100 to +100, 0 = linear): positive values make soft hits louder,
   so it is easier to play loud. Negative values need harder hits to get loud.
   The ends of the range never move.
   **Curve Preset** sets Curve to a named value: Linear (0), Soft (+40),
   Softer (+70), Hard (−40), Harder (−70).
3. **Min / Max** (1–127): the output range the result is scaled into.

Note-off (including note-on with velocity 0) is never changed. With the
defaults (input 1–127, Curve 0, output 1–127) every velocity comes out
unchanged.

Knobs on the main page: 1 Min, 2 Max, 3 Curve, 4 Input Min, 5 Input Max.

## Monitoring incoming velocities

The **Monitor** page, linked at the end of the main page, shows live velocity
readouts:

| Knob | Readout | Meaning |
|---|---|---|
| 1 | In | velocity of the last note from the keyboard |
| 2 | In Lo | lowest incoming velocity since the last reset |
| 3 | In Hi | highest incoming velocity since the last reset |
| 4 | Out | velocity of the last note after scaling |
| 5 | OutLo | lowest outgoing velocity since the last reset |
| 6 | OutHi | highest outgoing velocity since the last reset |
| 7 | Reset | click to clear all readouts |

The readouts show 0 until the first note. They are not saved with presets.

To calibrate a keyboard:

1. Open the Monitor page and click **Reset**.
2. Play your softest and your hardest notes several times.
3. Copy **In Lo** to **Input Min** and **In Hi** to **Input Max**.
4. Play again and check that **OutLo / OutHi** now cover the output range you
   want. Then set **Curve** to taste.

## Why it is not bundled any more

It shipped inside the host up to v0.2.0. It is a catalog module from v0.3.0, so
it can be updated without a host release — and so the host tarball carries only
what a device genuinely cannot start without.

Installing it over a bundled copy is an UPDATE, not a downgrade: 0.3.0 is
deliberately above the 0.2.0 that shipped in the bundle. v0.4.0 adds the input
range and the Monitor page.

## Build

    ./scripts/build.sh    # -> dist/velocity_scale-module.tar.gz

Needs Docker; it cross-compiles for the Move (ARM64).
