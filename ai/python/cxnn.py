"""Reading, writing and evaluating ConnectX network files (.cxnn) and datasets (.cxds).

This module only needs NumPy. It is the bridge between the C++ code (which trains the networks
from scratch) and TensorFlow / TensorFlow Lite (see export_tflite.py):

  * ``load_model`` / ``Model.save``   read and write the binary model format of ai/nn
  * ``Model.predict``                 a NumPy forward pass, bit-for-bit comparable with the C++ one
  * ``encode``                        builds the network input from a board, exactly like the C++ encoders
  * ``load_dataset``                  reads the solver-labelled positions written by `connectx-train gen-data`
  * ``Board``                         a tiny pure-Python Connect-X engine, to play games with a network

Model file format (all numbers little-endian)::

    char  magic[4] = "CXNN"
    u32   version                      1 or 2 (2 adds the encoding field)
    u32   board rows, board cols, connect length
    u32   encoding                     (version 2 only) 0 raw, 1 tactical, 2 windows
    u32   input size, policy size
    u32   metadata length, then that many bytes of UTF-8 text
    u32   number of dense layers
    for every layer:
        u32 role                       0 trunk, 1 policy head, 2 value head
        u32 activation                 0 linear, 1 relu, 2 tanh
        u32 inputs, u32 outputs
        f32 weights[inputs * outputs]  row-major [input][output] (the layout of Keras' Dense kernel)
        f32 bias[outputs]

Dataset file format (".cxds")::

    char  magic[4] = "CXDS";  u32 version = 1;  u32 rows, cols, connect;  u64 count
    count records of rows*cols + cols + 2 bytes:
        u8  cells[rows*cols]           row 0 = top; 0 empty, 1 stone of the player to move, 2 the opponent's
        i8  scores[cols]               solver score of playing each column, -128 = column full
        i8  value                      outcome for the player to move with perfect play: -1, 0, +1
        u8  reserved
"""

import struct
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import numpy as np

ENCODING_NAMES = {0: "raw", 1: "tactical", 2: "windows"}
ENCODING_IDS = {name: number for number, name in ENCODING_NAMES.items()}

ROLE_TRUNK, ROLE_POLICY, ROLE_VALUE = 0, 1, 2
ACT_LINEAR, ACT_RELU, ACT_TANH = 0, 1, 2


# ---------------------------------------------------------------------------
# Model files
# ---------------------------------------------------------------------------


@dataclass
class Layer:
    role: int
    activation: int
    weights: np.ndarray  # (inputs, outputs) float32
    bias: np.ndarray  # (outputs,) float32


