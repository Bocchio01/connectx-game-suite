#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_set>

#include "ai/agents/alphabeta_agent.hpp"
#include "ai/solver.hpp"
#include "ai/training/dataset.hpp"

namespace ai
{
    namespace training
    {
        static_assert(std::is_same<nn::Real, float>::value, "the training code assumes float networks");

        namespace
        {
            constexpr char kMagic[4] = {'C', 'X', 'D', 'S'};
            constexpr uint32_t kVersion = 1;

            int sign(int value) { return (value > 0) - (value < 0); }

            void writeU32(std::ostream &out, uint32_t value)
            {
                const unsigned char bytes[4] = {
                    static_cast<unsigned char>(value & 0xFF), static_cast<unsigned char>((value >> 8) & 0xFF),
                    static_cast<unsigned char>((value >> 16) & 0xFF), static_cast<unsigned char>((value >> 24) & 0xFF)};
                out.write(reinterpret_cast<const char *>(bytes), 4);
            }

            void writeU64(std::ostream &out, uint64_t value)
            {
                writeU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFu));
                writeU32(out, static_cast<uint32_t>(value >> 32));
            }

            uint32_t readU32(std::istream &in)
            {
                unsigned char bytes[4];
                in.read(reinterpret_cast<char *>(bytes), 4);
                if (!in)
                    throw std::runtime_error("Dataset file: unexpected end of data");
                return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                       (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
            }

            uint64_t readU64(std::istream &in)
            {
                const uint64_t low = readU32(in);
                const uint64_t high = readU32(in);
                return low | (high << 32);
            }
        }

        // ====================================================================
        // Targets
        // ====================================================================

        Targets makeTargets(const LabeledPosition &position, int cols, PolicyTarget mode, double soft_temperature)
        {
            Targets targets;
            targets.policy.fill(0.0f);
            targets.value = static_cast<float>(position.value);

            int best = -1000;
            for (int c = 0; c < cols; ++c)
            {
                if (position.scores[c] != kIllegalScore)
                    best = std::max(best, static_cast<int>(position.scores[c]));
            }

            uint32_t best_mask = 0;
            uint32_t outcome_mask = 0;
            for (int c = 0; c < cols; ++c)
            {
                if (position.scores[c] == kIllegalScore)
                    continue;
                const int score = position.scores[c];
                if (score == best)
                    best_mask |= 1u << c;
                if (sign(score) == sign(best))
                    outcome_mask |= 1u << c;
            }

            // In a lost position every move "keeps" the result: only the slowest defeats are called optimal
            targets.optimal_mask = best >= 0 ? outcome_mask : best_mask;

            auto uniformOver = [&](uint32_t mask)
            {
                const int n = popcount(mask);
                for (int c = 0; c < cols; ++c)
                {
                    if (mask & (1u << c))
                        targets.policy[c] = 1.0f / static_cast<float>(n);
                }
            };

            switch (mode)
            {
            case PolicyTarget::BEST:
                uniformOver(best_mask);
                break;

            case PolicyTarget::OUTCOME:
                uniformOver(targets.optimal_mask);
                break;

            case PolicyTarget::SOFT:
            {
                double total = 0;
                for (int c = 0; c < cols; ++c)
                {
                    if (position.scores[c] == kIllegalScore)
                        continue;
                    const double weight = std::exp((position.scores[c] - best) / soft_temperature);
                    targets.policy[c] = static_cast<float>(weight);
                    total += weight;
                }
                for (int c = 0; c < cols; ++c)
                    targets.policy[c] = static_cast<float>(targets.policy[c] / total);
                break;
            }
            }

            return targets;
        }

        void encodeInto(const Position &position, Encoding encoding, size_t row, nn::Matrix &inputs, nn::Matrix &masks)
        {
            encodePosition(position, encoding, inputs.row(row));
            for (int c = 0; c < position.cols(); ++c)
                masks(row, static_cast<size_t>(c)) = position.canPlay(c) ? 1.0f : 0.0f;
        }

        // ====================================================================
        // Dataset
        // ====================================================================

        void Dataset::append(const Dataset &other)
        {
            if (other.rows_ != rows_ || other.cols_ != cols_ || other.connect_ != connect_)
                throw std::invalid_argument("Cannot merge datasets of different boards");
            items_.insert(items_.end(), other.items_.begin(), other.items_.end());
        }

        void Dataset::save(const std::string &path) const
        {
            std::ofstream out(path, std::ios::binary);
            if (!out)
                throw std::runtime_error("Cannot open '" + path + "' for writing");

            out.write(kMagic, 4);
            writeU32(out, kVersion);
            writeU32(out, static_cast<uint32_t>(rows_));
            writeU32(out, static_cast<uint32_t>(cols_));
            writeU32(out, static_cast<uint32_t>(connect_));
            writeU64(out, items_.size());

            // One record per position: the board as bytes (row 0 = top; 0 empty, 1 stone of the
            // player to move, 2 stone of the opponent), one signed byte of score per column
            // (-128 = column full), the value, and a reserved zero byte.
            std::vector<unsigned char> record(static_cast<size_t>(rows_ * cols_ + cols_ + 2));
            for (const LabeledPosition &item : items_)
            {
                const Position position = Position::fromBitboards(rows_, cols_, connect_, item.current, item.mask);
                size_t index = 0;
                for (int r = 0; r < rows_; ++r)
                    for (int c = 0; c < cols_; ++c)
                        record[index++] = static_cast<unsigned char>(position.cell(r, c));
                for (int c = 0; c < cols_; ++c)
                    record[index++] = static_cast<unsigned char>(item.scores[c]);
                record[index++] = static_cast<unsigned char>(item.value);
                record[index++] = 0;
                out.write(reinterpret_cast<const char *>(record.data()), static_cast<std::streamsize>(record.size()));
            }

            if (!out)
                throw std::runtime_error("Failed to write the dataset");
        }

