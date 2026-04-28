/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:44:17
 * @ Description:
 */

#include <vector>
#include <optional>
#include <exception>
#include <yaml-cpp/yaml.h>

using CoreID = int;
using URI = std::string;
template <typename T> using Required = T;
template <typename T> using Optional = std::optional<T>;

struct MissingRequiredParameter : public std::exception
{
    MissingRequiredParameter(std::string const& paramName)
        : parameterName_(paramName)
    {
    }

    const char* what() const noexcept override
    {
        std::string const msg = "capture policies missing required parameter '" + parameterName_ + "'";
        return msg.c_str();
    }

    private:
    std::string const parameterName_;
};

struct InvalidParameterType : public std::exception
{
    InvalidParameterType(std::string const& paramName)
        : parameterName_(paramName)
    {
    }

    const char* what() const noexcept override
    {
        std::string const requiredDataType = "??";
        std::string const msg = "capture policies parameter '" + parameterName_ + "' must have type '" + requiredDataType;
        return msg.c_str();
    }

    private:
    std::string const parameterName_;
};

enum class ExportFormat
{
    FITS,
    NUMPY
};

enum class UriClass
{
    SMEM,
    FILE
};

struct FilePolicy
{
    Required<std::string> absPath;
    Optional<std::string> saveAsName;
};

struct SharedMemoryPolicy
{
    Required<std::string> absPath;
    Optional<std::string> saveAsName;
    Optional<bool> metadataOnly { false };
    Optional<size_t> nSamples;
    Required<ExportFormat> format;
    Optional<size_t> chunkSize;
    Optional<CoreID> exportThreadAffinity;
    Optional<CoreID> pollThreadAffinity;
    Optional<size_t> bufferLimit;
};

class CapturePolicies
{
    public:
    CapturePolicies(YAML::Node const& ymlDocument);

    private:
    //
    YAML::Node const& ymlDoc_;

    // capture session policies
    Required<std::string> rootStorage_;
    Optional<bool> groupingEnabled_ { true };
    Optional<std::string> groupName_;
    Optional<bool> overwriteExisting_ { false };

    // source policies
    std::vector<FilePolicy> filePolicies_;
    std::vector<SharedMemoryPolicy> smemPolicies_;

    // methods
    void load();
    void dump() const noexcept;
    void loadFilePolicy(YAML::Node const& sourceNode, FilePolicy& policySet);
    void loadSmemPolicy(YAML::Node const& sourceNode, SharedMemoryPolicy& policySet);
    UriClass classFromURI(URI const& uri) const;
    std::string locationFromURI(URI const& uri) const;

    inline static std::unordered_map<ExportFormat, std::string> formatNames_;
    {
        { ExportFormat::FITS, "fits" },
        { ExportFormat::NUMPY, "numpy" }
    };


    template <typename T> void loadRequired(T& store, std::string const& name, std::optional<YAML::Node const&> root = std::nullopt) const
    {
        YAML::Node node = root ? root.value() : ymlDoc_;
        auto const& parameter = ymlDoc_[name];

        if (!parameter) {
            throw MissingRequiredParameter(name);
        }

        if (parameter.Type() != YAML::NodeType::Scalar) {
            throw InvalidParameterType(name);
        }

        try {
            store = parameter.as<T>();
        } catch (YAML::BadConversion const& e) {
            throw InvalidParameterType(name);
        }
    }

    template <typename T> void loadOptional(T& store, std::string const& name, std::optional<YAML::Node const&> root = std::nullopt) const noexcept
    {
        YAML::Node node = root ? root.value() : ymlDoc_;
        if (auto const& parameter = ymlDoc_[name]; parameter) {
            if (parameter.Type() != YAML::NodeType::Scalar) {
                throw InvalidParameterType(name);
            }

            try {
                store = parameter.as<T>();
            } catch (YAML::BadConversion const& e) {
                throw InvalidParameterType(name);
            }
        }
    }
};
