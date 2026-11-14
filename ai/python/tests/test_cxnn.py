"""Tests of cxnn.py against the C++ implementation (NumPy only, no TensorFlow needed).

Run from the ai/python directory:   python -m unittest discover -s tests -v

The fixtures in tests/data were produced by the C++ tools (connectx-train gen-data / supervised /
export-vectors): the test vectors contain boards, the network input built by the C++ encoder and the
exact outputs of the C++ network. Everything computed here in Python must match them.
"""

import json
import os
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import cxnn  # noqa: E402

DATA = os.path.join(os.path.dirname(__file__), "data")


def vectors(encoding: str):
    with open(os.path.join(DATA, f"tiny_{encoding}_vectors.json")) as f:
        return json.load(f)


class ModelFileTests(unittest.TestCase):
    def test_reads_the_cpp_model_files(self):
        for name, encoding_id in (("raw", 0), ("tactical", 1), ("windows", 2)):
            model = cxnn.load_model(os.path.join(DATA, f"tiny_{name}.cxnn"))
            self.assertEqual((model.rows, model.cols, model.connect), (6, 7, 4))
            self.assertEqual(model.encoding, encoding_id)
            self.assertEqual(model.encoding_name, name)
            self.assertEqual(model.hidden, [16, 16])
            self.assertEqual(model.policy_size, 7)
            self.assertEqual(model.input_size, cxnn.encoded_size(6, 7, 4, encoding_id))
            self.assertEqual(model.trunk[0].weights.shape, (model.input_size, 16))
            self.assertIn("supervised", model.metadata)

    def test_save_and_load_round_trip(self):
        model = cxnn.load_model(os.path.join(DATA, "tiny_tactical.cxnn"))
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "copy.cxnn")
            model.save(path)
            again = cxnn.load_model(path)

        self.assertEqual(again.metadata, model.metadata)
        self.assertEqual(again.encoding, model.encoding)
        for a, b in zip(model.trunk + [model.policy, model.value], again.trunk + [again.policy, again.value]):
            np.testing.assert_array_equal(a.weights, b.weights)
            np.testing.assert_array_equal(a.bias, b.bias)

    def test_rejects_garbage(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "bad.cxnn")
            with open(path, "wb") as f:
                f.write(b"nope" + bytes(100))
            with self.assertRaises(ValueError):
                cxnn.load_model(path)


class EncodingTests(unittest.TestCase):
    def test_python_encoders_match_the_cpp_ones(self):
        for encoding in ("raw", "tactical", "windows"):
            data = vectors(encoding)
            self.assertEqual(data["encoding"], encoding)
            for sample in data["samples"]:
                cells = np.array(sample["cells"], dtype=np.uint8)
                expected = np.array(sample["input"], dtype=np.float32)
                actual = cxnn.encode(cells, data["connect"], encoding)
                self.assertEqual(actual.shape, expected.shape)
                np.testing.assert_allclose(actual, expected, atol=1e-7, err_msg=f"{encoding} encoder differs from C++")

    def test_numpy_forward_matches_the_cpp_network(self):
        for encoding in ("raw", "tactical", "windows"):
            model = cxnn.load_model(os.path.join(DATA, f"tiny_{encoding}.cxnn"))
            data = vectors(encoding)
            x = np.array([s["input"] for s in data["samples"]], dtype=np.float32)
            expected_logits = np.array([s["logits"] for s in data["samples"]], dtype=np.float32)
            expected_value = np.array([s["value"] for s in data["samples"]], dtype=np.float32)

            logits, value = model.predict(x)
            np.testing.assert_allclose(logits, expected_logits, atol=2e-4, rtol=1e-4)
            np.testing.assert_allclose(value, expected_value, atol=2e-4, rtol=1e-4)

    def test_threat_and_playable_maps(self):
        # x to move; x has three on the bottom row (columns 0-2): column 3 completes four
        cells = np.zeros((6, 7), dtype=np.uint8)
        cells[5, 0:3] = 1
        cells[4, 0:3] = 2
        wins = cxnn.winning_cells(cells, 1, 4)
        self.assertTrue(wins[5, 3])
        self.assertEqual(int(wins.sum()), 1)
        playable = cxnn.playable_cells(cells)
        self.assertEqual(int(playable.sum()), 7)
        self.assertTrue(playable[3, 0] and playable[5, 6])


