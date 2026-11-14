#pragma once

#include <memory>
#include <string>

#include "nn/network.hpp"

namespace ai
{
    /**
     * Finds a model file. A path that exists is used as it is; otherwise, for relative paths, the
     * folder "models" next to the executable (or next to its parent folder), the folder of the
     * executable, "./models" and the directories listed in the CONNECTX_MODEL_PATH environment
     * variable are tried (also adding the ".cxnn" extension). So "connectx_6x7" is enough to
     * name the pretrained model that is installed with the programs.
     * @throws std::runtime_error listing the places searched if nothing is found
     */
    std::string resolveModelPath(const std::string &path);

    /**
     * Loads a model file (".cxnn") and shares the network between all the agents that
     * use the same file: a tournament creates one agent per thread, but the weights
     * are loaded once. PolicyValueNet::predict() is thread-safe, so this is fine.
     *
     * The cache notices when the file changes (size or modification time), which
     * matters while a training run keeps overwriting its checkpoint.
     *
     * @throws std::runtime_error if the file cannot be read
     */
    std::shared_ptr<const nn::PolicyValueNet> loadSharedModel(const std::string &path);
}
