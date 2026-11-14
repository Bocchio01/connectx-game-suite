/*
  ConnectXNet: play Connect 4 against a neural network running on the microcontroller.

  The network was trained from scratch with the C++ code of this repository (ai/nn, connectx-train),
  converted to TensorFlow Lite with ai/python/export_tflite.py (int8 quantization) and embedded in
  model_data.h. It is run by TensorFlow Lite for Microcontrollers.

  How the computer plays (SEARCH_DEPTH below):
    SEARCH_DEPTH >= 1  alpha-beta search of that many plies in which the VALUE head of the network
                       evaluates the positions at the end of the search. Wins and forced moves are
                       found exactly, without the network. This is by far the strongest mode: a depth of
                       2 already plays like a classic alpha-beta of depth 4.
    SEARCH_DEPTH == 0  no search: the POLICY head scores the 7 columns and the best free one is played
                       (optionally with the "tactics shield": an immediate win is always taken and an
                       immediate loss always blocked). One network evaluation per move, but much weaker.

  Play from the Serial Monitor (115200 baud): type the number of a column (1-7) to drop a stone,
  'n' to start a new game.

  Libraries (Arduino IDE > Library Manager): "Chirale_TensorFlowLite" (TensorFlow Lite Micro for
  Nano 33 BLE, Nano ESP32, Nicla, Portenta, Giga R1...). The official Arduino_TensorFlowLite library
  is archived; with it change only the includes below and the MicroInterpreter constructor (see README).

  The model: generate model_data.h with
      python export_tflite.py ../models/connectx_6x7.cxnn --out-dir <folder>
  and copy <folder>/connectx_6x7_int8.h here as model_data.h (replacing the placeholder that is in this folder:
  it is not generated in advance because the conversion needs TensorFlow).
*/

// ---- Settings -------------------------------------------------------------------------------

// Search depth in plies (0 = no search, policy head only). Each extra ply multiplies the time by
// about 3-5: measure it on your board (the sketch prints the time of every move).
#define SEARCH_DEPTH 2

// Policy mode only (SEARCH_DEPTH 0): always play an immediate win / block an immediate loss
#define USE_TACTICS_SHIELD 1

// Evaluate every position and its mirror image and average the two answers: a little stronger,
// twice as slow (the game is symmetric, so both must agree: the average cancels some of the noise)
#define USE_MIRROR_AVERAGING 0

// 0: you move first, 1: the computer moves first
#define COMPUTER_STARTS 0

// Memory used by TensorFlow Lite for the activations and its own bookkeeping. If AllocateTensors() fails
// at start-up increase it; on boards with little RAM (< 64 KB) use a smaller network.
constexpr int kTensorArenaSize = 24 * 1024;

// ---------------------------------------------------------------------------------------------

#include <Chirale_TensorFlowLite.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "model_data.h"       // g_model: the .tflite file as a C array, plus CONNECTX_* constants
#include "connectx_board.h"   // the game, the encoders and the search (must come after model_data.h)

#ifndef CONNECTX_ENCODING
#define CONNECTX_ENCODING connectx::ENCODING_TACTICAL
#endif

alignas(16) static uint8_t tensor_arena[kTensorArenaSize]; // aligned to 16 bytes for CMSIS-NN

static const tflite::Model *model = nullptr;
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *input_tensor = nullptr;
static TfLiteTensor *policy_tensor = nullptr; // one score (logit) per column
static TfLiteTensor *value_tensor = nullptr;  // who is winning, in (-1, 1)

static connectx::Board game;
static uint8_t humanId = COMPUTER_STARTS ? 2 : 1;

// ---- Neural network -------------------------------------------------------------------------

[[noreturn]] static void fatal(const char *message)
{
    Serial.println(message);
    while (true)
    {
        delay(1000);
    }
}