class DatasetTests(unittest.TestCase):
    def test_reads_the_cpp_dataset(self):
        dataset = cxnn.load_dataset(os.path.join(DATA, "tiny.cxds"))
        self.assertEqual(len(dataset), 200)
        self.assertEqual(dataset.cells.shape, (200, 6, 7))
        self.assertEqual(dataset.scores.shape, (200, 7))
        self.assertTrue(set(np.unique(dataset.value)) <= {-1, 0, 1})

        # Stones obey gravity, and the player to move has as many stones as the opponent or one fewer
        for cells in dataset.cells:
            mine, theirs = int((cells == 1).sum()), int((cells == 2).sum())
            self.assertIn(theirs - mine, (0, 1))
            for c in range(7):
                column = cells[:, c]
                filled = column != 0
                self.assertTrue(np.all(filled == np.sort(filled)), "floating stones")

        # Full columns are marked, and the value is the sign of the best score
        legal = dataset.cells[:, 0, :] == 0
        np.testing.assert_array_equal(dataset.scores == -128, ~legal)
        best = np.where(legal, dataset.scores.astype(int), -1000).max(axis=1)
        np.testing.assert_array_equal(np.sign(best), dataset.value)

    def test_outcome_targets(self):
        dataset = cxnn.load_dataset(os.path.join(DATA, "tiny.cxds"))
        targets, optimal = cxnn.outcome_targets(dataset)
        np.testing.assert_allclose(targets.sum(axis=1), 1.0, atol=1e-6)
        self.assertTrue(np.all(targets[~optimal] == 0))
        self.assertTrue(np.all(optimal.sum(axis=1) >= 1))


class BoardTests(unittest.TestCase):
    def test_vertical_horizontal_and_diagonal_wins(self):
        def play(sequence: str, rows=6, cols=7, connect=4) -> Board:
            board = cxnn.Board(rows, cols, connect)
            for ch in sequence:
                board.play(int(ch) - 1)
            return board

        Board = cxnn.Board
        self.assertEqual(play("1212121").winner, 1)
        self.assertEqual(play("1122334").winner, 1)
        self.assertEqual(play("12234334144").winner, 1)  # diagonal "/"
        self.assertEqual(play("76654554744").winner, 1)  # diagonal "\"
        self.assertEqual(play("11223", 4, 4, 3).winner, 1)  # connect 3 on a 4x4 board
        self.assertEqual(play("121212").winner, 0)

        board = play("112233")
        self.assertTrue(board.wins_by_playing(3))
        self.assertFalse(board.wins_by_playing(2))

    def test_random_games_agree_with_a_brute_force_check(self):
        rng = np.random.default_rng(1)
        for _ in range(60):
            board = cxnn.Board()
            while not board.is_over():
                legal = np.flatnonzero(board.legal())
                col = int(rng.choice(legal))
                predicted = board.wins_by_playing(col)
                self.assertEqual(board.play(col), predicted)

    def test_view_is_relative_to_the_player_to_move(self):
        board = cxnn.Board()
        board.play(3)  # first player
        view = board.view()  # seen by the second player: the stone belongs to the opponent
        self.assertEqual(view[5, 3], 2)
        board.play(2)
        view = board.view()
        self.assertEqual(view[5, 3], 1)
        self.assertEqual(view[5, 2], 2)

    def test_tactics_shield(self):
        board = cxnn.Board()
        for ch in "17273":  # x threatens column 4 and it is o's turn
            board.play(int(ch) - 1)
        self.assertEqual(cxnn.tactical_move(board), 3)

        board = cxnn.Board()
        for ch in "112233":  # x can win at once
            board.play(int(ch) - 1)
        self.assertEqual(cxnn.tactical_move(board), 3)
        self.assertIsNone(cxnn.tactical_move(cxnn.Board()))


if __name__ == "__main__":
    unittest.main()
