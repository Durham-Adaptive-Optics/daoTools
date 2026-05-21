/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:22
 * @ Description: Implementation of DAQ session configuration.
 */

#include <configuration.hpp>
#include <fmt/format.h>
#include <daoTools.h>

 /* ---------------------------------------------------------------- */

namespace Dao::DAQ
{
    URIClass DAQConfiguration::classFromURI(URI const& uri) const {
        auto const splitPos = uri.find("://");
        if (splitPos == std::string::npos) {
            std::string const err = fmt::format("Malformed URI ({})", uri);
            throw std::runtime_error(err);
        }

        std::string const uriClassName = uri.substr(0, splitPos);
        if ("file" == uriClassName) {
            return URIClass::FILE;
        }
        else if ("smem" == uriClassName) {
            return URIClass::SMEM;
        }
        else {
            std::string const err = fmt::format("Invalid URI class ({})", uriClassName);
            throw std::runtime_error(err);
        }
    }

    std::string DAQConfiguration::locationFromURI(URI const& uri) const {
        std::string const uriDelimiter { "://" };
        auto const splitPos = uri.find(uriDelimiter);
        if (splitPos == std::string::npos) {
            std::string const err = fmt::format("Malformed URI ({})", uri);
            throw std::runtime_error(err);
        }

        return uri.substr(splitPos + uriDelimiter.length(), std::string::npos);
    }

    /* ---------------------------------------------------------------- */

    DAQConfiguration::DAQConfiguration(std::string const& daqRaqConfig, Dao::Log::Logger& log) :
        log_(log) {
        //
        load(
            YAML::Load(daqRaqConfig)
        );

        log_.Debug("parsed configuration");
    }

    DAQConfiguration::~DAQConfiguration() {
        log_.Debug("destroyed configuration");
    }

    /* ---------------------------------------------------------------- */

    void DAQConfiguration::loadFileParams([[maybe_unused]] YAML::Node const& sourceNode, [[maybe_unused]] FileParameters& params) {
        return;
    }

    void DAQConfiguration::loadSmemParams(YAML::Node const& sourceNode, SmemParameters& params) {
        loadOptional(params.metadataOnly, "metadata_only", sourceNode);
        loadOptional(params.nSamples, "samples", sourceNode);
        loadOptional(params.format, "format", sourceNode);
        loadOptional(params.fileRollover, "file_rollover", sourceNode);
        loadOptional(params.daqThreadAffinity, "daq_affinity", sourceNode);
        loadOptional(params.sinkThreadAffinity, "sink_affinity", sourceNode);
        loadOptional(params.bufferLimit, "buffer_limit", sourceNode);
        loadOptional(params.eagerStart, "eager_start", sourceNode);

        int buffLen {};
        if (DAO_SUCCESS != daoToolsLocalName(params.absPath.c_str(), nullptr, &buffLen)) {
            auto const err = fmt::format("failed to get smem local-name length {}", params.absPath);
            throw std::runtime_error(err);
        }

        char buff[buffLen];
        if (DAO_SUCCESS != daoToolsLocalName(params.absPath.c_str(), buff, nullptr)) {
            auto const err = fmt::format("failed to get smem local-name {}", params.absPath);
            throw std::runtime_error(err);
        }
        params.localName = buff;
    }

    /* ---------------------------------------------------------------- */

    void DAQConfiguration::load(YAML::Node const& ymlDoc) {
        // load session policies..
        loadRequired(sessionParams_.rootStorage, "root_storage", ymlDoc);

        // load source policies..
        if (auto const& sourcesNode = ymlDoc["sources"]; sourcesNode) {
            if (sourcesNode.Type() != YAML::NodeType::Sequence) {
                std::string const err = fmt::format("'sources' must be a list");
                throw std::runtime_error(err);
            }

            for (auto const& sourceNode : sourcesNode) {
                Required<URI> uri;
                loadRequired(uri, "uri", sourceNode);

                auto const& uriLocation = locationFromURI(uri);
                if (auto [_, inserted] = sourceLookup_.insert(uriLocation); !inserted) {
                    std::string const err = fmt::format("duplicate source {}", uri);
                    throw std::runtime_error(err);
                }

                switch (classFromURI(uri)) {
                    case URIClass::FILE: {
                        FileParameters& policySet = fileSrcs_.emplace_back();
                        policySet.absPath = uriLocation;
                        loadFileParams(sourceNode, policySet);
                    } break;

                    case URIClass::SMEM: {
                        SmemParameters& policySet = smemSrcs_.emplace_back();
                        policySet.absPath = uriLocation;
                        loadSmemParams(sourceNode, policySet);
                    } break;
                }
            }
        }
        else {
            throw std::runtime_error("missing 'sources' list");
        }

        //
        if (!numResources()) {
            throw std::runtime_error("empty 'sources' list");
        }
    }
};
