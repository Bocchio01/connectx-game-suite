#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include "ai/model_cache.hpp"

namespace fs = std::filesystem;

namespace ai
{
    namespace
    {
        struct CacheEntry
        {
            std::weak_ptr<const nn::PolicyValueNet> network;
            std::uintmax_t size = 0;
            fs::file_time_type modified;
        };

        /** Value of an environment variable (empty if it is not set). */
        std::string environmentVariable(const char *name)
        {
#ifdef _MSC_VER
            // getenv() is flagged as unsafe by MSVC
            char *value = nullptr;
            size_t length = 0;
            if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
                return std::string();
            std::string result(value);
            free(value);
            return result;
#else
            const char *value = std::getenv(name);
            return value ? std::string(value) : std::string();
#endif
        }

        /** Directory containing the running executable (empty if unknown). */
        fs::path executableDirectory()
        {
#ifdef _WIN32
            wchar_t buffer[MAX_PATH];
            const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
                return fs::path(buffer).parent_path();
#elif defined(__APPLE__)
            char buffer[PATH_MAX];
            uint32_t size = sizeof(buffer);
            if (_NSGetExecutablePath(buffer, &size) == 0)
                return fs::path(buffer).parent_path();
#else
            char buffer[4096];
            const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
            if (length > 0)
            {
                buffer[length] = '\0';
                return fs::path(buffer).parent_path();
            }
#endif
            return fs::path();
        }
    }

    std::string resolveModelPath(const std::string &path)
    {
        std::error_code error;
        if (fs::is_regular_file(path, error))
            return path;

        std::vector<fs::path> folders;
        const fs::path exe_dir = executableDirectory();
        if (!exe_dir.empty())
        {
            folders.push_back(exe_dir / "models");
            folders.push_back(exe_dir.parent_path() / "models"); // multi-config builds: bin/Release/connectx-ai.exe, bin/models
            folders.push_back(exe_dir);
        }
        folders.push_back(fs::path("models"));

        const std::string env = environmentVariable("CONNECTX_MODEL_PATH");
        if (!env.empty())
        {
#ifdef _WIN32
            const char separator = ';';
#else
            const char separator = ':';
#endif
            std::string list(env);
            size_t begin = 0;
            while (begin <= list.size())
            {
                const size_t end = list.find(separator, begin);
                const std::string item = list.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                if (!item.empty())
                    folders.push_back(fs::path(item));
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
        }

        std::string searched = "  " + path + "\n";
        if (!fs::path(path).is_absolute())
        {
            for (const fs::path &folder : folders)
            {
                for (const std::string &candidate : {path, path + ".cxnn"})
                {
                    const fs::path full = folder / candidate;
                    if (fs::is_regular_file(full, error))
                        return full.string();
                    searched += "  " + full.string() + "\n";
                }
            }
        }
        else if (fs::is_regular_file(path + ".cxnn", error))
        {
            return path + ".cxnn";
        }

        throw std::runtime_error("Model file '" + path + "' not found. Looked in:\n" + searched);
    }

    std::shared_ptr<const nn::PolicyValueNet> loadSharedModel(const std::string &path)
    {
        static std::mutex mutex;
        static std::map<std::string, CacheEntry> cache;

        const std::string resolved = resolveModelPath(path);

        std::error_code error;
        const auto size = fs::file_size(resolved, error);
        const auto modified = fs::last_write_time(resolved, error);
        const bool have_stats = !error;

        std::lock_guard<std::mutex> lock(mutex);

        auto it = cache.find(resolved);
        if (it != cache.end() && have_stats && it->second.size == size && it->second.modified == modified)
        {
            if (auto existing = it->second.network.lock())
                return existing;
        }

        auto network = std::make_shared<const nn::PolicyValueNet>(nn::PolicyValueNet::load(resolved));

        CacheEntry entry;
        entry.network = network;
        entry.size = have_stats ? size : 0;
        entry.modified = modified;
        cache[resolved] = entry;
        return network;
    }
}
