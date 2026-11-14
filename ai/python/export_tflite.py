#!/usr/bin/env python3
"""Export a ConnectX network trained by the C++ code to TensorFlow and TensorFlow Lite.

    python export_tflite.py ../models/connectx_6x7.cxnn --out-dir ../models

What it does, step by step (and checks, so that nothing is silently wrong):

  1. reads the .cxnn model written by `connectx-train` (the network trained *from scratch* in C++);
  2. rebuilds the very same architecture in Keras and copies the weights;
  3. PARITY CHECK: the Keras outputs must equal the C++ outputs (test vectors written by
     `connectx-train export-vectors`, if given) and the NumPy forward pass of cxnn.py;
  4. converts to TensorFlow Lite in float32 (`<name>_float32.tflite`);
  5. converts to a fully integer int8 model (`<name>_int8.tflite`) with post-training quantization,
     calibrated on real positions;
  6. QUANTIZATION CHECK: how often the int8 and the float model pick the same move, and how far apart
     their outputs are;
  7. writes `<name>_int8.h`, the int8 model as a C array, ready for the Arduino sketch.
"""

import argparse
import json
import os
import sys
import time

import numpy as np

import cxnn


def random_positions(model: cxnn.Model, count: int, seed: int = 0):
    """Positions reached by random play, as (cells seen by the player to move, encoded input) arrays."""
    rng = np.random.default_rng(seed)
    cells, inputs = [], []
    while len(inputs) < count:
        board = cxnn.Board(model.rows, model.cols, model.connect)
        stop = int(rng.integers(0, model.rows * model.cols - 4))
        while not board.is_over() and board.moves < stop:
            board.play(int(rng.choice(np.flatnonzero(board.legal()))))
        if board.is_over():
            continue
        view = board.view()
        cells.append(view)
        inputs.append(cxnn.encode(view, model.connect, model.encoding))
    return np.array(cells), np.array(inputs, dtype=np.float32)


def dataset_positions(model: cxnn.Model, path: str, count: int):
    dataset = cxnn.load_dataset(path)
    if (dataset.rows, dataset.cols, dataset.connect) != (model.rows, model.cols, model.connect):
        raise SystemExit(f"{path}: the dataset is for another board than the model")
    rng = np.random.default_rng(1)
    pick = rng.permutation(len(dataset))[:count]
    cells = dataset.cells[pick]
    inputs = np.array([cxnn.encode(c, model.connect, model.encoding) for c in cells], dtype=np.float32)
    return cells, inputs, dataset, pick


def legal_mask(cells: np.ndarray) -> np.ndarray:
    return cells[:, 0, :] == 0


