# The conformance suite

The suite sets out what a player of `.hok` files plays. A player conforms when its renders of the suite's files give
the hashes in `hashes.txt`.

## The files

The folder holds seven files in the text form, one for each engine and mixing mode:

| File | Engine | Mixing |
| --- | --- | --- |
| `switch-strict.hok` | the Switch | strict |
| `switch-enhanced.hok` | the Switch | enhanced |
| `wii-u-strict.hok` | the Wii U | strict |
| `wii-u-enhanced.hok` | the Wii U | enhanced |
| `3ds-strict.hok` | the 3DS | strict |
| `3ds-enhanced.hok` | the 3DS | enhanced |
| `wii-strict.hok` | the Wii | strict |

The Wii has no enhanced mixing. Each file holds one sequence sound, a bank of two instruments and a wave archive of two
looped waves. The song plays two tracks round a loop of three beats at 120 beats a minute. It sets the tracks' volumes,
pans and sends to aux bus A, and plays an LFO, a pitch bend and a release. Each file holds an effect of its console on
bus A, at the values that a new effect starts from in Hokuto. The Switch's file holds the I3DL2 reverb at nn::audio's
preset 1.

## The renders

Each line of `hashes.txt` gives a file, the index of a sequence sound in its sound table, the seconds of the render and
the render's SHA-256. The sound plays on the model of the file's engine, from the model's first frame, with the file's
effects, the volumes of the aux buses and the sound's mixing mode. The sound's variables start at their starting values.
The render lasts the seconds given, rounded up to whole frames of the model, or ends with the output.

The hash covers the render as interleaved stereo samples of 16 bits, in little-endian bytes, at the model's rate:

- 48 kHz on the Switch and the Wii U.
- 32,728 Hz on the 3DS in strict mixing, and about 48,070 Hz in enhanced mixing.
- 32 kHz on the Wii.

In strict mixing, the samples are the model's. In enhanced mixing, the model mixes in floating point, with full scale at
1.0. Each sample is then multiplied by 32,768, rounded to the nearest whole number, halves away from zero, and held to
the range of 16 bits.

## Running the suite

`ctest` runs the suite as the test `hokuto_conformance` (`tests/conformance_test.cpp`). The renders' code is in
`tests/conformance_render.h`.

`hokuto_make_conformance` makes the files again. A build with `-DHOKUTO_TOOLS=ON` makes it. Run it with a folder:
`build/hokuto_make_conformance <folder>`. It writes the files in the text form, the archive that each file holds beside
it, and `hashes.txt`. `hokuto_check_playback` checks Hokuto's player against the consoles' models on each archive, with
the file's effect and mixing mode.
