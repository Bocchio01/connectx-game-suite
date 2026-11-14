# ConnectX AI

Computer players for the ConnectX Game Suite, from a random mover to a neural network trained by self-play,
**all written from scratch** (no machine-learning library: the neural-network code is in [`nn/`](nn/README.md)).
It is meant to be read as much as to be used: every algorithm is a short, commented file.

* a ladder of **nine agents**, each one a step up from the previous: random → rules → minimax → alpha-beta →
  exact solver → Monte Carlo tree search → neural network → neural network + search → AlphaZero;
* a tournament tool, a training tool and a network client that lets any of them play on the ConnectX server;
* a pretrained network and the complete road to **TensorFlow Lite and the Arduino**
  ([`python/`](python/README.md), [`arduino/`](arduino/README.md)).

## Try it

```bash
cmake -S . -B build && cmake --build build -j      # (-DBUILD_GUI=OFF if you have no Qt and want a quiet configure)
cd build/bin

# 1. Play against the computer (three terminals; or just run ./connectx-gui --offline, see ui/gui)
./connectx-server
./connectx-cli -g "Duel<6 7 2 4>"                                   # you
./connectx-ai  -g Duel --agent alphabeta:depth=10,time=1000         # the computer

# 2. Watch the agents play each other: a round-robin tournament with Elo ratings
./connectx-arena -n 100 greedy alphabeta:depth=6 mcts:sims=2000,rollout=tactical az:model=connectx_6x7,sims=200

# 3. How good is a move picker, measured against the exact solver (positions it has never seen)
./connectx-train agent-accuracy --agent nnsearch:model=connectx_6x7,depth=3 --data models/test_positions_6x7.cxds
```

`connectx-ai --list-agents` describes every agent and its parameters. An agent is written `type:key=value,...`:
`random`, `greedy`, `minimax:depth=5`, `alphabeta:depth=10,time=1000`, `solver:time=3000`,
`mcts:sims=5000,rollout=tactical`, `nn:model=...`, `nnsearch:model=...,depth=3`, `az:model=...,sims=400`.
Models are looked up in `models/` next to the executables, so `connectx_6x7` is a complete model name.

## The ladder of agents

| # | Agent | The idea | Where |
|:-:|---|---|---|
| 0 | `random` | a legal move at random: the baseline | [`simple_agents.cpp`](src/agents/simple_agents.cpp) |
| 1 | `greedy` | one move of look-ahead: win now, else block the opponent's win, else avoid moves that hand him a win, else the centre | [`simple_agents.cpp`](src/agents/simple_agents.cpp) |
| 2 | `minimax` | explore every sequence of moves to a fixed depth and score the leaves with a hand-written evaluation (open lines) | [`minimax_agent.cpp`](src/agents/minimax_agent.cpp) |
| 3 | `alphabeta` | the same value as minimax, with **pruning** (skip branches that cannot matter), move ordering, exact tactics; with `time=` also iterative deepening and a transposition table | [`alphabeta_agent.cpp`](src/agents/alphabeta_agent.cpp) |
| 4 | `solver` | **perfect play**: exhaustive alpha-beta made fast by bitboards, a transposition table and null-window search. Time-limited, then falls back on 3 | [`solver.cpp`](src/solver.cpp) |
| 5 | `mcts` | **Monte Carlo tree search**: no evaluation function; play thousands of random games and keep statistics (UCT) | [`mcts_agent.cpp`](src/agents/mcts_agent.cpp) |
| 6 | `nn` | the **policy head** of the network alone: one forward pass, no search | [`neural_agent.cpp`](src/agents/neural_agent.cpp) |
| 7 | `nnsearch` | alpha-beta (level 3) whose evaluation function is the network's **value head** | [`neural_search_agent.cpp`](src/agents/neural_search_agent.cpp) |
| 8 | `az` | **AlphaZero**: Monte Carlo tree search (level 5) guided by both heads of the network (PUCT) | [`alphazero.cpp`](src/alphazero.cpp) |

