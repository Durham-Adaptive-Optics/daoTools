/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:22
 * @ Description: Telemetry capture tool configuration parsing.
 */

#include <policies.hpp>
#include <iostream>

/* Implements conversions between YAML node and custom types.
*/
namespace YAML
{
    template<>
    struct convert<Dao::Telemetry::ExportFormat>
    {
        static Node encode(Dao::Telemetry::ExportFormat const& rhs)
        {
            return YAML::Node { Dao::Telemetry::fmtToRepr.at(rhs) };
        }

        static bool decode(Node const& node, Dao::Telemetry::ExportFormat& rhs)
        {
            bool decoded { false };

            try {
                std::string const formatString = node.as<std::string>();
                auto const& kv = Dao::Telemetry::ReprToFmt.find(formatString);
                if (kv != Dao::Telemetry::ReprToFmt.end()) {
                    rhs = kv->second;
                    decoded = true;
                }
            } catch (...) {}

            return decoded;
        }
    };
};

namespace Dao::Telemetry
{
    UriClass CapturePolicies::classFromURI(URI const& uri) const
    {
        auto const splitPos = uri.find("://");
        if (splitPos == std::string::npos) {
            throw std::runtime_error("Malformed URI");
        }

        std::string const uriClassName = uri.substr(0, splitPos);
        if ("file" == uriClassName) {
            return UriClass::FILE;
        }
        else if ("smem" == uriClassName) {
            return UriClass::SMEM;
        }
        else {
            throw std::runtime_error("Malformed URI");
        }
    }

    std::string CapturePolicies::locationFromURI(URI const& uri) const
    {
        std::string const uriDelimiter { "://" };
        auto const splitPos = uri.find(uriDelimiter);
        if (splitPos == std::string::npos) {
            throw std::runtime_error("Malformed URI");
        }

        return uri.substr(splitPos + uriDelimiter.length(), std::string::npos);
    }

    CapturePolicies::CapturePolicies(std::string const& ymlDocumentString)
    {
        auto const ymlDoc = YAML::Load(ymlDocumentString);
        load(ymlDoc);
    }

    void CapturePolicies::loadFilePolicy(YAML::Node const& sourceNode, FilePolicy& policySet)
    {
        loadOptional(policySet.saveAsName, "save_as", sourceNode);
    }

    void CapturePolicies::loadSmemPolicy(YAML::Node const& sourceNode, SharedMemoryPolicy& policySet)
    {
        loadOptional(policySet.saveAsName, "save_as", sourceNode);
        loadOptional(policySet.metadataOnly, "metadata_only", sourceNode);
        loadOptional(policySet.nSamples, "samples", sourceNode);
        loadRequired(policySet.format, "format", sourceNode);

        loadOptional(policySet.chunkSize, "chunk_size", sourceNode);
        loadOptional(policySet.exportThreadAffinity, "export_affinity", sourceNode);
        loadOptional(policySet.pollThreadAffinity, "poll_affinity", sourceNode);
        loadOptional(policySet.bufferLimit, "buffer_limit", sourceNode);
    }

    void CapturePolicies::load(YAML::Node const& ymlDoc)
    {
        // load session policies..
        if (auto const& sessionNode = ymlDoc["session_policies"]; sessionNode) {
            loadRequired(generalPolicies.rootStorage, "root_storage", sessionNode);
            loadOptional(generalPolicies.groupingEnabled, "group_outputs", sessionNode);
            loadOptional(generalPolicies.groupName, "group_name", sessionNode);
        }
        else {
            throw std::runtime_error("session_policies");
        }

        // load source policies..
        if (auto const& sourcesNode = ymlDoc["source_list"]; sourcesNode) {
            if (sourcesNode.Type() != YAML::NodeType::Sequence) {
                throw std::runtime_error("source_list");
            }

            if (!sourcesNode.size()) {
                std::cout << "no sources specified!" << std::endl;
            }

            for (auto const& sourceNode : sourcesNode) {
                Required<URI> uri;
                loadRequired(uri, "uri", sourceNode);

                switch (classFromURI(uri)) {
                    case UriClass::FILE: {
                        FilePolicy& policySet = filePolicies.emplace_back();
                        policySet.absPath = locationFromURI(uri);
                        loadFilePolicy(sourceNode, policySet);
                    } break;

                    case UriClass::SMEM: {
                        SharedMemoryPolicy& policySet = smemPolicies.emplace_back();
                        policySet.absPath = locationFromURI(uri);
                        loadSmemPolicy(sourceNode, policySet);
                    } break;
                }
            }
        }
        else {
            throw std::runtime_error("source_list");
        }
    }

    void CapturePolicies::dump() const noexcept
    {
        std::cout << "\n=== Capture Policy Dump ===\n\n";

        // Session policies
        std::cout << "Session Policies:\n";
        std::cout << "  Root Storage:       " << generalPolicies.rootStorage << "\n";
        std::cout << "  Grouping Enabled:   " << (generalPolicies.groupingEnabled ? "true" : "false") << "\n";
        std::cout << "  Group Name:         " << (generalPolicies.groupName ? generalPolicies.groupName.value() : "Timestamp") << "\n";
        std::cout << "  Overwrite Existing: " << (generalPolicies.overwriteExisting ? "true" : "false") << "\n";

        // File policies
        std::cout << "\nFile Policies:\n";
        for (FilePolicy const& pol : filePolicies) {
            std::cout << "  - Absolute Path: " << pol.absPath << "\n";
            std::cout << "    Save As:     " << (pol.saveAsName ? pol.saveAsName.value() : "Original") << "\n";
        }

        // Shared memory policies
        std::cout << "\nShared Memory Policies:\n";
        for (SharedMemoryPolicy const& pol : smemPolicies) {
            std::cout << "  - Absolute Path:        " << pol.absPath << "\n";
            std::cout << "    Save As:     " << (pol.saveAsName ? pol.saveAsName.value() : "Original") << "\n";
            std::cout << "    Metadata Only:        " << (pol.metadataOnly ? "true" : "false") << "\n";
            std::cout << "    Samples:              " << (pol.nSamples ? std::to_string(pol.nSamples.value()) : "Unbounded") << "\n";
            std::cout << "    Export Format:        " << fmtToRepr.at(pol.format) << "\n";
            std::cout << "    Chunk Size:           " << (pol.chunkSize ? std::to_string(pol.chunkSize.value()) : "Unbounded single-file") << "\n";
            std::cout << "    Export Affinity:       " << (pol.exportThreadAffinity ? std::to_string(pol.exportThreadAffinity.value()) : "Any core") << "\n";
            std::cout << "    Poll Affinity:       " << (pol.pollThreadAffinity ? std::to_string(pol.pollThreadAffinity.value()) : "Any core") << "\n";
            std::cout << "    Buffer Limit:       " << (pol.bufferLimit ? std::to_string(pol.bufferLimit.value()) : "Unbounded") << "\n";
        }

        std::cout << "\n=== End Capture Policy Dump ===\n\n";
    }
};
