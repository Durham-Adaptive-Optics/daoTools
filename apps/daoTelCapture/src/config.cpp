/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2026-04-28 09:43:22
 * @ Description:
 */

#include <config.hpp>
#include <iostream>

CapturePolicies::CapturePolicies(YAML::Node const& ymlDocument)
    : ymlDoc_(ymlDocument)
{
    load();
    dump();
}

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
    auto const splitPos = uri.find("://");
    if (splitPos == std::string::npos) {
        throw std::runtime_error("Malformed URI");
    }

    return uri.substr(splitPos, std::string::npos);
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

void CapturePolicies::load()
{
    // load session policies..
    if (auto const& sessionNode = ymlDoc_["session_policies"]; sessionNode) {
        loadRequired(rootStorage_, "root_storage", sessionNode);
        loadOptional(groupingEnabled_, "group_outputs", sessionNode);
        loadOptional(groupName_, "group_name", sessionNode);
        loadOptional(overwriteExisting_, "overwrite_existing", sessionNode);
    }
    else {
        throw MissingRequiredParameter("session_policies");
    }

    // load source policies..
    if (auto const& sourcesNode = ymlDoc_["source_list"]; sourcesNode) {
        if (sourcesNode.Type() != YAML::NodeType::Sequence) {
            throw InvalidParameterType("source_list");
        }

        if (!sourcesNode.size()) {
            std::cout << "no sources specified!" << std::endl;
        }

        for (auto const& sourceNode : sourcesNode) {
            Required<URI> uri;
            loadRequired(uri, "uri", sourceNode);

            switch (classFromURI(uri)) {
                case UriClass::FILE: {
                    FilePolicy& policySet = filePolicies_.emplace_back();
                    policySet.absPath = locationFromURI(uri);
                    loadFilePolicy(sourceNode, policySet);
                } break;

                case UriClass::SMEM: {
                    SharedMemoryPolicy& policySet = smemPolicies_.emplace_back();
                    policySet.absPath = locationFromURI(uri);
                    loadSmemPolicy(sourceNode, policySet);
                } break;
            }
        }
    }
    else {
        throw MissingRequiredParameter("source_list");
    }
}

void CapturePolicies::dump() const noexcept
{
    std::cout << "\n=== Capture Policy Dump ===\n\n";

    // Session policies
    std::cout << "Session Policies:\n";
    std::cout << "  Root Storage:       " << rootStorage_ << "\n";
    std::cout << "  Grouping Enabled:   " << (groupingEnabled_ ? "true" : "false") << "\n";
    std::cout << "  Group Name:         " << (groupName_ ? groupName_.value() : "Timestamp") << "\n";
    std::cout << "  Overwrite Existing: " << (overwriteExisting_ ? "true" : "false") << "\n";

    // File policies
    std::cout << "\nFile Policies:\n";
    for (FilePolicy const& pol : filePolicies_) {
        std::cout << "  - Absolute Path: " << pol.absPath << "\n";
        std::cout << "    Save As:     " << (pol.saveAsName ? pol.saveAsName.value() : "Original") << "\n";
    }

    // Shared memory policies
    std::cout << "\nShared Memory Policies:\n";
    for (SharedMemoryPolicy const& pol : smemPolicies_) {
        std::cout << "  - Absolute Path:        " << pol.absPath << "\n";
        std::cout << "    Save As:     " << (pol.saveAsName ? pol.saveAsName.value() : "Original") << "\n";
        std::cout << "    Metadata Only:        " << (pol.metadataOnly ? "true" : "false") << "\n";
        std::cout << "    Samples:              " << (pol.nSamples ? std::to_string(pol.nSamples.value()) : "Unbounded") << "\n";
        std::cout << "    Export Format:        " << formatNames_.at(pol.format) << "\n";
        std::cout << "    Chunk Size:           " << (pol.chunkSize ? std::to_string(pol.chunkSize.value()) : "Unbounded single-file") << "\n";
        std::cout << "    Export Affinity:       " << (pol.exportThreadAffinity ? std::to_string(pol.exportThreadAffinity.value()) : "Any core") << "\n";
        std::cout << "    Poll Affinity:       " << (pol.pollThreadAffinity ? std::to_string(pol.pollThreadAffinity.value()) : "Any core") << "\n";
        std::cout << "    Buffer Limit:       " << (pol.bufferLimit ? std::to_string(pol.bufferLimit.value()) : "Unbounded") << "\n";
    }

    std::cout << "\n=== End Capture Policy Dump ===\n\n";
}