@dataclass
class Model:
    rows: int
    cols: int
    connect: int
    encoding: int
    input_size: int
    policy_size: int
    metadata: str
    trunk: List[Layer] = field(default_factory=list)
    policy: Optional[Layer] = None
    value: Optional[Layer] = None

    @property
    def encoding_name(self) -> str:
        return ENCODING_NAMES.get(self.encoding, "?")

    @property
    def hidden(self) -> List[int]:
        return [layer.weights.shape[1] for layer in self.trunk]

    def parameter_count(self) -> int:
        layers = self.trunk + [self.policy, self.value]
        return int(sum(layer.weights.size + layer.bias.size for layer in layers))

    def predict(self, x: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """Forward pass for a batch of encoded positions, shape (N, input_size).

        Returns (policy logits (N, policy_size), value (N,)) as float32, using the same operations
        as the C++ network: Dense + ReLU layers, a linear policy head, a Dense + tanh value head.
        """
        h = np.asarray(x, dtype=np.float32)
        if h.ndim == 1:
            h = h[None, :]
        for layer in self.trunk:
            h = np.maximum(h @ layer.weights + layer.bias, 0.0)
        logits = h @ self.policy.weights + self.policy.bias
        value = np.tanh(h @ self.value.weights + self.value.bias)[:, 0]
        return logits.astype(np.float32), value.astype(np.float32)

    def save(self, path: str) -> None:
        layers = self.trunk + [self.policy, self.value]
        text = self.metadata.encode("utf-8")
        with open(path, "wb") as f:
            f.write(b"CXNN")
            f.write(struct.pack("<7I", 2, self.rows, self.cols, self.connect, self.encoding, self.input_size, self.policy_size))
            f.write(struct.pack("<I", len(text)))
            f.write(text)
            f.write(struct.pack("<I", len(layers)))
            for layer in layers:
                inputs, outputs = layer.weights.shape
                f.write(struct.pack("<4I", layer.role, layer.activation, inputs, outputs))
                f.write(np.ascontiguousarray(layer.weights, dtype="<f4").tobytes())
                f.write(np.ascontiguousarray(layer.bias, dtype="<f4").tobytes())


def random_model(rows: int, cols: int, connect: int, encoding, hidden: List[int], seed: int = 1) -> Model:
    """A freshly initialised network (He-normal trunk, small policy head), like the C++ constructor."""
    if isinstance(encoding, str):
        encoding = ENCODING_IDS[encoding]
    rng = np.random.default_rng(seed)
    input_size = encoded_size(rows, cols, connect, encoding)

    def dense(role, activation, inputs, outputs, scale=1.0, he=True):
        variance = 2.0 / inputs if he else 2.0 / (inputs + outputs)
        weights = rng.normal(0.0, np.sqrt(variance) * scale, size=(inputs, outputs)).astype(np.float32)
        return Layer(role, activation, weights, np.zeros(outputs, dtype=np.float32))

    model = Model(rows, cols, connect, encoding, input_size, cols, "")
    width = input_size
    for size in hidden:
        model.trunk.append(dense(ROLE_TRUNK, ACT_RELU, width, size))
        width = size
    model.policy = dense(ROLE_POLICY, ACT_LINEAR, width, cols, scale=0.1, he=False)
    model.value = dense(ROLE_VALUE, ACT_TANH, width, 1, he=False)
    return model


def load_model(path: str) -> Model:
    """Reads a ``.cxnn`` file (versions 1 and 2)."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"CXNN":
        raise ValueError(f"{path}: not a ConnectX model (bad magic number)")

    offset = 4

    def u32() -> int:
        nonlocal offset
        (value,) = struct.unpack_from("<I", data, offset)
        offset += 4
        return value

    version = u32()
    if version not in (1, 2):
        raise ValueError(f"{path}: unsupported model version {version}")

    rows, cols, connect = u32(), u32(), u32()
    encoding = u32() if version >= 2 else 0
    input_size, policy_size = u32(), u32()

    metadata_size = u32()
    metadata = data[offset : offset + metadata_size].decode("utf-8", errors="replace")
    offset += metadata_size

    model = Model(rows, cols, connect, encoding, input_size, policy_size, metadata)
    layer_count = u32()
    for _ in range(layer_count):
        role, activation, inputs, outputs = u32(), u32(), u32(), u32()
        count = inputs * outputs
        weights = np.frombuffer(data, dtype="<f4", count=count, offset=offset).reshape(inputs, outputs).copy()
        offset += 4 * count
        bias = np.frombuffer(data, dtype="<f4", count=outputs, offset=offset).copy()
        offset += 4 * outputs

        layer = Layer(role, activation, weights, bias)
        if role == ROLE_TRUNK:
            model.trunk.append(layer)
        elif role == ROLE_POLICY:
            model.policy = layer
        elif role == ROLE_VALUE:
            model.value = layer
        else:
            raise ValueError(f"{path}: unknown layer role {role}")

    if model.policy is None or model.value is None:
        raise ValueError(f"{path}: the model has no policy or value head")
    return model


# ---------------------------------------------------------------------------
# Datasets
# ---------------------------------------------------------------------------


@dataclass
class Dataset:
    rows: int
    cols: int
    connect: int
    cells: np.ndarray  # (N, rows, cols) uint8
    scores: np.ndarray  # (N, cols) int8, -128 for full columns
    value: np.ndarray  # (N,) int8

    def __len__(self) -> int:
        return len(self.value)


def load_dataset(path: str) -> Dataset:
    """Reads a ``.cxds`` file written by ``connectx-train gen-data``."""
    with open(path, "rb") as f:
        header = f.read(4 + 4 + 12 + 8)
        if header[:4] != b"CXDS":
            raise ValueError(f"{path}: not a ConnectX dataset")
        version, rows, cols, connect, count = struct.unpack_from("<4IQ", header, 4)
        if version != 1:
            raise ValueError(f"{path}: unsupported dataset version {version}")

        record = np.dtype([("cells", "u1", (rows, cols)), ("scores", "i1", (cols,)), ("value", "i1"), ("reserved", "u1")])
        array = np.fromfile(f, dtype=record, count=count)

    if len(array) != count:
        raise ValueError(f"{path}: truncated dataset")
    return Dataset(rows, cols, connect, array["cells"].copy(), array["scores"].copy(), array["value"].copy())


def outcome_targets(dataset: Dataset) -> Tuple[np.ndarray, np.ndarray]:
    """The policy targets used by the C++ trainer (``--target outcome``).

    Returns (targets (N, cols) float32 summing to 1 over the moves that keep the game-theoretic
    result, optimal mask (N, cols) bool). In a won position all winning moves; in a drawn one
    all drawing moves; in a lost one the moves that lose most slowly.
    """
    scores = dataset.scores.astype(np.int32)
    legal = scores != -128
    best = np.where(legal, scores, -1000).max(axis=1)

    same_class = np.sign(scores) == np.sign(best)[:, None]
    is_best = scores == best[:, None]
    optimal = legal & np.where((best >= 0)[:, None], same_class, is_best)

    targets = optimal.astype(np.float32)
    targets /= targets.sum(axis=1, keepdims=True)
    return targets, optimal


# ---------------------------------------------------------------------------
# Board and encodings (the Python twin of ai/src/encoding.cpp)
# ---------------------------------------------------------------------------


def _lines(rows: int, cols: int, connect: int):
    """Every line of `connect` cells, in the order used by the C++ code (Geometry::windows).

    Cells are (row_from_top, col) pairs.
    """
    steps = [(0, 1), (1, 0), (1, 1), (1, -1)]  # (d_col, d_row), row counted from the bottom
    windows = []
    for d_col, d_row in steps:
        for col in range(cols):
            for row_from_bottom in range(rows):
                end_col = col + d_col * (connect - 1)
                end_row = row_from_bottom + d_row * (connect - 1)
                if not (0 <= end_col < cols and 0 <= end_row < rows):
                    continue
                windows.append(
                    [(rows - 1 - (row_from_bottom + d_row * i), col + d_col * i) for i in range(connect)]
                )
    return windows


def winning_cells(cells: np.ndarray, player: int, connect: int) -> np.ndarray:
    """Boolean (rows, cols) map of the empty cells where `player` would complete `connect` in a row."""
    rows, cols = cells.shape
    result = np.zeros((rows, cols), dtype=bool)
    for r in range(rows):
        for c in range(cols):
            if cells[r, c] != 0:
                continue
            for dr, dc in ((0, 1), (1, 0), (1, 1), (1, -1)):
                run = 1
                for sign in (1, -1):
                    rr, cc = r + sign * dr, c + sign * dc
                    while 0 <= rr < rows and 0 <= cc < cols and cells[rr, cc] == player:
                        run += 1
                        rr += sign * dr
                        cc += sign * dc
                if run >= connect:
                    result[r, c] = True
                    break
    return result


def playable_cells(cells: np.ndarray) -> np.ndarray:
    """Boolean (rows, cols) map: the cell where a stone dropped in each non-full column lands."""
    rows, cols = cells.shape
    result = np.zeros((rows, cols), dtype=bool)
    for c in range(cols):
        for r in range(rows - 1, -1, -1):
            if cells[r, c] == 0:
                result[r, c] = True
                break
    return result


def encoded_size(rows: int, cols: int, connect: int, encoding: int) -> int:
    if encoding == 0:
        return rows * cols * 2
    if encoding == 1:
        return rows * cols * 5
    if encoding == 2:
        return rows * cols * 2 + 2 * len(_lines(rows, cols, connect))
    raise ValueError(f"unknown encoding {encoding}")


def encode(cells: np.ndarray, connect: int, encoding) -> np.ndarray:
    """Network input for a position, flattened as NHWC (``out[(row*cols + col)*planes + plane]``).

    :param cells: (rows, cols) array, row 0 = top; 0 empty, 1 stone of the player to move, 2 the opponent's
    :param encoding: 0/"raw", 1/"tactical" or 2/"windows" (see ai/include/ai/encoding.hpp for the meaning)
    """
    if isinstance(encoding, str):
        encoding = ENCODING_IDS[encoding]
    cells = np.asarray(cells)
    rows, cols = cells.shape

    mine = (cells == 1).astype(np.float32)
    theirs = (cells == 2).astype(np.float32)

    if encoding in (0, 2):
        planes = np.stack([mine, theirs], axis=-1).reshape(-1)
        if encoding == 0:
            return planes
        features = []
        for window in _lines(rows, cols, connect):
            my_stones = sum(1 for r, c in window if cells[r, c] == 1)
            their_stones = sum(1 for r, c in window if cells[r, c] == 2)
            features.append(my_stones / connect if their_stones == 0 else 0.0)
            features.append(their_stones / connect if my_stones == 0 else 0.0)
        return np.concatenate([planes, np.array(features, dtype=np.float32)])

    if encoding == 1:
        my_threats = winning_cells(cells, 1, connect).astype(np.float32)
        their_threats = winning_cells(cells, 2, connect).astype(np.float32)
        playable = playable_cells(cells).astype(np.float32)
        return np.stack([mine, theirs, my_threats, their_threats, playable], axis=-1).reshape(-1)

    raise ValueError(f"unknown encoding {encoding}")


def legal_columns(cells: np.ndarray) -> np.ndarray:
    """Boolean vector: which columns are not full."""
    return np.asarray(cells)[0, :] == 0


def best_legal_move(logits: np.ndarray, legal: np.ndarray) -> int:
    """The legal column with the highest score (ties: the lowest index)."""
    masked = np.where(legal, logits, -np.inf)
    return int(np.argmax(masked))


class Board:
    """A minimal Connect-X game, used to play games with a network from Python.

    ``cells`` holds absolute player numbers (1 = first player, 2 = second player), row 0 = top.
    ``view()`` gives the board as the network wants it: 1 = the player to move, 2 = the opponent.
    """

    def __init__(self, rows: int = 6, cols: int = 7, connect: int = 4):
        self.rows, self.cols, self.connect = rows, cols, connect
        self.cells = np.zeros((rows, cols), dtype=np.uint8)
        self.player = 1  # the player to move
        self.winner = 0  # 0 = nobody (yet)
        self.moves = 0

    def copy(self) -> "Board":
        other = Board(self.rows, self.cols, self.connect)
        other.cells = self.cells.copy()
        other.player, other.winner, other.moves = self.player, self.winner, self.moves
        return other

    def legal(self) -> np.ndarray:
        return self.cells[0, :] == 0

    def is_over(self) -> bool:
        return self.winner != 0 or self.moves == self.rows * self.cols

    def view(self) -> np.ndarray:
        """The board from the point of view of the player to move (1 = mine, 2 = the opponent's)."""
        other = 3 - self.player
        view = np.zeros_like(self.cells)
        view[self.cells == self.player] = 1
        view[self.cells == other] = 2
        return view

    def wins_by_playing(self, col: int) -> bool:
        """Would the player to move win by dropping a stone in `col`?"""
        row = self._landing_row(col)
        if row < 0:
            return False
        return self._completes_line(row, col, self.player)

    def play(self, col: int) -> bool:
        """Drops a stone for the player to move. Returns True if it wins the game."""
        row = self._landing_row(col)
        if row < 0 or self.is_over():
            raise ValueError(f"illegal move: column {col}")
        self.cells[row, col] = self.player
        self.moves += 1
        won = self._completes_line(row, col, self.player)
        if won:
            self.winner = self.player
        self.player = 3 - self.player
        return won

    def _landing_row(self, col: int) -> int:
        for r in range(self.rows - 1, -1, -1):
            if self.cells[r, col] == 0:
                return r
        return -1

    def _completes_line(self, row: int, col: int, player: int) -> bool:
        for dr, dc in ((0, 1), (1, 0), (1, 1), (1, -1)):
            run = 1
            for sign in (1, -1):
                r, c = row + sign * dr, col + sign * dc
                while 0 <= r < self.rows and 0 <= c < self.cols and self.cells[r, c] == player:
                    run += 1
                    r += sign * dr
                    c += sign * dc
            if run >= self.connect:
                return True
        return False

    def __str__(self) -> str:
        symbols = {0: ".", 1: "X", 2: "O"}
        lines = [" ".join(symbols[int(v)] for v in row) for row in self.cells]
        lines.append(" ".join(str(c + 1) for c in range(self.cols)))
        return "\n".join(lines)


def tactical_move(board: Board) -> Optional[int]:
    """The 1-move tactics shield: a winning column if there is one, else a column that blocks an
    opponent's immediate win, else None. (This is what the Arduino sketch does before / instead of
    asking the network when it is enabled.)"""
    legal = [c for c in range(board.cols) if board.legal()[c]]
    for c in legal:
        if board.wins_by_playing(c):
            return c
    opponent = board.copy()
    opponent.player = 3 - board.player
    for c in legal:
        if opponent.wins_by_playing(c):
            return c
    return None
