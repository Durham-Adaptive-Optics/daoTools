/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:44:17
 * @ Description: DAQ Configuration Header
 */

#pragma once

 /* ---------------------------------------------------------------- */

#include <yaml-cpp/yaml.h>
#include <export.hpp>
#include <exception>
#include <optional>
#include <vector>

/* ---------------------------------------------------------------- */

namespace Dao::DAQ
{
    using CoreID = int;
    using URI = std::string;
    template <typename T> using Required = T;
    template <typename T> using Optional = std::optional<T>;

    /* The various supported data sources that a URI can
     * point to.
    */
    enum class URIClass {
        FILE, SMEM
    };

    /* Structures for defining the various parameters for a session
     * and the available data sources.
    */
    struct SessionParameters {
        Required<std::string> rootStorage;
    };

    struct FileParameters {
        Required<std::string> absPath;
    };

    struct SmemParameters {
        Required<std::string> absPath;
        Optional<bool> metadataOnly { false };
        Optional<size_t> nSamples;
        Required<ExportFormat> format;
        Optional<size_t> fileRollover;
        Optional<CoreID> sinkThreadAffinity;
        Optional<CoreID> daqThreadAffinity;
        Optional<size_t> bufferLimit;
        Optional<bool> eagerStart { true };
    };

    /* ---------------------------------------------------------------- */

    /* Class responsible for parsing a raw YAML string for
     * the DAQ session parameter configuration and storing them
     * for later reference.
    */
    class DAQConfiguration {
        public:
        /* Parse and store capture session policies from yaml document.
         * Throws an exception if the parse fails for any reason.
        */
        DAQConfiguration(std::string const& docString);

        auto const& sessionParameters() const { return sessionParams_; }
        auto const& fileResources() const { return fileSrcs_; }
        auto const& smemResources() const { return smemSrcs_; }
        size_t numResources() const { return fileSrcs_.size() + smemSrcs_.size(); }

        private:
        /* Variables storing the parsed DAQ session configuration.
        */
        SessionParameters sessionParams_;
        std::vector<FileParameters> fileSrcs_;
        std::vector<SmemParameters> smemSrcs_;

        /* Parse a URI and return its resource class. If the URI is
         * malformed then an exception is thrown.
        */
        URIClass classFromURI(URI const& uri) const;

        /* Parse a URI and return its resource location. If the URI is
         * malformed then an exception is thrown.
        */
        std::string locationFromURI(URI const& uri) const;

        /* Parses the yaml document and loads its configuration into
         * this object for later reference; an exception is thrown if
         * there was an issue.
        */
        void load(YAML::Node const& doc);

        /* Load file parameters from DAQ configuration source.
        */
        void loadFileParams(YAML::Node const& ctx, FileParameters& params);

        /* Load shared memory parameters from DAQ configuration source.
        */
        void loadSmemParams(YAML::Node const& ctx, SmemParameters& params);

        /* Template specializations for unwrapping std::optional types.
        */
        template <typename Y> struct UnwrapOptional { using type = Y; };
        template <typename Y> struct UnwrapOptional<std::optional<Y>> { using type = Y; };

        /* Method to load a required parameter from the provided yaml node; if the load fails then
         * an exception is thrown.
        */
        template <typename T>
        void loadRequired(T& store, std::string const& name, YAML::Node const ctx) const {
            auto const& parameter = ctx[name];

            if (!parameter) {
                std::string const err = fmt::format("DAQ configuration omitted required parameter '{}'", name);
                throw std::runtime_error(err);
            }

            if (parameter.Type() != YAML::NodeType::Scalar) {
                std::string const err = fmt::format("DAQ configuration required parameter '{}' is not a scalar", name);
                throw std::runtime_error(err);
            }

            try {
                store = parameter.as<typename UnwrapOptional<T>::type>();
            } catch (YAML::BadConversion const& e) {
                std::string const err = fmt::format("DAQ configuration required parameter '{}' has incorrect value-type", name);
                throw std::runtime_error(err);
            }
        }

        /* Method to load an optional parameter from the provided yaml node, if present; if the load fails then
         * an exception is thrown.
        */
        template <typename T>
        void loadOptional(T& store, std::string const& name, YAML::Node const ctx) const {
            if (auto const& parameter = ctx[name]; parameter) {
                if (parameter.Type() != YAML::NodeType::Scalar) {
                    std::string const err = fmt::format("DAQ configuration optional parameter '{}' is not a scalar", name);
                    throw std::runtime_error(err);
                }

                try {
                    store = parameter.as<typename UnwrapOptional<T>::type>();
                } catch (YAML::BadConversion const& e) {
                    std::string const err = fmt::format("DAQ configuration optional parameter '{}' has incorrect value-type", name);
                    throw std::runtime_error(err);
                }
            }
        }
    };
};