        Dataset Dataset::load(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw std::runtime_error("Cannot open dataset '" + path + "'");

            char magic[4];
            in.read(magic, 4);
            if (!in || std::memcmp(magic, kMagic, 4) != 0)
                throw std::runtime_error("'" + path + "' is not a ConnectX dataset");
            if (readU32(in) != kVersion)
                throw std::runtime_error("Unsupported dataset version");

            const int rows = static_cast<int>(readU32(in));
            const int cols = static_cast<int>(readU32(in));
            const int connect = static_cast<int>(readU32(in));
            const uint64_t count = readU64(in);

            std::string reason;
            if (!Geometry::isSupported(rows, cols, connect, &reason))
                throw std::runtime_error("Dataset board not supported: " + reason);

            Dataset dataset(rows, cols, connect);
            dataset.items_.reserve(static_cast<size_t>(count));

            const Geometry &geometry = Geometry::get(rows, cols, connect);
            std::vector<unsigned char> record(static_cast<size_t>(rows * cols + cols + 2));

            for (uint64_t i = 0; i < count; ++i)
            {
                in.read(reinterpret_cast<char *>(record.data()), static_cast<std::streamsize>(record.size()));
                if (!in)
                    throw std::runtime_error("Dataset file truncated");

                LabeledPosition item;
                size_t index = 0;
                for (int r = 0; r < rows; ++r)
                {
                    for (int c = 0; c < cols; ++c)
                    {
                        const unsigned char cell = record[index++];
                        const uint64_t bit = 1ULL << (c * geometry.stride + (rows - 1 - r));
                        if (cell == 1)
                            item.current |= bit;
                        if (cell != 0)
                            item.mask |= bit;
                    }
                }
                for (int c = 0; c < cols; ++c)
                    item.scores[c] = static_cast<int8_t>(record[index++]);
                item.value = static_cast<int8_t>(record[index++]);
                dataset.items_.push_back(item);
            }

            return dataset;
        }

        // ====================================================================
        // Generation
        // ====================================================================

        Dataset generateDataset(const GenerateOptions &options,
                                const std::function<void(size_t, size_t, size_t)> &progress)
        {
            std::string reason;
            if (!Geometry::isSupported(options.rows, options.cols, options.connect, &reason))
                throw std::invalid_argument("Unsupported board: " + reason);

            Dataset dataset(options.rows, options.cols, options.connect);
            dataset.reserve(options.count);

            std::mutex mutex;
            std::unordered_set<uint64_t> seen;
            std::atomic<size_t> collected(0);
            std::atomic<size_t> skipped(0);

            auto worker = [&](int thread_index)
            {
                Rng rng(options.seed * 0x9E3779B97F4A7C15ULL + static_cast<uint64_t>(thread_index) * 7919 + 3);
                Solver solver(size_t(1) << 22);
                solver.setLimits(0, options.solve_time_ms);

                AlphaBetaAgent::Options player_options;
                player_options.depth = std::max(1, options.player_depth);
                player_options.use_table = 0;
                AlphaBetaAgent player(player_options, rng());

                while (collected.load() < options.count)
                {
                    Position position(options.rows, options.cols, options.connect);

                    while (!position.isTerminal() && collected.load() < options.count)
                    {
                        // Label this position (some of the time)
                        if (position.moves() >= options.min_stones && rng.uniform() < options.sample_probability)
                        {
                            bool is_new;
                            {
                                std::lock_guard<std::mutex> lock(mutex);
                                is_new = seen.insert(position.key()).second;
                            }

                            if (is_new)
                            {
                                std::array<int, kMaxCols> scores;
                                if (solver.scoreMoves(position, scores, options.weak))
                                {
                                    LabeledPosition item;
                                    item.current = position.current();
                                    item.mask = position.mask();

                                    int best = -1000;
                                    for (int c = 0; c < options.cols; ++c)
                                    {
                                        if (scores[c] == Solver::kIllegal)
                                            continue;
                                        item.scores[c] = static_cast<int8_t>(scores[c]);
                                        best = std::max(best, scores[c]);
                                    }
                                    item.value = static_cast<int8_t>(sign(best));

                                    std::lock_guard<std::mutex> lock(mutex);
                                    if (dataset.size() < options.count)
                                    {
                                        dataset.add(item);
                                        collected.store(dataset.size());
                                        if (progress && (dataset.size() % 100 == 0 || dataset.size() == options.count))
                                            progress(dataset.size(), options.count, skipped.load());
                                    }
                                }
                                else
                                {
                                    ++skipped;
                                }
                            }
                        }

                        // Play on: a noisy alpha-beta player (or pure random play)
                        int column;
                        if (options.player_depth <= 0 || rng.uniform() < options.epsilon)
                            column = columnOfCell(position, randomCell(position.legalMoveMask(), rng));
                        else
                            column = player.chooseMove(position).column;
                        position.play(column);
                    }
                }
            };

            std::vector<std::thread> pool;
            for (int i = 0; i < std::max(1, options.threads); ++i)
                pool.emplace_back(worker, i);
            for (auto &thread : pool)
                thread.join();

            return dataset;
        }
    }
}
