"""TensorFlow / TensorFlow Lite helpers (imported only by the scripts that need TensorFlow).

Everything that touches TensorFlow lives here so that cxnn.py (and its tests) work with NumPy alone.
"""

import os
from typing import Iterable, List, Optional, Tuple

import numpy as np

import cxnn


def _tf():
    # TensorFlow takes seconds to import and prints a lot: import it lazily and quietly
    os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
    import tensorflow as tf

    return tf


# ---------------------------------------------------------------------------
# From the C++ network to Keras
# ---------------------------------------------------------------------------


def build_keras_model(model: cxnn.Model):
    """The same two-headed network in Keras, with the weights of the C++ model.

    A Keras ``Dense`` kernel has shape (inputs, outputs) and computes ``x @ kernel + bias``: exactly
    the layout of the C++ library, so the weights are copied as they are.
    """
    keras = _tf().keras

    inputs = keras.Input(shape=(model.input_size,), name="board")
    x = inputs
    for i, layer in enumerate(model.trunk):
        x = keras.layers.Dense(layer.weights.shape[1], activation="relu", name=f"trunk_{i}")(x)
    policy = keras.layers.Dense(model.policy_size, name="policy")(x)  # logits: softmax is applied by the user
    value = keras.layers.Dense(1, activation="tanh", name="value")(x)
    keras_model = keras.Model(inputs, [policy, value], name="connectx_net")

    for i, layer in enumerate(model.trunk):
        keras_model.get_layer(f"trunk_{i}").set_weights([layer.weights, layer.bias])
    keras_model.get_layer("policy").set_weights([model.policy.weights, model.policy.bias])
    keras_model.get_layer("value").set_weights([model.value.weights, model.value.bias])
    return keras_model


def keras_to_cxnn(keras_model, template: cxnn.Model, metadata: str) -> cxnn.Model:
    """Copies the weights of a (trained) Keras model back into a ``cxnn.Model`` with the architecture of `template`."""
    result = cxnn.Model(template.rows, template.cols, template.connect, template.encoding, template.input_size,
                        template.policy_size, metadata)
    for i, layer in enumerate(template.trunk):
        weights, bias = keras_model.get_layer(f"trunk_{i}").get_weights()
        result.trunk.append(cxnn.Layer(cxnn.ROLE_TRUNK, cxnn.ACT_RELU, weights.astype(np.float32), bias.astype(np.float32)))
    weights, bias = keras_model.get_layer("policy").get_weights()
    result.policy = cxnn.Layer(cxnn.ROLE_POLICY, cxnn.ACT_LINEAR, weights.astype(np.float32), bias.astype(np.float32))
    weights, bias = keras_model.get_layer("value").get_weights()
    result.value = cxnn.Layer(cxnn.ROLE_VALUE, cxnn.ACT_TANH, weights.astype(np.float32), bias.astype(np.float32))
    return result


def keras_predict(keras_model, x: np.ndarray, batch_size: int = 1024) -> Tuple[np.ndarray, np.ndarray]:
    logits, value = keras_model.predict(x.astype(np.float32), batch_size=batch_size, verbose=0)
    return np.asarray(logits, dtype=np.float32), np.asarray(value, dtype=np.float32)[:, 0]


# ---------------------------------------------------------------------------
# TensorFlow Lite conversion
# ---------------------------------------------------------------------------


def _converter(keras_model):
    tf = _tf()
    try:
        return tf.lite.TFLiteConverter.from_keras_model(keras_model)
    except Exception:  # pragma: no cover - Keras 3 fallback: go through a SavedModel
        import tempfile

        folder = tempfile.mkdtemp(prefix="connectx_savedmodel_")
        keras_model.export(folder)
        return tf.lite.TFLiteConverter.from_saved_model(folder)


def convert_float32(keras_model) -> bytes:
    """TensorFlow Lite model with float32 weights and activations (about 4 bytes per parameter)."""
    return _converter(keras_model).convert()