Everything stands on a fast game representation, the bitboard [`Position`](include/ai/position.hpp) (a whole
6×7 board in two 64-bit words, win detection in a dozen instructions), which is checked against the reference
engine of `core/` by millions of random positions in the unit tests. Boards up to `cols × (rows + 1) ≤ 64` bits
(6×7, 7×7, 7×8, 5×9, 4×12...) with any connect length and **two players** are supported.

## How strong are the agents?

Two independent measurements, both made with tools of this repository (`connectx-arena` and `connectx-train agent-accuracy`).

**1. Playing against each other.** A round-robin tournament on the standard 6×7 board: every agent plays every other
agent 40 games (in pairs that share a random two-move opening, with the colours swapped), i.e. 560 games per agent,
4 minutes on a laptop. The Elo rating only tells how an agent does *against this field* (1000 = the average of the
field); "ms/move" is the average thinking time.

| # | Agent | Score | Elo | ms/move |
|:-:|---|--:|--:|--:|
| 1 | `az:model=connectx_6x7,sims=400` | 85.3% | 1342 | 7.9 |
| 2 | `mcts:sims=10000,rollout=tactical` | 76.4% | 1247 | 30 |
| 3 | `az:model=connectx_6x7,sims=100` | 68.6% | 1175 | 2.1 |
| 4 | `alphabeta:depth=14,time=200` | 68.4% | 1174 | 65 |
| 5 | `mcts:sims=1000,rollout=tactical` | 63.1% | 1129 | 3.5 |
| 6 | `nnsearch:model=connectx_6x7,depth=3` | 61.5% | 1116 | 1.8 |
| 7 | `nnsearch:model=connectx_6x7,depth=2` | 58.3% | 1090 | 0.65 |
| 8 | `alphabeta:depth=8` | 57.0% | 1079 | 6.3 |
| 9 | `alphabeta:depth=4` | 41.0% | 949 | 0.11 |
| 10 | `minimax:depth=4` | 40.7% | 947 | 0.55 |
| 11 | `mcts:sims=1000` | 39.0% | 932 | 1.4 |
| 12 | `nn:model=connectx_6x7,tactics=1` | 37.8% | 921 | 0.02 |
| 13 | `nnsearch:model=connectx_6x7,depth=1` | 34.2% | 890 | 0.12 |
| 14 | `greedy` | 18.0% | 713 | < 0.001 |
| 15 | `random` | 0.7% | 295 | < 0.001 |

What the table shows:

* **The ladder works**: `random` 295 < `greedy` 713 < the depth-4 searches ~950 < alpha-beta depth 8 1079 < ... < `az` 1342.
* **Pruning is free speed**: `minimax` and `alphabeta` of depth 4 play the same (947 and 949: they compute the same
  values), but alpha-beta needs a fifth of the time. Looking four moves further (depth 8, 6 ms per move) is worth 130 Elo.
* **The simulation policy matters more than the number of simulations**: MCTS with random rollouts and 1000
  simulations (932) is weaker than a depth-4 search; with the "tactical" rollout (take a win, avoid a move that loses
  at once) the same 1000 simulations reach 1129.
* **The network alone plays like a depth-4 search** (`nn`, 921) with one forward pass of 0.02 ms, five times faster.
  As the evaluation function of an alpha-beta search (`nnsearch`) it is much better: with 2 plies (1090) it equals the
  classic depth-8 search in a tenth of the time. (With a single ply the value head is no improvement on the policy
  head; the gain appears from two plies on.)
* **AlphaZero is the strongest**: `az` with 400 simulations (8 ms per move) beats every other agent, including MCTS
  with 25 times more simulations (70% of the points) and alpha-beta searching 14 plies for up to 200 ms (76%).
* Differences of a few tens of Elo (for example ranks 3 and 4) are within the noise of 40 games per pairing.

The exact solver (level 4) is not in the table: it plays perfectly, but only when it has time to finish (about 0.1 s
with 12 stones on the board, half a second to 6 seconds with 6-8 stones, 16 minutes for the empty board), and when the
time limit expires it falls back on alpha-beta, so in a timed tournament it would just be an alpha-beta of unknown depth.
Its job is to be the referee of the second measurement.

