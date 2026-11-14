# Playing Connect 4 against a neural network on an Arduino

[`ConnectXNet`](ConnectXNet/ConnectXNet.ino) is a sketch that runs the network trained in this repository on a
microcontroller with **TensorFlow Lite for Microcontrollers**. You play from the Serial Monitor; the board answers
with the move chosen by the network. A session looks like this (the numbers of the last line are illustrative: they
depend on your board and settings):

```
=== New game ===
You are X (you move first).

. . . . . . .
. . . . . . .
. . . . . . .
. . . . . . .
. . . . . . .
. . . . . . .
1 2 3 4 5 6 7

Your move: 4
The computer plays column 4  (38 network evaluations, 311.7 ms)
```

> **Status.** The sketch has **not been run on real hardware yet**, and the conversion scripts that produce its model
> ([`ai/python`](../python/README.md)) have not been run with TensorFlow yet either. What *is* verified, on the PC, is
> everything the sketch is made of: the board code, the encoders and the search of `connectx_board.h` are compiled and
> tested against the C++ reference engine and the C++ encoders used for training
> ([`ai/tests/test_arduino_board.cpp`](../tests/test_arduino_board.cpp), 400k+ checks), and the Python reader and
> encoders agree with the C++ ones. The TensorFlow Lite Micro calls follow the library's own `hello_world` example.
> If something does not compile or run on your board, the places to look are listed in *Troubleshooting*.

## What you need

