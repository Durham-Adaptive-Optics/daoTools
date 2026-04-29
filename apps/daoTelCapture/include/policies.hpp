/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:44:17
 * @ Description: Telemetry capture tool configuration parsing.
 */

#include <vector>
#include <optional>
#include <exception>
#include <yaml-cpp/yaml.h>

namespace Dao
{
    namespace Telemetry
    {
        using CoreID = int;
        using URI = std::string;
        template <typename T> using Required = T;
        template <typename T> using Optional = std::optional<T>;

        // @todo move all export related things to export hpp file..
        enum class ExportFormat
        {
            FITS,
            NUMPY
        };

        static inline std::unordered_map<ExportFormat, std::string> const fmtToRepr
        {
            { ExportFormat::FITS, "fits" },
            { ExportFormat::NUMPY, "numpy" }
        };

        static inline std::unordered_map<std::string, ExportFormat> const ReprToFmt
        {
            { "fits", ExportFormat::FITS },
            { "numpy", ExportFormat::NUMPY }
        };

        /* Telemetry source types from URI.
        */
        enum class UriClass
        {
            FILE,
            SMEM
        };

        /* General session policies.
        */
        struct GeneralPolicies
        {
            Required<std::string> rootStorage;
            Optional<bool> groupingEnabled { true };
            Optional<std::string> groupName;
        };

        /* File source policies.
        */
        struct FilePolicy
        {
            Required<std::string> absPath;
            Optional<std::string> saveAsName;
        };

        /* Shared-memory source policies.
        */
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
            /* Parse and store capture session policies from yaml document.
             * Throws an exception if the parse fails for any reason.
            */
            CapturePolicies(std::string const& ymlDocumentString);

            /* Utility method for printing capture policies to stdout
             * for viewing.
            */
            void dump() const noexcept;

            GeneralPolicies generalPolicies;
            std::vector<FilePolicy> filePolicies;
            std::vector<SharedMemoryPolicy> smemPolicies;

            private:
            /* Parse a URI and return its resource class. If the URI is
             * malformed then an exception is thrown.
            */
            UriClass classFromURI(URI const& uri) const;

            /* Parse a URI and return its resource location. If the URI is
             * malformed then an exception is thrown.
            */
            std::string locationFromURI(URI const& uri) const;

            /* Parses the yaml document and populates
             * the capture-policy object. It will throw an exception if
             * there was an issue during the parse for any reason.
            */
            void load(YAML::Node const& ymlDoc);

            /* Loads file policy parameters from a yaml source node.
            */
            void loadFilePolicy(YAML::Node const& sourceNode, FilePolicy& policySet);

            /* Loads shared-memory policy parameters from a yaml source node.
            */
            void loadSmemPolicy(YAML::Node const& sourceNode, SharedMemoryPolicy& policySet);

            /* Template specializations for unwrapping std::optional types.
            */
            template <typename Y> struct UnwrapOptional { using type = Y; };
            template <typename Y> struct UnwrapOptional<std::optional<Y>> { using type = Y; };

            /* Method to load a required parameter from the provided yaml node; if the load fails then
             * an exception is thrown.
            */
            template <typename T>
            void loadRequired(T& store, std::string const& name, std::optional<YAML::Node> const docRoot) const
            {
                auto const& parameter = docRoot[name];

                if (!parameter) {
                    throw std::runtime_error(name);
                }

                if (parameter.Type() != YAML::NodeType::Scalar) {
                    throw std::runtime_error(name);
                }

                try {
                    store = parameter.as<typename UnwrapOptional<T>::type>();
                } catch (YAML::BadConversion const& e) {
                    throw std::runtime_error(name);
                }
            }

            /* Method to load an optional parameter from the provided yaml node, if present; if the load fails then
             * an exception is thrown.
            */
            template <typename T>
            void loadOptional(T& store, std::string const& name, std::optional<YAML::Node> const docRoot) const
            {
                if (auto const& parameter = docRoot[name]; parameter) {
                    if (parameter.Type() != YAML::NodeType::Scalar) {
                        throw std::runtime_error(name);
                    }

                    try {
                        store = parameter.as<typename UnwrapOptional<T>::type>();
                    } catch (YAML::BadConversion const& e) {
                        throw std::runtime_error(name);
                    }
                }
            }
        };

    };
};

