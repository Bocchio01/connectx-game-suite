#!/usr/bin/env python3
"""Measure how good a ConnectX network is, from Python.

    # the C++ network (NumPy forward pass)
    python eval_model.py ../models/connectx_6x7.cxnn --games 200 --shield

    # the quantized model that will run on the Arduino
    python eval_model.py ../models/connectx_6x7.cxnn --tflite ../models/connectx_6x7_int8.tflite --games 200 --shield

    # accuracy against the exact solver on labelled positions
    python eval_model.py ../models/connectx_6x7.cxnn --dataset data.cxds

The .cxnn file is always needed (it knows the board and the input encoding); with --tflite the moves
are computed by the TensorFlow Lite model instead. The games are played in pure Python against simple
opponents, so this checks the deployed model end to end, independently from the C++ code.
"""

import argparse
import sys
import time

import numpy as np

import cxnn


def greedy_move(board: cxnn.Board, rng: np.random.Generator) -> int:
    """The one-move-lookahead opponent of the C++ code (agent "greedy"): win, block, avoid blunders, centre."""
    legal = [c for c in range(board.cols) if board.legal()[c]]

    def most_central(columns):
        distance = [abs(2 * c - (board.cols - 1)) for c in columns]
        best = [c for c, d in zip(columns, distance) if d == min(distance)]
        return int(rng.choice(best))

    winning = [c for c in legal if board.wins_by_playing(c)]
    if winning:
        return most_central(winning)

    safe = []
    for c in legal:
        after = board.copy()
        after.play(c)
        if after.is_over() or not any(after.wins_by_playing(o) for o in range(after.cols) if after.legal()[o]):
            safe.append(c)
    return most_central(safe or legal)


class Player:
    """Chooses moves with a network, optionally with the 1-move tactics shield and mirror averaging."""

    def __init__(self, model: cxnn.Model, predict, shield: bool, mirror: bool):
        self.model, self.predict, self.shield, self.mirror = model, predict, shield, mirror

    def move(self, board: cxnn.Board) -> int:
        if self.shield:
            forced = cxnn.tactical_move(board)
            if forced is not None:
                return forced

        view = board.view()
        x = cxnn.encode(view, self.model.connect, self.model.encoding)
        logits, _ = self.predict(x)
        logits = logits[0]
        if self.mirror:
            mirrored = cxnn.encode(view[:, ::-1], self.model.connect, self.model.encoding)
            mirrored_logits, _ = self.predict(mirrored)
            logits = (logits + mirrored_logits[0][::-1]) / 2
        return cxnn.best_legal_move(logits, board.legal())


def play_game(player: Player, opponent: str, network_first: bool, rng: np.random.Generator) -> int:
    """Returns +1 if the network wins, -1 if it loses, 0 for a draw."""
    model = player.model
    board = cxnn.Board(model.rows, model.cols, model.connect)
    network_id = 1 if network_first else 2

    while not board.is_over():
        if board.player == network_id:
            column = player.move(board)
        elif opponent == "random":
            column = int(rng.choice(np.flatnonzero(board.legal())))
        else:
            column = greedy_move(board, rng)
        board.play(column)

    if board.winner == 0:
        return 0
    return 1 if board.winner == network_id else -1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("model", help=".cxnn file")
    parser.add_argument("--tflite", help="evaluate this .tflite model instead of the NumPy forward pass")
    parser.add_argument("--dataset", help="measure the accuracy against the solver on this .cxds dataset")
    parser.add_argument("--limit", type=int, default=3000, help="positions of the dataset to use (default 3000)")
    parser.add_argument("--games", type=int, default=0, help="games to play against each baseline opponent")
    parser.add_argument("--shield", action="store_true", help="play an immediate win / block an immediate loss before asking the network")
    parser.add_argument("--mirror", action="store_true", help="average the answer with the one for the mirrored board")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    model = cxnn.load_model(args.model)
    if args.tflite:
        import tfbridge

        tflite = tfbridge.TFLiteModel(args.tflite, model.policy_size)
        predict = tflite.predict
        what = f"TensorFlow Lite model {args.tflite} ({tflite.describe()})"
    else:
        predict = model.predict
        what = f"NumPy forward pass of {args.model}"
    print(f"Evaluating the {what}")
    print(f"  board {model.rows}x{model.cols}, connect {model.connect}, encoding '{model.encoding_name}', "
          f"{model.parameter_count()} parameters, shield={'on' if args.shield else 'off'}, mirror={'on' if args.mirror else 'off'}")

    rng = np.random.default_rng(args.seed)

    if args.dataset:
        dataset = cxnn.load_dataset(args.dataset)
        count = min(args.limit, len(dataset))
        _, optimal = cxnn.outcome_targets(dataset)
        hits = {1: [0, 0], 0: [0, 0], -1: [0, 0]}
        for i in range(count):
            cells = dataset.cells[i]
            board = cxnn.Board(model.rows, model.cols, model.connect)
            # Rebuild a Board whose player to move sees `cells`: stone 1 = mine
            board.cells = cells.copy()
            board.player = 1
            if args.shield and cxnn.tactical_move(board) is not None:
                column = cxnn.tactical_move(board)
            else:
                x = cxnn.encode(cells, model.connect, model.encoding)
                logits, _ = predict(x)
                logits = logits[0]
                if args.mirror:
                    mirrored_logits, _ = predict(cxnn.encode(cells[:, ::-1], model.connect, model.encoding))
                    logits = (logits + mirrored_logits[0][::-1]) / 2
                column = cxnn.best_legal_move(logits, board.legal())
            outcome = int(dataset.value[i])
            hits[outcome][1] += 1
            hits[outcome][0] += int(optimal[i][column])

        def rate(outcome):
            good, total = hits[outcome]
            return f"{100.0 * good / total:5.1f}% ({total})" if total else "  n/a"

        both = (hits[1][0] + hits[0][0]) / max(1, hits[1][1] + hits[0][1])
        print(f"\nAccuracy against the exact solver on {count} positions (a move that keeps the game-theoretic result):")
        print(f"  winning positions {rate(1)}   drawn positions {rate(0)}   win+draw {100.0 * both:5.1f}%")

    if args.games > 0:
        player = Player(model, predict, args.shield, args.mirror)
        print(f"\nGames ({args.games} against each opponent, the network moves first in half of them):")
        for opponent in ("random", "greedy"):
            started = time.time()
            results = [play_game(player, opponent, network_first=(g % 2 == 0), rng=rng) for g in range(args.games)]
            wins, draws, losses = results.count(1), results.count(0), results.count(-1)
            score = (wins + 0.5 * draws) / args.games
            print(f"  vs {opponent:7s}: {wins:3d} wins, {draws:3d} draws, {losses:3d} losses  -> score {100.0 * score:5.1f}%   ({time.time() - started:.1f}s)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
