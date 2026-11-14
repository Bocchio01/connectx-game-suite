# nn: a neural network library written from scratch

A small deep-learning library with **no dependencies** (not even for the linear algebra): matrices,
layers with hand-written backpropagation, losses, the SGD and Adam optimisers, a two-headed
policy/value network and its file format. About 1,300 lines of C++17, written to be read.

It knows nothing about Connect-X: the game specific code (encoders, trainers, agents) lives one level up
in [`ai/`](../README.md) and just *uses* it. It is the engine behind the neural agents `nn`, `nnsearch`
and `az`, and what `connectx-train` trains.

| File | What it contains |
|---|---|
| [`matrix.hpp`](include/nn/matrix.hpp) / [`matrix.cpp`](src/matrix.cpp) | row-major `Matrix` and the three matrix products a network needs: `A·B`, `Aᵀ·B`, `A·Bᵀ` |
| [`layers.hpp`](include/nn/layers.hpp) / [`layers.cpp`](src/layers.cpp) | `Dense`, `ReLU`, `Tanh`, `Sequential`; every layer has `forward()` and `backward()` |
| [`loss.hpp`](include/nn/loss.hpp) / [`loss.cpp`](src/loss.cpp) | softmax, cross-entropy for the policy, mean squared error for the value |
| [`optimizer.hpp`](include/nn/optimizer.hpp) / [`optimizer.cpp`](src/optimizer.cpp) | SGD with momentum, Adam (AdamW), gradient clipping |
| [`network.hpp`](include/nn/network.hpp) / [`network.cpp`](src/network.cpp) | `PolicyValueNet`: the two-headed network, batched training and single-sample inference, `.cxnn` files |

## The ideas, in 5 minutes

**A layer is a function with memory.** `forward(x)` computes the output and remembers what the backward
pass will need; `backward(dY)` receives the gradient of the loss with respect to the layer's output and
returns the gradient with respect to its input, adding the gradient of its own parameters to `Parameter::grad`.
Chaining the `backward` calls in reverse order *is* backpropagation: the chain rule, one layer at a time.

**Dense** computes `Y = X·W + b` for a batch `X` (one sample per row). With `dY = ∂L/∂Y`:

```
dW = Xᵀ · dY        (the sum over the batch of the outer products x·dyᵀ)
db = column sums of dY
dX = dY · Wᵀ
```

**ReLU** (`max(0, x)`) passes the gradient only where the input was positive. **Tanh** has derivative
`1 − tanh²(x)`: the value head uses it to squash its output to (−1, 1).

**Policy loss.** The network outputs one score ("logit") per column. Softmax turns them into
probabilities, cross-entropy compares them with the target distribution. The gradient of the pair is
famously simple: `∂L/∂logitⱼ = pⱼ·Σt − tⱼ` (just `p − t` for a proper distribution). Illegal columns can be
masked out of the softmax. **Value loss** is the mean squared error against the game outcome.

**Training** is mini-batch gradient descent: forward pass, losses, backward pass, an optimiser step.
`SGD` keeps a running average of the gradients (momentum); `Adam` also rescales each weight by the history
of its gradient size, which makes the learning rate much less delicate. Weights are initialised with
He's rule `N(0, 2/fan_in)` in front of ReLUs, Xavier's rule for linear and tanh outputs.

**Two heads, one trunk.** `PolicyValueNet` is the network of AlphaZero, scaled down to an MLP:

```
                         ┌─▶ Dense(columns) ───────────────▶ policy logits
 input ─▶ [Dense+ReLU]ⁿ ─┤
                         └─▶ Dense(1) ─▶ tanh ─────────────▶ value in (−1, 1)
```

Both heads read the same features, so their gradients **add up** at the end of the trunk (the multivariate
chain rule) — see `PolicyValueNet::backward`.

## Is the backpropagation right? Gradient checking

[`tests/test_gradients.cpp`](tests/test_gradients.cpp) compares, for every parameter `w` of several networks,
the gradient computed by `backward()` with the numerical derivative of the loss
`(L(w+h) − L(w−h)) / 2h`. For this test the library is rebuilt with `-DNN_REAL_TYPE=double` (the library is
`float` otherwise, like TensorFlow Lite), so rounding noise does not hide bugs. Measured worst relative
error: **6·10⁻⁷** over `Dense`/`Tanh`/`ReLU`, the masked softmax cross-entropy, the MSE and the whole two-headed
network (with and without trunk). [`tests/test_basics.cpp`](tests/test_basics.cpp) also checks the matrix kernels
against naive loops, that the optimisers learn a line and XOR, that single-sample `predict()` equals the batched
forward pass and that models survive a save/load round trip.

## Using it

```cpp
#include "nn/network.hpp"
#include "nn/loss.hpp"
#include "nn/optimizer.hpp"

nn::NetworkConfig config;
config.input_size = 210;          // what the encoder produces
config.hidden = {256, 256};
config.policy_size = 7;           // one score per column
nn::PolicyValueNet net(config, /*seed=*/1);
nn::Adam optimizer(1e-3f);
auto parameters = net.parameters();

nn::Matrix x(batch, 210), policy_target(batch, 7), value_target(batch, 1);   // ... fill them ...
nn::Matrix logits, values, dlogits, dvalues;

net.zeroGrad();
net.forward(x, logits, values);
nn::softmaxCrossEntropy(logits, policy_target, dlogits);
nn::meanSquaredError(values, value_target, dvalues);
net.backward(dlogits, dvalues);
optimizer.step(parameters);

float scores[7], value;
net.predict(x.row(0), scores, &value);          // one sample, no allocation, thread-safe
net.save("model.cxnn", "free text stored in the file");
```

## Performance notes

* Inputs are sparse (a board has at most 42 stones) and so are ReLU activations: the products skip
  zero entries, which roughly halves the cost of the first layer.
* The loops are laid out so that the innermost one walks contiguous memory, and the compiler vectorises it.
  A training step of the default 256+256 network on a batch of 256 takes ~18 ms on one core; one evaluation
  of a position takes ~25 µs.
* `predict()` keeps its scratch space on the stack (no `thread_local`, no allocation) and only reads the
  weights, so many threads can evaluate the same network at once (this is what makes parallel self-play easy).
* Not in the library, on purpose: convolutions, batch normalisation, dropout, GPU support. A convolutional
  network is the "right" architecture for a board game, but on one CPU core it would be ~100 times slower to
  train; the extra input planes of the `tactical` encoding give the MLP the pattern information a convolution
  would have to discover.

## The `.cxnn` model file

Binary, little-endian. This is all you need to read it in any language (`ai/python/cxnn.py` is a ~40-line
reference reader).

```
char  magic[4] = "CXNN"
u32   version                      2   (version 1 files, without the encoding field, are still readable)
u32   board rows, board cols, connect length      (the game the network was trained for; 0 = unknown)
u32   encoding                     how the board is turned into the input: 0 raw, 1 tactical, 2 windows
u32   input size, policy size
u32   metadata length, then that many bytes of UTF-8 text      (how the model was trained)
u32   number of dense layers
repeated for every layer, in this order: the trunk layers, the policy head, the value head
    u32 role                       0 trunk, 1 policy head, 2 value head
    u32 activation                 0 linear, 1 ReLU, 2 tanh
    u32 inputs, u32 outputs
    f32 weights[inputs * outputs]  row-major: weights[input][output]
    f32 bias[outputs]
```

The weights use the layout of a **Keras `Dense` kernel** (`inputs × outputs`, `y = x·W + b`): exporting to
TensorFlow is a plain copy (see [`ai/python`](../python/README.md)).
