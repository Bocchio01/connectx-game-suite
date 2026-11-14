#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "nn/network.hpp"

namespace nn
{
    namespace
    {
        // ---- model file format -------------------------------------------------

        constexpr char kMagic[4] = {'C', 'X', 'N', 'N'};
        constexpr uint32_t kVersion = 2; // v1 files (no encoding field) are still readable

        enum Role : uint32_t
        {
            ROLE_TRUNK = 0,
            ROLE_POLICY = 1,
            ROLE_VALUE = 2
        };

        enum Activation : uint32_t
        {
            ACT_LINEAR = 0,
            ACT_RELU = 1,
            ACT_TANH = 2
        };

        void writeU32(std::ostream &out, uint32_t value)
        {
            const unsigned char bytes[4] = {
                static_cast<unsigned char>(value & 0xFF),
                static_cast<unsigned char>((value >> 8) & 0xFF),
                static_cast<unsigned char>((value >> 16) & 0xFF),
                static_cast<unsigned char>((value >> 24) & 0xFF)};
            out.write(reinterpret_cast<const char *>(bytes), 4);
        }

        void writeF32(std::ostream &out, float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            writeU32(out, bits);
        }

        uint32_t readU32(std::istream &in)
        {
            unsigned char bytes[4];
            in.read(reinterpret_cast<char *>(bytes), 4);
            if (!in)
                throw std::runtime_error("Model file: unexpected end of data");
            return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                   (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
        }

        float readF32(std::istream &in)
        {
            const uint32_t bits = readU32(in);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        void writeDense(std::ostream &out, const Dense &layer, Role role, Activation activation)
        {
            writeU32(out, role);
            writeU32(out, activation);
            writeU32(out, static_cast<uint32_t>(layer.inputSize()));
            writeU32(out, static_cast<uint32_t>(layer.outputSize()));

            const Real *w = layer.weight().value.data();
            for (size_t i = 0; i < layer.weight().value.size(); ++i)
                writeF32(out, static_cast<float>(w[i]));

            const Real *b = layer.bias().value.data();
            for (size_t i = 0; i < layer.bias().value.size(); ++i)
                writeF32(out, static_cast<float>(b[i]));
        }

        // ---- single-sample inference ------------------------------------------

        /** y = x W + b for one sample, skipping the zero inputs. */
        void denseForward(const Dense &layer, const Real *x, Real *y)
        {
            const size_t inputs = layer.inputSize();
            const size_t outputs = layer.outputSize();
            const Real *w = layer.weight().value.data();
            const Real *b = layer.bias().value.data();

            for (size_t j = 0; j < outputs; ++j)
                y[j] = b[j];

            for (size_t k = 0; k < inputs; ++k)
            {
                const Real xk = x[k];
                if (xk == 0)
                    continue;

                const Real *row = w + k * outputs;
                for (size_t j = 0; j < outputs; ++j)
                    y[j] += xk * row[j];
            }
        }
    }

    // ========================================================================
    // Construction
    // ========================================================================

    PolicyValueNet::PolicyValueNet(const NetworkConfig &config, uint64_t seed)
        : config_(config)
    {
        if (config.input_size == 0 || config.policy_size == 0)
            throw std::invalid_argument("PolicyValueNet: input_size and policy_size must be positive");

        std::mt19937_64 rng(seed);

        size_t width = config.input_size;
        for (size_t hidden : config.hidden)
        {
            if (hidden == 0)
                throw std::invalid_argument("PolicyValueNet: hidden layers must not be empty");

            // He initialisation: the right scale for weights followed by a ReLU
            trunk_dense_.push_back(&trunk_.add<Dense>(width, hidden, rng, Init::HE_NORMAL));
            trunk_.add<ReLU>();
            width = hidden;
        }

        // Small initial weights in the policy head: the untrained network starts with an almost uniform policy
        policy_head_ = std::make_unique<Dense>(width, config.policy_size, rng, Init::XAVIER_NORMAL, Real(0.1));
        value_head_ = std::make_unique<Dense>(width, 1, rng, Init::XAVIER_NORMAL);
    }

    // ========================================================================
    // Training
    // ========================================================================

    void PolicyValueNet::forward(const Matrix &input, Matrix &logits, Matrix &values)
    {
        const Matrix &features = trunk_.forward(input);

        logits = policy_head_->forward(features);
        values = value_activation_.forward(value_head_->forward(features));
    }

    void PolicyValueNet::backward(const Matrix &dlogits, const Matrix &dvalues)
    {
        // Both heads read the same features: their gradients add up (multivariate chain rule)
        const Matrix &dvalue_raw = value_activation_.backward(dvalues);
        const Matrix &dfeatures_value = value_head_->backward(dvalue_raw);
        const Matrix &dfeatures_policy = policy_head_->backward(dlogits);

        grad_features_.resize(dfeatures_policy.rows(), dfeatures_policy.cols());
        for (size_t i = 0; i < grad_features_.size(); ++i)
            grad_features_.data()[i] = dfeatures_policy.data()[i] + dfeatures_value.data()[i];

        if (trunk_.size() > 0)
            trunk_.backward(grad_features_);
    }

    std::vector<Parameter *> PolicyValueNet::parameters()
    {
        std::vector<Parameter *> result;
        trunk_.collectParameters(result);
        policy_head_->collectParameters(result);
        value_head_->collectParameters(result);
        return result;
    }

    void PolicyValueNet::zeroGrad()
    {
        for (Parameter *p : parameters())
            p->zeroGrad();
    }

    size_t PolicyValueNet::parameterCount() const
    {
        size_t count = 0;
        for (const Dense *layer : trunk_dense_)
            count += layer->weight().value.size() + layer->bias().value.size();
        count += policy_head_->weight().value.size() + policy_head_->bias().value.size();
        count += value_head_->weight().value.size() + value_head_->bias().value.size();
        return count;
    }

    void PolicyValueNet::copyWeightsFrom(const PolicyValueNet &other)
    {
        auto mine = parameters();
        auto theirs = const_cast<PolicyValueNet &>(other).parameters();
        if (mine.size() != theirs.size())
            throw std::invalid_argument("copyWeightsFrom: the networks have a different shape");

        for (size_t i = 0; i < mine.size(); ++i)
        {
            if (mine[i]->value.size() != theirs[i]->value.size())
                throw std::invalid_argument("copyWeightsFrom: the networks have a different shape");
            mine[i]->value = theirs[i]->value;
        }
    }

    // ========================================================================
    // Inference
    // ========================================================================

    void PolicyValueNet::predict(const Real *input, Real *logits, Real *value) const
    {
        // Scratch space for the activations of the hidden layers. It lives on the stack: no
        // allocation per call and no thread_local objects (whose destructors misbehave when the
        // thread exits on some MinGW-w64 runtimes). Unusually wide layers fall back on the heap.
        constexpr size_t kStackWidth = 1024;
        Real stack_a[kStackWidth];
        Real stack_b[kStackWidth];
        std::vector<Real> heap_a, heap_b;

        size_t widest = 0;
        for (size_t hidden : config_.hidden)
            widest = std::max(widest, hidden);

        Real *buffer_a = stack_a;
        Real *buffer_b = stack_b;
        if (widest > kStackWidth)
        {
            heap_a.resize(widest);
            heap_b.resize(widest);
            buffer_a = heap_a.data();
            buffer_b = heap_b.data();
        }

        const Real *current = input;
        Real *next = buffer_a;

        for (const Dense *layer : trunk_dense_)
        {
            denseForward(*layer, current, next);
            for (size_t j = 0; j < layer->outputSize(); ++j)
                next[j] = next[j] > 0 ? next[j] : 0; // ReLU

            current = next;
            next = (next == buffer_a) ? buffer_b : buffer_a;
        }

        denseForward(*policy_head_, current, logits);

        Real raw = 0;
        denseForward(*value_head_, current, &raw);
        *value = std::tanh(raw);
    }

    // ========================================================================
    // Persistence
    // ========================================================================

    void PolicyValueNet::save(std::ostream &out, const std::string &metadata) const
    {
        out.write(kMagic, 4);
        writeU32(out, kVersion);
        writeU32(out, config_.board_rows);
        writeU32(out, config_.board_cols);
        writeU32(out, config_.connect);
        writeU32(out, config_.encoding);
        writeU32(out, static_cast<uint32_t>(config_.input_size));
        writeU32(out, static_cast<uint32_t>(config_.policy_size));

        writeU32(out, static_cast<uint32_t>(metadata.size()));
        out.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));

