// PLACEHOLDER: replace this file with the network as a C array (and the board constants).
//
// It is not in the repository because producing it needs TensorFlow (see ai/python/README.md):
//
//     cd ai/python
//     python export_tflite.py ../models/connectx_6x7.cxnn --out-dir ../models
//     cp ../models/connectx_6x7_int8.h ../arduino/ConnectXNet/model_data.h
//
// Any other model works the same way: the exporter writes the board size and the encoding next to the array.
#error "ConnectXNet needs model_data.h: replace this placeholder with the output of ai/python/export_tflite.py (see ai/arduino/README.md)"
