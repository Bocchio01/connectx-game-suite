#!/usr/bin/env python3
"""Turn a .tflite file into a C header (the equivalent of `xxd -i model.tflite`, but portable).

    python make_header.py ../models/connectx_6x7_int8.tflite model_data.h

No TensorFlow needed. The array is called `g_model` (length in `g_model_len`) like in the TensorFlow
Lite Micro examples. `export_tflite.py` already writes a header with the board constants too: use this
script when you only have the .tflite file.
"""

import argparse
import os
import sys

import tfbridge


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tflite", help="input .tflite file")
    parser.add_argument("header", help="output .h file")
    parser.add_argument("--array-name", default="g_model", help="name of the C array (default g_model)")
    args = parser.parse_args()

    with open(args.tflite, "rb") as f:
        data = f.read()

    guard = "CONNECTX_" + os.path.splitext(os.path.basename(args.header))[0].upper().replace("-", "_") + "_H"
    text = tfbridge.to_c_header(data, f"TensorFlow Lite model from {os.path.basename(args.tflite)} ({len(data)} bytes).",
                                array_name=args.array_name, guard=guard)
    with open(args.header, "w") as f:
        f.write(text)
    print(f"Wrote {args.header}: {len(data)} bytes in a C array named {args.array_name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