def convert_int8(keras_model, calibration: np.ndarray) -> bytes:
    """Fully quantized TensorFlow Lite model: int8 weights, activations, inputs and outputs.

    Post-training quantization: the converter runs the representative `calibration` positions through
    the float model to learn the range of every tensor, then maps those ranges to 8 bits. The model is
    4x smaller and runs on integer-only hardware (a Cortex-M4 of an Arduino Nano 33 BLE has no FPU for
    doubles, but integer kernels from CMSIS-NN are very fast).
    """
    tf = _tf()

    def representative_dataset():
        for row in calibration:
            yield [row[None, :].astype(np.float32)]

    converter = _converter(keras_model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    return converter.convert()


def _make_interpreter(content: bytes):
    # LiteRT (the new name of TensorFlow Lite) if installed, else the interpreter bundled with TensorFlow
    try:
        from ai_edge_litert.interpreter import Interpreter

        return Interpreter(model_content=content)
    except ImportError:
        return _tf().lite.Interpreter(model_content=content)


class TFLiteModel:
    """Runs a ``.tflite`` ConnectX model on NumPy inputs, taking care of the quantization.

    The policy and value outputs are told apart by their shape: (1, policy_size) and (1, 1).
    """

    def __init__(self, source, policy_size: int):
        content = open(source, "rb").read() if isinstance(source, str) else source
        self.content = content
        self.interpreter = _make_interpreter(content)
        self.interpreter.allocate_tensors()

        self.input = self.interpreter.get_input_details()[0]
        outputs = self.interpreter.get_output_details()
        self.policy_output = next(o for o in outputs if int(o["shape"][-1]) == policy_size)
        self.value_output = next(o for o in outputs if o is not self.policy_output)

    @property
    def quantized(self) -> bool:
        return self.input["dtype"] == np.int8

    def describe(self) -> str:
        parts = [f"input {np.dtype(self.input['dtype']).name} {tuple(self.input['shape'])}"]
        if self.quantized:
            scale, zero_point = self.input["quantization"]
            parts.append(f"(scale {scale:.6g}, zero point {zero_point})")
        return " ".join(parts)

    def _read(self, detail) -> np.ndarray:
        raw = self.interpreter.get_tensor(detail["index"])
        if detail["dtype"] == np.int8:
            scale, zero_point = detail["quantization"]
            return (raw.astype(np.float32) - zero_point) * scale
        return raw.astype(np.float32)

    def predict(self, x: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        x = np.asarray(x, dtype=np.float32)
        if x.ndim == 1:
            x = x[None, :]

        logits, values = [], []
        for row in x:
            if self.quantized:
                scale, zero_point = self.input["quantization"]
                sample = np.clip(np.round(row / scale + zero_point), -128, 127).astype(np.int8)
            else:
                sample = row
            self.interpreter.set_tensor(self.input["index"], sample[None, :])
            self.interpreter.invoke()
            logits.append(self._read(self.policy_output)[0])
            values.append(self._read(self.value_output)[0, 0])
        return np.array(logits, dtype=np.float32), np.array(values, dtype=np.float32)


# ---------------------------------------------------------------------------
# C header for microcontrollers
# ---------------------------------------------------------------------------


def to_c_header(data: bytes, comment: str, array_name: str = "g_model", guard: str = "CONNECTX_MODEL_DATA_H",
                defines: Optional[List[Tuple[str, int]]] = None) -> str:
    """The bytes of a .tflite file as a C array (what ``xxd -i`` does, without needing xxd)."""
    lines = []
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")

    header = [
        f"// {line}" for line in comment.strip().splitlines()
    ]
    header += [
        "// Generated by export_tflite.py / make_header.py: do not edit by hand.",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    for name, value in defines or []:
        header.append(f"#define {name} {value}")
    if defines:
        header.append("")
    header += [
        "// The flatbuffer must be aligned in memory (the TensorFlow Lite Micro examples use alignas(8)).",
        f"alignas(8) const unsigned char {array_name}[] = {{",
        *lines,
        "};",
        f"const unsigned int {array_name}_len = {len(data)};",
        "",
        f"#endif // {guard}",
        "",
    ]
    return "\n".join(header)
