#!/usr/bin/env python3
"""Play Connect-4 against a network in the terminal (no server needed).

    python play.py ../models/connectx_6x7.cxnn
    python play.py ../models/connectx_6x7.cxnn --tflite ../models/connectx_6x7_int8.tflite --shield

Type a column number (1-7) to drop a stone, q to quit. With --tflite the moves come from the
quantized TensorFlow Lite model: this is what the Arduino will play like.
"""

import argparse
import sys

import cxnn
from eval_model import Player


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("model", help=".cxnn file")
    parser.add_argument("--tflite", help="use this .tflite model for the moves")
    parser.add_argument("--shield", action="store_true", help="the computer always takes an immediate win / blocks an immediate loss")
    parser.add_argument("--mirror", action="store_true", help="average the network output with the one for the mirrored board")
    parser.add_argument("--second", action="store_true", help="let the computer start")
    args = parser.parse_args()

    model = cxnn.load_model(args.model)
    if args.tflite:
        import tfbridge

        predict = tfbridge.TFLiteModel(args.tflite, model.policy_size).predict
    else:
        predict = model.predict

    computer = Player(model, predict, args.shield, args.mirror)
    board = cxnn.Board(model.rows, model.cols, model.connect)
    human_id = 2 if args.second else 1

    print(f"You are X (first player)." if human_id == 1 else "You are O (second player).")
    while not board.is_over():
        print("\n" + str(board) + "\n")
        if board.player == human_id:
            text = input(f"Your move (1-{model.cols}, q to quit): ").strip().lower()
            if text == "q":
                return 0
            if not text.isdigit() or not (1 <= int(text) <= model.cols) or not board.legal()[int(text) - 1]:
                print("Not a valid column.")
                continue
            board.play(int(text) - 1)
        else:
            column = computer.move(board)
            print(f"The computer plays column {column + 1}")
            board.play(column)

    print("\n" + str(board) + "\n")
    if board.winner == 0:
        print("It's a draw.")
    elif board.winner == human_id:
        print("You win!")
    else:
        print("The computer wins.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