**2. Choosing the right move where the answer is known.** 2,396 positions that the networks never saw in training
([`models/test_positions_6x7.cxds`](models)), labelled by the exact solver. In a *winning* position (2,000 of them) a
move is right if the player still wins with perfect play afterwards; in a *drawn* position (396) if the draw is kept.
A random mover is not at zero: in many positions several moves are good, or the game is won whatever you do.

| Agent | Winning positions (2000) | Drawn positions (396) | ms/move |
|---|--:|--:|--:|
| `random` | 61.6% | 53.3% | < 0.001 |
| `greedy` | 81.2% | 74.7% | < 0.001 |
| `minimax:depth=4` | 89.8% | 82.1% | 0.53 |
| `alphabeta:depth=4` | 90.2% | 83.8% | 0.12 |
| `alphabeta:depth=8` | 92.9% | 85.9% | 6.1 |
| `alphabeta:depth=12,time=200` | 94.2% | 89.9% | 20 |
| `mcts:sims=1000` | 90.7% | 84.1% | 1.4 |
| `mcts:sims=1000,rollout=tactical` | 94.8% | 90.7% | 3.0 |
| `mcts:sims=10000,rollout=tactical` | 96.3% | 92.7% | 25 |
| `nn:model=connectx_6x7` | 88.8% | 85.4% | 0.02 |
| `nnsearch:model=connectx_6x7,depth=1` | 91.5% | 83.3% | 0.11 |
| `nnsearch:model=connectx_6x7,depth=2` | 94.1% | 87.4% | 0.59 |
| `nnsearch:model=connectx_6x7,depth=3` | 95.2% | 87.6% | 1.5 |
| `az:model=connectx_6x7,sims=100` | 96.2% | 91.4% | 2.1 |
| `az:model=connectx_6x7,sims=400` | 97.5% | 92.2% | 8.0 |

The network alone finds a good move in 89% of the winning positions, the same network inside a search in 94-95%, and
`az` in 97.5%: better than MCTS with 25 times more simulations (96.3%), in a third of the time. (Positions that are
already lost are not counted: there is nothing to preserve.)

## How the pieces work

### The exact solver (level 4)
Connect-4 is a solved game: with perfect play the first player wins by starting in the centre column. The
[`Solver`](include/ai/solver.hpp) computes the game-theoretic value of any position with negamax and alpha-beta,
and the ingredients that make it fast are the interesting part: (1) the bitboard position, where playing a move
and testing for a win are a few bit operations; (2) moves that lose at once are never searched, and a forced move
is played without searching; (3) moves that create the most threats are tried first; (4) a transposition table
remembers positions already analysed; (5) instead of one big search, a sequence of cheap *null-window* searches
("is the score above `m`?") narrows the score down by bisection. It is verified against an independent
exhaustive search on small boards and against a full-depth search of real positions, including the *speed* of
the win. Solving a position with 12 stones takes ~0.1 s on average and one with 8 stones a few seconds; the empty
board took 16 minutes (1.6 billion positions examined, on a laptop, with a 16-million-entry table) and confirmed
that the first player wins.

It also acts as the **teacher** of the neural networks: positions labelled by the solver are the training data.

### Alpha-beta and its relatives (levels 2-3)
Minimax assumes both players choose the best move for themselves; alpha-beta computes exactly the same value but
stops exploring a branch as soon as it is provably irrelevant: with good move ordering it needs about the square
root of the nodes. The test suite checks that `alphabeta` and `minimax` return *identical values* on thousands of
positions while `alphabeta` visits several times fewer nodes. The hand-written evaluation counts, for every line of
four cells, how many stones one player has in it when the other has none.

### Monte Carlo tree search (level 5)
No knowledge of the game but its rules. Four steps repeated thousands of times: **select** a path down the tree
following the UCT rule (average result + an exploration bonus for rarely tried moves), **expand** a leaf, **simulate**
a game to the end with a cheap policy, **back-propagate** the result. The simulation policy matters more than
anything else: `rollout=tactical` (take a win, avoid a move that loses at once) is dramatically stronger than random rollouts.

### Neural networks (levels 6-8)
A two-headed network ([`nn/`](nn/README.md)) reads the board and produces a *policy* (a score for each column) and
a *value* (who is winning). The **input encoding** ([`encoding.hpp`](include/ai/encoding.hpp)) is a design
decision of its own: with `raw` the network only sees two planes of stones and has to discover what a "three in a row"
is; with `tactical` (default) it is also shown the cells where either player would complete four and the playable
cells, so tactics become visible and the network can spend its capacity on strategy. On the same data this is worth
~3 points of accuracy.