* A board supported by the [Chirale_TensorFlowLite](https://github.com/spaziochirale/Chirale_TensorFlowLite)
  library: **Arduino Nano 33 BLE / Sense** (the reference), Nano ESP32, Nicla, Portenta, Giga R1. 256 KB of RAM and
  1 MB of flash are plenty. (The official `Arduino_TensorFlowLite` library was archived in 2025 and is no longer in
  the Library Manager; Chirale's is a maintained port of the current TensorFlow Lite Micro.)
* Arduino IDE ≥ 2: install the board package of your board and, from *Library Manager*, `Chirale_TensorFlowLite`.
* A model in `model_data.h`. The folder only contains a placeholder (compiling it stops with a message): the
  conversion needs TensorFlow, so the header is not generated in advance. To make it, from the pretrained network
  or from your own:

```bash
cd ai/python
python export_tflite.py ../models/connectx_6x7.cxnn --out-dir ../models
cp ../models/connectx_6x7_int8.h ../arduino/ConnectXNet/model_data.h
```

Then open `ConnectXNet/ConnectXNet.ino`, select the board and the port, upload, open the Serial Monitor at
**115200 baud** and type the number of a column (1-7). `n` starts a new game.

## How the computer plays

The strength comes from *how the network is used*; the knob is `SEARCH_DEPTH` at the top of the sketch.

| `SEARCH_DEPTH` | The computer... | Network evaluations per move | Strength of this mode on the PC (C++ agent with the same logic)* |
|---|---|---|---|
| `0` | asks the policy head for a score per column and plays the best free one (with `USE_TACTICS_SHIELD`: wins/blocks first) | 1 | Elo 921: like an alpha-beta of depth 4 (949) |
| `1` | plays the move that leaves the position the value head likes most | up to 7 | Elo 890: no better than `0` |
| `2` (default) | alpha-beta search of 2 plies, the value head evaluates the leaves | up to 49 | Elo 1090: as strong as an alpha-beta of depth 8 (1079) |
| `3` | the same, deeper | up to 343 (alpha-beta prunes many) | Elo 1116 |
| `4` | the same, deeper | up to 2401 | beats `3` in 76% of the games and the depth-8 alpha-beta in 62% (60-game matches) |

\* Elo of the tournament table of the [AI guide](../README.md#how-strong-are-the-agents) (1000 = the average of that
field of 15 agents, 40 games per pairing); `4` was measured in a separate small match.

Why not simply trust the policy head? Because a small network trained on a limited number of examples is far
better at *judging* a position (the value head) than at *proposing* the best move; and the classic search is
exactly the tool that turns a mediocre judge into a good player by looking a few moves ahead (a single ply is not
enough: the gain starts at two). Wins and forced
moves are always found exactly, without the network (`connectx::negamax`), so the network is only asked what it is
good at. The sketch is therefore the same alpha-beta of the `alphabeta` agent with the network as evaluation
function: the agent `nnsearch` of the C++ code.

## Memory and speed

* **Flash**: the model, ~1 byte per parameter (the default network has ~122 000 parameters: ~125 KB), plus the
  TensorFlow Lite Micro library.
* **RAM**: the *tensor arena* (`kTensorArenaSize`, 24 KB by default) holds the activations; the sketch prints the
  bytes actually used at start-up. Increase it if `AllocateTensors()` fails.
* **Time**: one network evaluation is ~122 000 integer multiply-accumulates. Every move prints the number of
  evaluations and the time it took, so you can pick the deepest `SEARCH_DEPTH` you find comfortable.
  A Cortex-M4 at 64 MHz with the CMSIS-NN kernels should do a few milliseconds per evaluation (not measured).
* A smaller network (`connectx-train supervised --hidden 64,64 ...`, 18 000 parameters) is ~7 times cheaper and a
  little weaker: use it on boards with little memory or to search deeper.

## Settings in the sketch

| Setting | Meaning |
|---|---|
| `SEARCH_DEPTH` | plies of look-ahead (0 = policy head only) |
| `USE_TACTICS_SHIELD` | (depth 0) always take a win / block a loss |
| `USE_MIRROR_AVERAGING` | evaluate every position and its mirror image and average: a bit stronger, twice as slow |
| `COMPUTER_STARTS` | who moves first |
| `kTensorArenaSize` | memory for TensorFlow Lite |

## How it is built

* [`connectx_board.h`](ConnectXNet/connectx_board.h): dependency-free C++11 shared with the PC tests: the game, the
  three input encoders (`raw`, `tactical`, `windows`) and the search. `CONNECTX_ROWS/COLS/CONNECT` (written by the
  exporter into `model_data.h`) select the board; the default is the standard 6×7.
* [`ConnectXNet.ino`](ConnectXNet/ConnectXNet.ino): TensorFlow Lite Micro set-up (a 4-operator resolver: fully
  connected, tanh, quantize, dequantize), int8 quantization of the input and dequantization of the outputs using the
  scales stored in the model, the search and the Serial interface. The two outputs of the model are told apart by
  their size (7 scores vs 1 value), so the order the converter gives them does not matter.

## Using the older `Arduino_TensorFlowLite` library

Replace `#include <Chirale_TensorFlowLite.h>` with `#include <TensorFlowLite.h>` and give the interpreter an error
reporter, as the library's examples do:

```cpp
#include "tensorflow/lite/micro/micro_error_reporter.h"
static tflite::MicroErrorReporter micro_error_reporter;
static tflite::MicroInterpreter staticInterpreter(model, resolver, tensor_arena, kTensorArenaSize, &micro_error_reporter);
```

## Troubleshooting

* *`AllocateTensors() failed`*: raise `kTensorArenaSize`, or use a smaller network.
* *"schema version does not match"*: the TensorFlow version that wrote the `.tflite` is newer than the library's
  flatbuffer schema; install the latest `Chirale_TensorFlowLite` or export with the TensorFlow version in
  `requirements.txt`.
* *Compile error about a missing op (`AddTanh`, `AddQuantize`...)*: your library version names the operators
  differently; replace the `MicroMutableOpResolver` with `tflite::AllOpsResolver resolver;` (bigger, but it needs no list).
* *The computer plays badly or illegally*: the board constants in `model_data.h` must match the network (the
  exporter writes them), and the input encoding of the model must be the one the sketch is compiled with. Evaluate
  the same `.tflite` file on the PC with `ai/python/eval_model.py --tflite`: if it is strong there, the problem is in
  the sketch; if not, in the model.
* To test the *logic* without any hardware: `ctest -R arduino` runs the unit tests of `connectx_board.h`, and
  `python play.py model.cxnn --tflite model_int8.tflite` plays against the quantized model in the terminal.