static void initNetwork()
{
    model = tflite::GetModel(g_model);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        fatal("The model's schema version does not match the TensorFlow Lite Micro library.");
    }

    // Only the operations the network uses: fully connected layers (with ReLU fused) and tanh.
    // Quantize / Dequantize are needed only if the model was exported with float inputs/outputs.
    static tflite::MicroMutableOpResolver<4> resolver;
    resolver.AddFullyConnected();
    resolver.AddTanh();
    resolver.AddQuantize();
    resolver.AddDequantize();

    static tflite::MicroInterpreter staticInterpreter(model, resolver, tensor_arena, kTensorArenaSize);
    interpreter = &staticInterpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk)
    {
        fatal("AllocateTensors() failed: increase kTensorArenaSize.");
    }

    input_tensor = interpreter->input(0);

    // The two outputs are told apart by their size: COLS scores for the policy, 1 number for the value
    for (size_t i = 0; i < interpreter->outputs_size(); ++i)
    {
        TfLiteTensor *tensor = interpreter->output(i);
        const int size = tensor->dims->data[tensor->dims->size - 1];
        if (size == connectx::COLS && policy_tensor == nullptr)
        {
            policy_tensor = tensor;
        }
        else
        {
            value_tensor = tensor;
        }
    }
    if (policy_tensor == nullptr || value_tensor == nullptr)
    {
        fatal("The model must have two outputs: one score per column and one value.");
    }
}

static float readOutput(const TfLiteTensor *tensor, int index)
{
    if (tensor->type == kTfLiteInt8)
    {
        // Dequantize: x = (q - zero_point) * scale
        return (tensor->data.int8[index] - tensor->params.zero_point) * tensor->params.scale;
    }
    return tensor->data.f[index];
}

// Runs the network on an encoded position: the COLS scores of the policy head and the value
static void infer(const float *encoded, float *scores, float *value)
{
    const int inputSize = connectx::encodedSize(CONNECTX_ENCODING);

    if (input_tensor->type == kTfLiteInt8)
    {
        // Quantize: q = x / scale + zero_point
        const float scale = input_tensor->params.scale;
        const int zeroPoint = input_tensor->params.zero_point;
        for (int i = 0; i < inputSize; ++i)
        {
            const int q = static_cast<int>(encoded[i] / scale + (encoded[i] >= 0 ? 0.5f : -0.5f)) + zeroPoint;
            input_tensor->data.int8[i] = static_cast<int8_t>(q < -128 ? -128 : (q > 127 ? 127 : q));
        }
    }
    else
    {
        for (int i = 0; i < inputSize; ++i)
        {
            input_tensor->data.f[i] = encoded[i];
        }
    }

    if (interpreter->Invoke() != kTfLiteOk)
    {
        fatal("TensorFlow Lite Invoke() failed.");
    }

    for (int c = 0; c < connectx::COLS; ++c)
    {
        scores[c] = readOutput(policy_tensor, c);
    }
    *value = readOutput(value_tensor, 0);
}

// Mirror image of a board (the game is symmetric)
static connectx::Board mirrorOf(const connectx::Board &board)
{
    connectx::Board mirrored = board;
    for (int r = 0; r < connectx::ROWS; ++r)
    {
        for (int c = 0; c < connectx::COLS; ++c)
        {
            mirrored.cells[r][c] = board.cells[r][connectx::COLS - 1 - c];
        }
    }
    return mirrored;
}

// The network seen as an evaluation function: policy scores and value of a position for the player to move
static void evaluate(const connectx::Board &board, float *scores, float *value)
{
    static float encoded[connectx::MAX_ENCODED_SIZE];

    connectx::encodeBoard(board, CONNECTX_ENCODING, encoded);
    infer(encoded, scores, value);

#if USE_MIRROR_AVERAGING
    float mirroredScores[connectx::COLS];
    float mirroredValue;
    connectx::encodeBoard(mirrorOf(board), CONNECTX_ENCODING, encoded);
    infer(encoded, mirroredScores, &mirroredValue);

    for (int c = 0; c < connectx::COLS; ++c)
    {
        scores[c] = (scores[c] + mirroredScores[connectx::COLS - 1 - c]) / 2;
    }
    *value = (*value + mirroredValue) / 2;
#endif
}