        writeU32(out, static_cast<uint32_t>(trunk_dense_.size() + 2));
        for (const Dense *layer : trunk_dense_)
            writeDense(out, *layer, ROLE_TRUNK, ACT_RELU);
        writeDense(out, *policy_head_, ROLE_POLICY, ACT_LINEAR);
        writeDense(out, *value_head_, ROLE_VALUE, ACT_TANH);

        if (!out)
            throw std::runtime_error("Failed to write the model");
    }

    void PolicyValueNet::save(const std::string &path, const std::string &metadata) const
    {
        std::ofstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open '" + path + "' for writing");
        save(file, metadata);
    }

    PolicyValueNet PolicyValueNet::load(std::istream &in, std::string *metadata)
    {
        char magic[4];
        in.read(magic, 4);
        if (!in || std::memcmp(magic, kMagic, 4) != 0)
            throw std::runtime_error("Not a ConnectX model file (bad magic number)");

        const uint32_t version = readU32(in);
        if (version != 1 && version != kVersion)
            throw std::runtime_error("Unsupported model file version " + std::to_string(version));

        NetworkConfig config;
        config.board_rows = readU32(in);
        config.board_cols = readU32(in);
        config.connect = readU32(in);
        config.encoding = version >= 2 ? readU32(in) : 0;
        config.input_size = readU32(in);
        config.policy_size = readU32(in);

        const uint32_t metadata_size = readU32(in);
        if (metadata_size > (1u << 24))
            throw std::runtime_error("Model file: implausible metadata size");
        std::string text(metadata_size, '\0');
        if (metadata_size > 0)
        {
            in.read(&text[0], static_cast<std::streamsize>(metadata_size));
            if (!in)
                throw std::runtime_error("Model file: unexpected end of data");
        }
        if (metadata != nullptr)
            *metadata = text;

        const uint32_t layer_count = readU32(in);
        if (layer_count < 2 || layer_count > 64)
            throw std::runtime_error("Model file: implausible number of layers");

        // First pass over the layer headers is not possible without the weights in between,
        // so the layers are read into temporary buffers and validated while reading.
        struct LayerData
        {
            uint32_t role, activation, inputs, outputs;
            std::vector<float> weights, bias;
        };
        std::vector<LayerData> layers(layer_count);

        for (LayerData &layer : layers)
        {
            layer.role = readU32(in);
            layer.activation = readU32(in);
            layer.inputs = readU32(in);
            layer.outputs = readU32(in);

            if (layer.inputs == 0 || layer.outputs == 0 || layer.inputs > (1u << 20) || layer.outputs > (1u << 20) ||
                static_cast<uint64_t>(layer.inputs) * layer.outputs > (1u << 26))
                throw std::runtime_error("Model file: implausible layer size");

            layer.weights.resize(static_cast<size_t>(layer.inputs) * layer.outputs);
            for (float &w : layer.weights)
                w = readF32(in);
            layer.bias.resize(layer.outputs);
            for (float &b : layer.bias)
                b = readF32(in);
        }

        // Structure: trunk layers (ReLU), then the policy head (linear), then the value head (tanh)
        const size_t trunk_count = layer_count - 2;
        config.hidden.clear(); // (NetworkConfig has a default width: start from scratch)
        size_t width = config.input_size;
        for (size_t i = 0; i < trunk_count; ++i)
        {
            const LayerData &layer = layers[i];
            if (layer.role != ROLE_TRUNK || layer.activation != ACT_RELU || layer.inputs != width)
                throw std::runtime_error("Model file: unexpected trunk layer");
            config.hidden.push_back(layer.outputs);
            width = layer.outputs;
        }

        const LayerData &policy = layers[trunk_count];
        const LayerData &value = layers[trunk_count + 1];
        if (policy.role != ROLE_POLICY || policy.activation != ACT_LINEAR || policy.inputs != width || policy.outputs != config.policy_size)
            throw std::runtime_error("Model file: unexpected policy head");
        if (value.role != ROLE_VALUE || value.activation != ACT_TANH || value.inputs != width || value.outputs != 1)
            throw std::runtime_error("Model file: unexpected value head");

        PolicyValueNet net(config, 1);
        std::vector<Parameter *> params = net.parameters();
        assert(params.size() == 2 * layer_count);

        for (size_t i = 0; i < layer_count; ++i)
        {
            Parameter *weight = params[2 * i];
            Parameter *bias = params[2 * i + 1];
            for (size_t k = 0; k < weight->value.size(); ++k)
                weight->value.data()[k] = static_cast<Real>(layers[i].weights[k]);
            for (size_t k = 0; k < bias->value.size(); ++k)
                bias->value.data()[k] = static_cast<Real>(layers[i].bias[k]);
        }

        return net;
    }

    PolicyValueNet PolicyValueNet::load(const std::string &path, std::string *metadata)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open model file '" + path + "'");
        return load(file, metadata);
    }
}
