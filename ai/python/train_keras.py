#!/usr/bin/env python3
"""Train a ConnectX network with TensorFlow / Keras on solver-labelled positions.

    python train_keras.py --dataset data.cxds --out keras_model.cxnn --hidden 256,256 --encoding tactical

This is the "TensorFlow-native" alternative to the C++ trainer (`connectx-train supervised`): the same
data, the same architecture, the same losses, but the gradient descent is done by Keras. The result is
written as a .cxnn file, so the C++ tools (`connectx-arena`, the AI client, the GUI) and
export_tflite.py can use it like any model trained in C++.

Datasets are produced by `connectx-train gen-data`. Everything is learned from the solver's verdicts:
the policy head imitates the moves that keep the game-theoretic result, the value head predicts the
result (the same "outcome" targets as the C++ trainer).
"""

import argparse
import sys

import numpy as np

import cxnn
import tfbridge


def build_arrays(datasets, encoding: int):
    """Encoded inputs, policy targets, value targets, optimal-move masks and legal-move masks, with the
    left-right mirror image of every position added right after it."""
    inputs, policy, value, optimal, legal = [], [], [], [], []
    for dataset in datasets:
        targets, mask = cxnn.outcome_targets(dataset)
        for i in range(len(dataset)):
            cells = dataset.cells[i]
            inputs.append(cxnn.encode(cells, dataset.connect, encoding))
            policy.append(targets[i])
            optimal.append(mask[i])
            legal.append(cells[0, :] == 0)
            value.append(float(dataset.value[i]))

            # The game is symmetric: the mirrored board has the mirrored answer
            inputs.append(cxnn.encode(cells[:, ::-1], dataset.connect, encoding))
            policy.append(targets[i][::-1])
            optimal.append(mask[i][::-1])
            legal.append(cells[0, ::-1] == 0)
            value.append(float(dataset.value[i]))

    return (np.array(inputs, dtype=np.float32), np.array(policy, dtype=np.float32),
            np.array(value, dtype=np.float32), np.array(optimal, dtype=bool), np.array(legal, dtype=bool))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dataset", required=True, help=".cxds file(s), comma separated")
    parser.add_argument("--out", required=True, help="the .cxnn model to write")
    parser.add_argument("--hidden", default="256,256", help="hidden layer widths (default 256,256)")
    parser.add_argument("--encoding", default="tactical", choices=["raw", "tactical", "windows"])
    parser.add_argument("--init", help="start from this .cxnn model instead of a random network")
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch", type=int, default=256)
    parser.add_argument("--lr", type=float, default=2e-3)
    parser.add_argument("--validation", type=float, default=0.05)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    tf = tfbridge._tf()
    keras = tf.keras
    tf.random.set_seed(args.seed)

    datasets = [cxnn.load_dataset(path) for path in args.dataset.split(",")]
    first = datasets[0]

    if args.init:
        template = cxnn.load_model(args.init)
    else:
        template = cxnn.random_model(first.rows, first.cols, first.connect, args.encoding,
                                     [int(h) for h in args.hidden.split(",")], seed=args.seed)

    x, policy, value, optimal, legal = build_arrays(datasets, template.encoding)
    print(f"{len(x)} training examples (with mirror images), {template.input_size} inputs, "
          f"{template.parameter_count()} parameters")

    # Hold out a validation set (whole positions: a position and its mirror stay together)
    order = np.random.default_rng(args.seed).permutation(len(x) // 2)
    validation_pairs = max(1, int(args.validation * len(order)))
    validation_index = np.concatenate([[2 * i, 2 * i + 1] for i in order[:validation_pairs]])
    train_index = np.concatenate([[2 * i, 2 * i + 1] for i in order[validation_pairs:]])

    keras_model = tfbridge.build_keras_model(template)

    steps = max(1, len(train_index) // args.batch) * args.epochs
    schedule = keras.optimizers.schedules.CosineDecay(args.lr, steps, alpha=0.05)
    keras_model.compile(
        optimizer=keras.optimizers.Adam(schedule),
        loss={
            # cross-entropy between the softmax of the logits and the target distribution
            "policy": keras.losses.CategoricalCrossentropy(from_logits=True),
            "value": keras.losses.MeanSquaredError(),
        },
    )

    keras_model.fit(
        x[train_index], {"policy": policy[train_index], "value": value[train_index][:, None]},
        validation_data=(x[validation_index], {"policy": policy[validation_index], "value": value[validation_index][:, None]}),
        epochs=args.epochs, batch_size=args.batch, shuffle=True, verbose=2,
    )

    # The metric that matters: does the best legal move keep the game-theoretic result?
    logits, predicted_value = tfbridge.keras_predict(keras_model, x[validation_index])
    best = np.argmax(np.where(legal[validation_index], logits, -np.inf), axis=1)
    rate = optimal[validation_index][np.arange(len(best)), best].mean()
    outcome_accuracy = (np.clip(np.round(predicted_value), -1, 1) == value[validation_index]).mean()
    print(f"validation: plays a result-preserving move in {100.0 * rate:.1f}% of the positions, "
          f"value rounded to win/draw/loss correct in {100.0 * outcome_accuracy:.1f}%")

    result = tfbridge.keras_to_cxnn(keras_model, template, f"trained with Keras: {len(train_index)} examples, {args.epochs} epochs")
    result.save(args.out)
    print(f"Saved {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
