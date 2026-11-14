# From the C++ network to TensorFlow, TensorFlow Lite and the Arduino

The networks of this project are trained **from scratch in C++** ([`ai/nn`](../nn/README.md), `connectx-train`).
This folder is the bridge to the TensorFlow world:

```
connectx-train  ──▶  model.cxnn  ──▶  Keras model  ──▶  model_float32.tflite
 (C++, from scratch)  (weights)       (TensorFlow)   └─▶  model_int8.tflite  ──▶  model_int8.h  ──▶  Arduino sketch
                                                          (quantized, ~4x smaller)   (C array)
```

Everything is checked at every step: the Keras model must reproduce the C++ outputs, the float TFLite model
must reproduce them too, and the int8 model is compared with the float one (how often it picks the same move).

> **Status.** The NumPy part (`cxnn.py`, `eval_model.py`, `play.py`, `make_header.py`) is tested: the Python reader and
> encoders agree with the C++ ones (`python -m unittest discover -s tests`, and the test vectors of the pretrained model).
> The scripts that need TensorFlow (`export_tflite.py`, `train_keras.py`, `tfbridge.py`) follow the TensorFlow 2.16+
> API but have **not been executed yet**, because no TensorFlow was available where they were written: if a first
> run reports a problem, that is the most likely place. The built-in checks (outputs against the C++ network, int8
> against float) are there precisely to tell you whether a conversion can be trusted.

## Setup

Only NumPy is needed to read models and datasets, to evaluate a network and to play against it
(`cxnn.py`, `eval_model.py`, `play.py`, `make_header.py`). TensorFlow is needed for the export and for training
with Keras.

```bash
python -m venv .venv
# Windows:  .venv\Scripts\activate        Linux/macOS:  source .venv/bin/activate
pip install -r requirements.txt
```

> **Python version.** TensorFlow wheels exist only for some Python versions (check
> [tensorflow.org/install/pip](https://www.tensorflow.org/install/pip)): at the time of writing 3.9 to 3.12 on
> Windows (CPU build; native Windows GPU support ended with TensorFlow 2.10). If `pip` cannot find a matching
> TensorFlow, create the virtual environment with another interpreter (e.g. `py -3.12 -m venv .venv`).

Run the tests of the NumPy part (they compare Python with the C++ encoders and networks):

```bash
python -m unittest discover -s tests -v
```

## The scripts

| Script | What it does | Needs TensorFlow |
|---|---|:---:|
| [`cxnn.py`](cxnn.py) | reads/writes `.cxnn` models and `.cxds` datasets; NumPy forward pass; the three input encoders; a tiny Connect-X engine | no |
| [`export_tflite.py`](export_tflite.py) | `.cxnn` → Keras → TFLite float32 and int8 → C header, with parity and quantization checks | yes |
| [`eval_model.py`](eval_model.py) | accuracy against the exact solver and games against baseline opponents, for a `.cxnn` or a `.tflite` | only for `.tflite` |
| [`play.py`](play.py) | play against a network in the terminal | only for `.tflite` |
| [`make_header.py`](make_header.py) | any `.tflite` → C array (like `xxd -i`) | no |
| [`train_keras.py`](train_keras.py) | train the same network with Keras instead of the C++ trainer; writes a `.cxnn` | yes |
| [`tfbridge.py`](tfbridge.py) | the TensorFlow code shared by the scripts above | yes |

## Workflow

**1. Get a trained model.** A pretrained one is in [`ai/models`](../models). To train your own with the C++
tools see the [AI guide](../README.md#training-your-own-network).

**2. Export it.**

```bash
python export_tflite.py ../models/connectx_6x7.cxnn --out-dir ../models --vectors ../models/connectx_6x7_vectors.json
```

Output (the files go to `--out-dir`):

* `connectx_6x7_float32.tflite`: weights and activations in float32 (~4 bytes per parameter);
* `connectx_6x7_int8.tflite`: fully quantized, **int8 weights, activations, inputs and outputs**;
* `connectx_6x7_int8.h`: the int8 model as a C array (`g_model`) plus the board constants, for the
  [Arduino sketch](../arduino/README.md).

`--vectors` is a file written by `connectx-train export-vectors`: positions together with the *exact* outputs of the
C++ network. The script checks that the Keras model reproduces them to 1e-3 (float32 arithmetic should give ~1e-6).

**3. Evaluate the deployed model, from Python.**

```bash
# the float network
python eval_model.py ../models/connectx_6x7.cxnn --games 200
# the int8 TFLite model that will run on the Arduino (same board code, independent of the C++ engine)
python eval_model.py ../models/connectx_6x7.cxnn --tflite ../models/connectx_6x7_int8.tflite --games 200
# accuracy against the exact solver on labelled positions
python eval_model.py ../models/connectx_6x7.cxnn --tflite ../models/connectx_6x7_int8.tflite --dataset test.cxds
```

**4. Play against it** (no Arduino needed): `python play.py ../models/connectx_6x7.cxnn --tflite ../models/connectx_6x7_int8.tflite`

**5. Put it on a microcontroller:** see [`ai/arduino`](../arduino/README.md).

## What the conversion does (and why the int8 model is smaller *and* faster)

* **Keras.** A Keras `Dense` layer computes `y = x·W + b` with a kernel of shape `(inputs, outputs)`: exactly the
  layout of the C++ library, so the weights are copied as they are. The network has two outputs (the policy logits
  and the value), built with the functional API.
* **TensorFlow Lite.** `tf.lite.TFLiteConverter` turns the Keras graph into a TFLite flatbuffer. This network only
  needs two operators, `FULLY_CONNECTED` (with the ReLU fused in) and `TANH`.
* **Quantization.** A float32 weight costs 4 bytes; int8 costs 1 and integer arithmetic is much cheaper on a
  microcontroller. *Post-training quantization* maps every tensor to 8 bits with `real = scale · (q − zero_point)`;
  the scales are measured by running a *representative dataset* (real positions) through the float model.
  The script then measures the cost of the approximation: the share of positions where the int8 and the float
  model choose the same move, and the largest difference of the outputs.
* **The input encoding.** The model expects the input described in
  [`ai/include/ai/encoding.hpp`](../include/ai/encoding.hpp) (the default network uses the `tactical` encoding: 5
  planes of 6×7). `cxnn.encode` is the Python implementation, tested to be bit-identical to the C++ one, and the
  Arduino sketch has its own C++ twin (`connectx_board.h`, also tested against the C++ encoder).

## LiteRT

Google is renaming TensorFlow Lite to LiteRT. `tf.lite.TFLiteConverter` is still the way to produce the model; the
scripts run it with `ai_edge_litert` when that package is installed and fall back to the interpreter that comes with
TensorFlow otherwise.

## Training with Keras instead of C++

```bash
python train_keras.py --dataset data.cxds --out keras_model.cxnn --hidden 256,256 --encoding tactical --epochs 40
```

Same data, same architecture and losses as `connectx-train supervised`, with Keras doing the gradient descent.
The result is a `.cxnn` file: the C++ tools (arena, AI client, GUI) and `export_tflite.py` use it like any other
model. (Datasets come from `connectx-train gen-data`; no self-play in this script: use the C++ tool for that.)
