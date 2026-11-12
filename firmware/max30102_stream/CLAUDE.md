# AF detection firmware — Arduino Nano 33 BLE (nRF52840), Zephyr

## Project
On-device atrial fibrillation (AF) screening from PPG. A MAX30102 sensor (IR channel)
feeds a signal-processing pipeline and an int8 TensorFlow Lite Micro model.
The ML side is finished in Python (see `01_explore.ipynb`); this folder is the firmware.

Goal is industry-style firmware, NOT the Arduino framework:
- Zephyr RTOS via nRF Connect SDK, built with west/CMake/Kconfig
- Board target: `arduino_nano_33_ble`
- Hardware described in Devicetree (overlay for the MAX30102 on I2C + its INT pin)
- Own MAX30102 driver on Zephyr's I2C API (no third-party Arduino libraries)
- TensorFlow Lite Micro as a Zephyr module, with CMSIS-NN kernels
- Flashing: double-tap RESET, then `west flash` with Arduino's bossac.exe
  (`--bossac=<path> --bossac-port=COMx`). Developer is on Windows.

## The firmware MUST match the Python pipeline exactly
| Setting | Value |
|---|---|
| Sensor channel | IR, 100 Hz output (e.g. 400 Hz internal with 4-sample averaging) |
| Polarity | Invert raw MAX30102 counts (pulse must point upward) |
| DC removal | Subtract the mean of the first 1 s (100 samples) before filtering. Raw counts are ~100,000; without this the filter's start-up transient lasts longer than the 2 s skip and creates false beats |
| Filter | Butterworth band-pass 0.5–8 Hz, order 2, CAUSAL (scipy `sosfilt`, not `sosfiltfilt`); export SOS coefficients from Python |
| Window | 30 s (3000 samples), non-overlapping; discard first 2 s after start (filter settling) |
| Normalization | Per window: z-score (subtract mean, divide by std) |
| Beat detection | Peaks with min spacing 33 samples (0.33 s) and prominence ≥ 0.5 (scipy `find_peaks` semantics) |
| Valid intervals | Keep IBIs between 300 and 2000 ms; reject window if < 15 beats |
| Features (model inputs, in order) | `nrmssd` = RMSSD / mean IBI; `cv` = SD / mean IBI; `pnn_rel` = fraction of |successive diffs| > 0.065 × mean IBI; `mad_rel` = median |successive diff| / mean IBI |
| Quality gate (SQI) | Template matching: cut each beat ± half the median IBI around its peak, average into a template, SQI = mean Pearson correlation of beats with template. Fewer than 5 beats → SQI 0. Report "unreliable signal" if SQI < 0.80 |

Use `float` (nRF52840 has a single-precision FPU), never `double`.

## Model (`af_model.h`, 2,848 bytes, int8)
- Architecture: Normalization → Dense(16, ReLU) → Dense(1, sigmoid)
- Input quantization: scale = 0.003795, zero_point = −128
  int8 = clamp(round(feature / 0.003795) − 128, −128, 127)
- Output quantization: scale = 1/256, zero_point = −128
  probability = (out + 128) / 256; **AF if out ≥ 0**
- Outputs per window: "normal", "possible AF", or "unreliable signal" (SQI gate)

## Firmware architecture
1. Sensor thread: MAX30102 INT pin → read FIFO over I2C → ring buffer. Never blocks on processing.
2. Processing thread: every 30 s run filter → beats → features → SQI → inference.
3. Output: USB serial log first; BLE service later (HR, rhythm class, SQI).
4. Sleep between interrupts; measure current for energy per inference.

## Build order (do one step at a time, verify before moving on)
- F1: toolchain + blinky on the board
- F2: USB serial logging (Zephyr `cdc-acm-console` snippet)
- F3: MAX30102 driver; stream raw IR samples to PC, check in the notebook
- F4: port DSP + features + SQI to C++; verify against golden test windows exported from Python
- F5: TFLite Micro inference; measure latency and RAM
- F6: BLE output and power measurement

## Working style
The developer is learning industry embedded practice. Explain what each file and config
option does, prefer small verifiable steps, and keep code readable.
