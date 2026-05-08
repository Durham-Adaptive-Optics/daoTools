/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:22
 * @ Description: Implementation of DAQ session configuration.
 */

#include <configuration.hpp>
#include <fmt/format.h>

 /* Implements conversions between YAML node and custom types.
 */
namespace YAML
{
    template<>
    struct convert<Dao::DAQ::ExportFormat> {
        static Node encode(Dao::DAQ::ExportFormat const& rhs) {
            return YAML::Node { Dao::DAQ::fmtToRepr.at(rhs) };
        }

        static bool decode(Node const& node, Dao::DAQ::ExportFormat& rhs) {
            bool decoded { true };

            try {
                std::string const formatString = node.as<std::string>();
                auto const& kv = Dao::DAQ::ReprToFmt.find(formatString);
                if (kv != Dao::DAQ::ReprToFmt.end()) {
                    rhs = kv->second;
                }
            } catch (...) {
                decoded = false;
            }

            return decoded;
        }
    };
};

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

    DAQConfiguration::DAQConfiguration(std::string const& docString) {
        auto const doc = YAML::Load(docString);
        load(doc);
    }

    /* ---------------------------------------------------------------- */

    void DAQConfiguration::loadFileParams(YAML::Node const& sourceNode, FileParameters& policySet) {
        return;
    }

    void DAQConfiguration::loadSmemParams(YAML::Node const& sourceNode, SmemParameters& policySet) {
        loadOptional(policySet.metadataOnly, "metadata_only", sourceNode);
        loadOptional(policySet.nSamples, "samples", sourceNode);
        loadRequired(policySet.format, "format", sourceNode);
        loadOptional(policySet.fileRollover, "file_rollover", sourceNode);
        loadOptional(policySet.daqThreadAffinity, "daq_affinity", sourceNode);
        loadOptional(policySet.sinkThreadAffinity, "sink_affinity", sourceNode);
        loadOptional(policySet.bufferLimit, "buffer_limit", sourceNode);
        loadOptional(policySet.eagerStart, "eager_start", sourceNode);
    }

    /* ---------------------------------------------------------------- */

    void DAQConfiguration::load(YAML::Node const& ymlDoc) {
        // load session policies..
        if (auto const& sessionNode = ymlDoc["session_parameters"]; sessionNode) {
            loadRequired(sessionParams_.rootStorage, "root_storage", sessionNode);
        }
        else {
            std::string const err = fmt::format("DAQ configuration omitted 'session_parameters'");
            throw std::runtime_error(err);
        }

        // load source policies..
        if (auto const& sourcesNode = ymlDoc["source_list"]; sourcesNode) {
            if (sourcesNode.Type() != YAML::NodeType::Sequence) {
                std::string const err = fmt::format("DAQ configuration 'source_list' is not a list");
                throw std::runtime_error(err);
            }

            for (auto const& sourceNode : sourcesNode) {
                Required<URI> uri;
                loadRequired(uri, "uri", sourceNode);

                switch (classFromURI(uri)) {
                    case URIClass::FILE: {
                        FileParameters& policySet = fileSrcs_.emplace_back();
                        policySet.absPath = locationFromURI(uri);
                        loadFileParams(sourceNode, policySet);
                    } break;

                    case URIClass::SMEM: {
                        SmemParameters& policySet = smemSrcs_.emplace_back();
                        policySet.absPath = locationFromURI(uri);
                        loadSmemParams(sourceNode, policySet);
                    } break;
                }
            }
        }
        else {
            std::string const err = fmt::format("DAQ configuration omitted 'source_list'");
            throw std::runtime_error(err);
        }
    }
};