// The evaluation function handed to the search
struct ValueEvaluator
{
    uint32_t calls = 0;

    float operator()(const connectx::Board &board)
    {
        float scores[connectx::COLS];
        float value;
        evaluate(board, scores, &value);
        ++calls;
        return value;
    }
};

// ---- Game and user interface ----------------------------------------------------------------

static void printBoard()
{
    Serial.println();
    for (int r = 0; r < connectx::ROWS; ++r)
    {
        for (int c = 0; c < connectx::COLS; ++c)
        {
            const uint8_t cell = game.cells[r][c];
            Serial.print(cell == 0 ? '.' : (cell == 1 ? 'X' : 'O'));
            Serial.print(' ');
        }
        Serial.println();
    }
    for (int c = 0; c < connectx::COLS; ++c)
    {
        Serial.print(c + 1);
        Serial.print(' ');
    }
    Serial.println();
    Serial.println();
}

static void startGame()
{
    game.reset();
    Serial.println();
    Serial.println("=== New game ===");
    Serial.print("You are ");
    Serial.print(humanId == 1 ? "X (you move first)" : "O (the computer moves first)");
    Serial.println(".");
    printBoard();
}

static void announceResult()
{
    if (game.winner == 0)
    {
        Serial.println("It's a draw.");
    }
    else if (game.winner == humanId)
    {
        Serial.println("You win!");
    }
    else
    {
        Serial.println("The computer wins.");
    }
    Serial.println("Type n for a new game.");
}

static void computerMove()
{
    float scores[connectx::COLS];
    int column;
    uint32_t evaluations = 0;

    const uint32_t started = micros();

#if SEARCH_DEPTH > 0
    ValueEvaluator evaluator;
    column = connectx::searchColumn(game, SEARCH_DEPTH, evaluator, scores);
    evaluations = evaluator.calls;
#else
    int forced = -1;
#if USE_TACTICS_SHIELD
    forced = connectx::tacticalColumn(game);
#endif
    if (forced >= 0)
    {
        column = forced; // a forced move needs no network at all
    }
    else
    {
        float value;
        evaluate(game, scores, &value);
        evaluations = 1;
        column = connectx::chooseColumn(game, scores, false);
    }
#endif

    const uint32_t elapsed = micros() - started;

    game.play(column);

    Serial.print("The computer plays column ");
    Serial.print(column + 1);
    Serial.print("  (");
    Serial.print(evaluations);
    Serial.print(" network evaluations, ");
    Serial.print(elapsed / 1000.0f, 1);
    Serial.println(" ms)");
    printBoard();
}

void setup()
{
    Serial.begin(115200);
    while (!Serial)
    {
        ; // wait for the Serial Monitor
    }

    Serial.println("Connect 4 against a neural network (TensorFlow Lite Micro)");
    Serial.print("Model: ");
    Serial.print(g_model_len);
    Serial.println(" bytes");

    initNetwork();

    Serial.print("Input: ");
    Serial.print(connectx::encodedSize(CONNECTX_ENCODING));
    Serial.print(" numbers, tensor arena used: ");
    Serial.print(interpreter->arena_used_bytes());
    Serial.print(" of ");
    Serial.print(kTensorArenaSize);
    Serial.print(" bytes, search depth: ");
    Serial.println(SEARCH_DEPTH);

    startGame();
}

void loop()
{
    if (!game.isOver() && game.player != humanId)
    {
        computerMove();
        if (game.isOver())
        {
            announceResult();
        }
        return;
    }

    if (!Serial.available())
    {
        return;
    }

    const char ch = static_cast<char>(Serial.read());

    if (ch == 'n' || ch == 'N')
    {
        startGame();
        return;
    }

    if (game.isOver() || ch < '1' || ch > static_cast<char>('0' + connectx::COLS))
    {
        return; // (line endings and other characters are ignored)
    }

    const int column = ch - '1';
    if (!game.canPlay(column))
    {
        Serial.println("That column is full.");
        return;
    }

    game.play(column);
    printBoard();
    if (game.isOver())
    {
        announceResult();
    }
}