def best_moves(logits: np.ndarray, legal: np.ndarray) -> np.ndarray:
    return np.argmax(np.where(legal, logits, -np.inf), axis=1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("model", help="the .cxnn file written by connectx-train")
    parser.add_argument("--out-dir", default=".", help="where to write the .tflite files and the C header")
    parser.add_argument("--name", help="base name of the output files (default: the model file name)")
    parser.add_argument("--vectors", help="test vectors written by `connectx-train export-vectors` (parity check against C++)")
    parser.add_argument("--dataset", help="a .cxds dataset to calibrate the quantization and to measure the accuracy on "
                                          "(default: positions reached by random play)")
    parser.add_argument("--samples", type=int, default=1000, help="positions used for calibration and for the checks")
    parser.add_argument("--skip-int8", action="store_true", help="only produce the float32 model")
    args = parser.parse_args()

    import tfbridge  # imports TensorFlow lazily

    model = cxnn.load_model(args.model)
    name = args.name or os.path.splitext(os.path.basename(args.model))[0]
    os.makedirs(args.out_dir, exist_ok=True)

    print(f"Model {args.model}")
    print(f"  board {model.rows}x{model.cols}, connect {model.connect}, encoding '{model.encoding_name}', "
          f"{model.input_size} inputs, trunk {model.hidden}, {model.parameter_count()} parameters")
    if model.metadata:
        print(f"  metadata: {model.metadata}")

    # ---- 1. Keras ---------------------------------------------------------------------------------
    keras_model = tfbridge.build_keras_model(model)

    if args.dataset:
        cells, inputs, dataset, pick = dataset_positions(model, args.dataset, args.samples)
    else:
        cells, inputs = random_positions(model, args.samples)
        dataset = pick = None
    legal = legal_mask(cells)

    # ---- 2. parity with the C++ implementation ------------------------------------------------------
    numpy_logits, numpy_value = model.predict(inputs)
    keras_logits, keras_value = tfbridge.keras_predict(keras_model, inputs)
    error = max(float(np.abs(keras_logits - numpy_logits).max()), float(np.abs(keras_value - numpy_value).max()))
    print(f"\n[parity] Keras vs NumPy forward pass on {len(inputs)} positions: max |difference| = {error:.2e}")
    if error > 1e-3:
        print("  ERROR: the Keras model does not reproduce the C++ network", file=sys.stderr)
        return 1

    if args.vectors:
        with open(args.vectors) as f:
            vectors = json.load(f)
        x = np.array([s["input"] for s in vectors["samples"]], dtype=np.float32)
        expected_logits = np.array([s["logits"] for s in vectors["samples"]], dtype=np.float32)
        expected_value = np.array([s["value"] for s in vectors["samples"]], dtype=np.float32)
        logits, value = tfbridge.keras_predict(keras_model, x)
        error = max(float(np.abs(logits - expected_logits).max()), float(np.abs(value - expected_value).max()))
        print(f"[parity] Keras vs the C++ test vectors ({len(x)} positions): max |difference| = {error:.2e}")
        if error > 1e-3:
            print("  ERROR: the Keras outputs differ from the C++ ones", file=sys.stderr)
            return 1

    # ---- 3. float32 TFLite --------------------------------------------------------------------------
    float_bytes = tfbridge.convert_float32(keras_model)
    float_path = os.path.join(args.out_dir, f"{name}_float32.tflite")
    with open(float_path, "wb") as f:
        f.write(float_bytes)

    float_model = tfbridge.TFLiteModel(float_bytes, model.policy_size)
    check = min(len(inputs), 300)
    logits, value = float_model.predict(inputs[:check])
    error = max(float(np.abs(logits - numpy_logits[:check]).max()), float(np.abs(value - numpy_value[:check]).max()))
    print(f"\n[float32] {float_path}  ({len(float_bytes) / 1024:.1f} KiB)")
    print(f"  TFLite float32 vs C++/NumPy outputs: max |difference| = {error:.2e}")
    if error > 1e-3:
        print("  ERROR: the float TFLite model differs from the C++ network", file=sys.stderr)
        return 1

    if args.skip_int8:
        return 0

    # ---- 4. int8 TFLite ------------------------------------------------------------------------------
    calibration_cells, calibration = random_positions(model, args.samples, seed=7) if not args.dataset else (cells, inputs)
    started = time.time()
    int8_bytes = tfbridge.convert_int8(keras_model, calibration)
    int8_path = os.path.join(args.out_dir, f"{name}_int8.tflite")
    with open(int8_path, "wb") as f:
        f.write(int8_bytes)
    print(f"\n[int8] {int8_path}  ({len(int8_bytes) / 1024:.1f} KiB, converted in {time.time() - started:.1f}s)")

    int8_model = tfbridge.TFLiteModel(int8_bytes, model.policy_size)
    print(f"  {int8_model.describe()}")

    # ---- 5. what did the quantization cost? ----------------------------------------------------------
    check = min(len(inputs), 500)
    q_logits, q_value = int8_model.predict(inputs[:check])
    same_move = best_moves(q_logits, legal[:check]) == best_moves(numpy_logits[:check], legal[:check])
    print(f"  quantization check on {check} positions:")
    print(f"    same best legal move as the float model: {100.0 * same_move.mean():.1f}%")
    print(f"    max |logit difference|: {np.abs(q_logits - numpy_logits[:check]).max():.3f}   "
          f"max |value difference|: {np.abs(q_value - numpy_value[:check]).max():.3f}")

    if dataset is not None:
        _, optimal = cxnn.outcome_targets(dataset)
        chosen_float = best_moves(numpy_logits[:check], legal[:check])
        chosen_int8 = best_moves(q_logits, legal[:check])
        rows = np.arange(check)
        print(f"    plays a result-preserving move: float {100.0 * optimal[pick[:check]][rows, chosen_float].mean():.1f}%  "
              f"int8 {100.0 * optimal[pick[:check]][rows, chosen_int8].mean():.1f}%  (vs. the exact solver)")

    # ---- 6. C header for the Arduino sketch ----------------------------------------------------------
    header_path = os.path.join(args.out_dir, f"{name}_int8.h")
    comment = (f"ConnectX network '{name}': board {model.rows}x{model.cols}, connect {model.connect}, "
               f"input encoding '{model.encoding_name}' ({model.input_size} inputs), {model.parameter_count()} parameters, "
               f"int8 quantized TensorFlow Lite model ({len(int8_bytes)} bytes).")
    defines = [("CONNECTX_ROWS", model.rows), ("CONNECTX_COLS", model.cols), ("CONNECTX_CONNECT", model.connect),
               ("CONNECTX_ENCODING", model.encoding), ("CONNECTX_INPUT_SIZE", model.input_size)]
    with open(header_path, "w") as f:
        f.write(tfbridge.to_c_header(int8_bytes, comment, defines=defines))
    print(f"\n[header] {header_path}  (copy it next to the sketch as model_data.h)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
