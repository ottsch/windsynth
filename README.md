# windsynth

Simple monophonic wind synth for Daisy Seed.

## Setup

### 1. Install the development tools

Install Git, GNU Make, the GNU Arm Embedded toolchain, and `dfu-util`. The
compiler tools (`arm-none-eabi-gcc`, `arm-none-eabi-g++`, and related programs)
must be available on your `PATH`. The official
[Daisy development-environment guide](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment)
has instructions for Linux, macOS, and Windows.

Check the two device-specific tools before continuing:

```sh
arm-none-eabi-g++ --version
dfu-util --version
```

### 2. Clone the project

Clone recursively so that the pinned libDaisy, DaisySP, and DaisySP-LGPL
revisions are checked out with the project:

```sh
git clone --recurse-submodules <repository-url>
cd windsynth
```

If the repository was cloned without `--recurse-submodules`, repair the
checkout with:

```sh
git submodule update --init --recursive
```

### 3. Assemble the hardware

Disconnect power before wiring the Seed, then make the MIDI, OLED, encoder,
and audio connections listed under [Hardware and controls](#hardware-and-controls).
Connect the Seed's audio outputs to a line-level input and begin with the
monitor or mixer volume low. Use a data-capable Micro-USB cable for programming;
charge-only cables will power the Seed but cannot flash it.

### 4. Build the firmware

Build the dependencies once, then build windsynth:

```sh
make deps -j
make -j
```

The resulting firmware is `build/windsynth.bin`. Run `make clean` before a full
rebuild if the toolchain or build configuration changes.

`ReverbSc` comes from the LGPL-2.1 DaisySP-LGPL submodule; see that submodule's
README for binary distribution and relinking requirements.

### 5. Flash the Daisy Seed

This project runs directly from the Seed's internal flash and does not require
installing the optional Daisy bootloader.

1. Connect the Seed's Micro-USB port to the computer.
2. Press and hold **BOOT**.
3. Press and release **RESET** while continuing to hold **BOOT**.
4. Release **BOOT**. The Seed is now in its built-in ROM DFU mode.
5. From the repository root, run:

   ```sh
   make program-dfu
   ```

The Seed leaves DFU mode and starts the firmware after a successful flash. You
can also upload `build/windsynth.bin` with the official
[Daisy Web Programmer](https://flash.daisy.audio/) in its built-in DFU mode.

### 6. First run

After reset, the OLED should show the current preset. Send a MIDI Note On and
raise MIDI CC2 (breath); a note by itself is intentionally silent while breath
is at zero. Start with CC1 at zero for the basic voice, then raise it to add
growl and, near the top of its range, the overtone sequence.

If flashing reports that no DFU device is present, repeat the BOOT/RESET
sequence, verify that the USB cable carries data, and check the operating
system's DFU/USB permissions. If the build cannot find a library, run the
recursive submodule-update command from step 2.

## Hardware and controls

- MIDI input is UART MIDI from a WIDI Core breakout board.
- Connect the WIDI Core breakout TX signal to Daisy Seed D14.
- Connect WIDI Core breakout ground to Daisy Seed ground.
- TX from Daisy Seed is not used in this iteration.
- A 128x64 SSD1306/SSD1309 4-wire SPI OLED displays note and breath status.
- OLED SCK/CLK connects to D8, SDA/MOSI to D10, CS to D7, DC to D9, and RES/RST to D30.
- OLED VCC connects to Daisy Seed 3V3 and GND connects to Daisy Seed GND.
- A rotary encoder selects between four named presets. Click its button to
  cycle through preset selection, reverb mix, and reverb time, then turn to
  adjust the active value. Connect A to D0, B to D1, common to ground, and its
  push switch between D2 and ground. The inputs use libDaisy's internal pull-ups.
- The preset bank contains Dark Reed, Soft Flute, Bright Brass, and Hollow Clar;
  the active preset number and name appear at the top of the OLED.
- The last selected preset, reverb mix, and reverb time are restored after
  power-off. Settings are written to the final 4 KB sector of QSPI flash after
  the encoder has been idle for 10 seconds, limiting flash wear. Keep
  `0x907ff000` reserved if adding other QSPI data later.
- MIDI Note On sets the saw oscillator pitch.
- MIDI pitch bend adjusts pitch directly over the standard +/-2 semitone range.
- MIDI CC1 adds a smoothed 30 Hz growl, progressively modulating amplitude and
  filter cutoff while adding a little saturation and filter drive. In roughly
  the top 30% of its range, it also introduces a sine partial that steps through
  harmonics 2-8 while retaining the fundamental; partials above the safe audio
  range are automatically limited.
- MIDI CC5 controls legato glide time with a curved response; non-legato notes jump immediately.
- MIDI CC2 acts as breath control, opening the low-pass filter and raising the output level.
- Filter cutoff uses an exponential breath curve with light key tracking, so higher notes stay brighter.
- Pitch starts slightly flat at breath onset and settles as breath rises.
- A quiet breath-shaped noise layer has its own airy high-pass path for articulation.
- A pulse-forward source gives a darker odd-harmonic reed tone, with saw reduced to a supporting layer.
- A tiny breath-scaled PWM movement keeps the pulse source from sounding static.
- A triangle body layer fills out low-breath notes.
- Gentle soft saturation thickens the exciter before filtering.
- Stereo reverb follows the synth voice; its wet mix is adjustable from 0-100%
  and its time from 60-95%, both in 5% steps with the encoder.
- Nonlinear voice controls update at 6 kHz while oscillators, filters, and
  reverb still process every sample. A 16-sample audio block provides more
  callback headroom with only 0.33 ms of buffering at 48 kHz.
- After note-off, the reverb continues processing its complete audible tail.
  Once its output remains below -100 dBFS for half a second, audio DSP sleeps
  and the callback only writes silence. Breath on an active note wakes it on
  the next audio block.
- The OLED shows average CPU callback load and the peak since startup; a peak
  at or above 100% means the callback missed its audio deadline. Settings saves
  keep the audio DMA running, so the delayed QSPI save does not mute the output
  or cut off a reverb tail.
- After 30 seconds without notes, breath/control changes, or encoder activity,
  the OLED turns off. The next meaningful MIDI or encoder event wakes it and
  redraws the current state.
- A 2.5x master gain uses the available output range, with a final +/-1.0
  ceiling to prevent DAC overload from filter or reverb peaks.