What the experiments taught, and what the numbers above show:
* the **policy head alone is a modest player** (about as strong as a depth-4 search): even when it picks a
  result-preserving move ~89% of the time, the 11% of mistakes accumulate over a game;
* the **value head is much more useful**, and the classical search is the right tool to exploit it: `nnsearch` with
  the same network is far stronger than `nn`, and a depth of 2 is already on par with a classic alpha-beta of depth 8
  (which takes ten times longer to think);
* with a tree search driven by both heads (`az`) the network becomes a very strong player.

### AlphaZero (level 8)
[`AlphaZeroSearch`](include/ai/alphazero.hpp) is Monte Carlo tree search in which the network replaces both
weak points of the plain version: a new leaf is not played out but *evaluated by the value head*, and the policy head
gives every move a prior probability that steers the selection (the PUCT rule
`Q + c·P·√N / (1 + n)`). The visit counts at the root are a better policy than the one the network started from.

**Training by self-play** ([`selfplay.cpp`](src/training/selfplay.cpp)) closes the loop: the network plays games against itself
with this search (exploring with Dirichlet noise at the root and by sampling the early moves), every position is stored
with the visit counts as the policy target and the final result as the value target, and the network is trained on a
replay buffer of recent games. A better network gives a better search, which gives better targets. Our network starts
from the supervised one (below) and keeps a share of solver-labelled positions in every batch ("teacher" data), which
anchors the value head to the truth in the early iterations.

## Training your own network

```bash
cd build/bin
# 1. Labelled positions: games of a noisy alpha-beta player; the exact solver labels the positions
#    (win/draw/loss of every move). ~20 positions/s on 4 cores: this is the expensive step (50 minutes).
./connectx-train gen-data --out data.cxds --count 60000 --threads 4

# 2. Supervised learning: the network imitates the solver (a few minutes)
./connectx-train supervised --data data.cxds --out supervised.cxnn --hidden 256,256 --encoding tactical --epochs 45

# 3. Self-play reinforcement learning, starting from the supervised network and with teacher data mixed in
./connectx-train selfplay --out-dir run --init supervised.cxnn --teacher data.cxds --teacher-fraction 0.3 \
    --iterations 120 --games 300 --sims 200 --steps 400 --buffer 200000

# 4. Measure it
./connectx-arena -n 100 greedy alphabeta:depth=6 nnsearch:model=run/latest.cxnn,depth=2 az:model=run/latest.cxnn,sims=100
./connectx-train agent-accuracy --agent nn:model=run/latest.cxnn --data test.cxds

# 5. TensorFlow, TensorFlow Lite, Arduino: see python/README.md and arduino/README.md
```

`connectx-train --help` lists every command and option: `gen-data`, `supervised`, `selfplay`, `inspect`,
`dataset-info`, `merge-data`, `eval`, `agent-accuracy`, `export-vectors`. The pretrained model of this repository was
made with these commands (the data in four runs of 15,000 positions with different seeds, joined by `merge-data`, which
drops the duplicates): about an hour and three quarters on a 4-core laptop CPU, no GPU, of which 50 minutes of data
generation, 3 of supervised learning and 45 of self-play. Models and datasets are documented binary files
([`nn/README.md`](nn/README.md), [`python/cxnn.py`](python/cxnn.py)).

### What small experiments said

Before the long runs, each design question was settled with a short supervised experiment: the same 30,000 labelled
positions (27,000 to train on, 3,000 held out), 40 epochs, 1-3 minutes each. The last column is the share of held-out
positions where the network's best move preserves the game-theoretic result.

| Question | `--encoding` | `--hidden` | Parameters | Best move right |
|---|---|---|--:|--:|
| what the network sees | `raw`: my stones, his stones (84 inputs) | 128,128 | 28,424 | 84.5% |
| | `windows`: + how full each of the 69 lines of four is (222) | 128,128 | 46,088 | 86.6% |
| | `tactical`: + winning cells, playable cells (210) | 128,128 | 44,552 | 87.1% |
| how big | `tactical` | 64,64 | 18,184 | 84.3% |
| | `tactical` | 256,128,64 | 95,688 | 89.1% |
| | `tactical` | 256,256 (the one we use) | 121,864 | 89.9% |
| how much data | `tactical`, 60,000 positions instead of 30,000 | 256,256 | 121,864 | 90.0% |

Showing the network the tactics is worth about as much as doubling its width, and doubling the data barely moved the
needle: at this scale the limit is what an MLP can represent, not how many positions it has seen.

## Code map

| Path | Content |
|---|---|
| [`include/ai`](include/ai), [`src`](src) | the library `connectx_ai`: position, evaluation, solver, agents, arena, encoders, training |
| [`nn/`](nn/README.md) | the neural-network library written from scratch |
| [`app/`](app) | `connectx-ai` (network client), `connectx-arena`, `connectx-train` |
| [`python/`](python/README.md) | `.cxnn` ↔ NumPy / Keras / TensorFlow Lite, evaluation and play scripts |
| [`arduino/`](arduino/README.md) | the sketch that plays on a microcontroller |
| [`models/`](models) | the pretrained network, test vectors and held-out test positions |
| [`tests/`](tests) | unit tests (CTest): `ctest` runs them all |

The AI client ([`bot_client.cpp`](src/bot_client.cpp)) is a normal client of the server: it uses the same `Client`
class as the command-line and graphical interfaces, so a computer player is indistinguishable from a human one.

## Things to try

The code is small enough to be changed, and the tools tell you what the change did:

1. **Break the pruning.** [`alphabeta_agent.cpp`](src/agents/alphabeta_agent.cpp) tries the columns from the centre
   outwards ([`position.hpp`](include/ai/position.hpp)). Try them left to right and compare the nodes per move
   (`connectx-ai -v`) and the thinking time of `alphabeta:depth=8`: alpha-beta is only as good as its move ordering.
2. **Write an agent.** Derive from `Agent` ([`agent.hpp`](include/ai/agent.hpp): `name()`, `chooseMove()`, `seed()`) and
   add it to `createAgent()` and `availableAgents()` in [`factory.cpp`](src/factory.cpp): it is then available to
   `connectx-arena`, `connectx-ai` and `agent-accuracy`. Good first candidates: a 1-ply agent that maximises the
   evaluation function of level 2, or MCTS with a rollout of your own.
3. **Tune the search.** Pit `az:model=connectx_6x7,sims=200,c=0.5` against `c=1.5` and `c=4` in the arena, or do the same
   with the exploration constant `c` of `mcts`: the best values are not obvious.
4. **Change what the network sees or learns.** Train with `--encoding raw` or `--hidden 32,32`, or with `--target best` /
   `--target soft` instead of `outcome` (`connectx-train supervised --help`), and compare with `agent-accuracy`: the
   table of experiments above was made exactly like this.
5. **Self-play without a teacher.** `--teacher-fraction 0` removes the solver-labelled positions from the batches and
   leaving out `--init` starts from a random network: how many iterations does it take to beat `greedy`?
6. **Another board.** Add `--rows 5 --cols 6` to `connectx-arena` and to `gen-data` / `selfplay` (the bot plays
   whatever board its game has): on small boards the exact solver finishes quickly, so you can compare a network
   trained for that board with perfect play (`solver`).
7. **Squeeze it onto the Arduino.** Train with `--hidden 64,64` (7 times cheaper) and see how much strength a deeper
   `SEARCH_DEPTH` buys back ([`arduino/`](arduino/README.md)).

## Limits

* Two-player games, on boards that fit the bitboard (`cols × (rows + 1) ≤ 64`); the AI client refuses other games with
  an explanation. The pretrained network is for the standard 6×7 board with connect 4; the search agents work on any
  supported board and `connectx-train` can train a network for any of them.
* The networks are multilayer perceptrons: there are no convolutions (they would be ~100 times slower to train on a CPU).
* Positions with fewer than 8 stones cannot be labelled by the solver in reasonable time: the supervised data covers
  the middle game and the endgame, the opening is learned by self-play.
