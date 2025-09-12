/**
 * @ Author: Thomas N. Davies
 * @ Company: Centre for Advanced Instrumentation, Durham University
 * @ Contact: thomas.n.davies@durham.ac.uk
 * @ Create Time: 2025-09-11 14:20:40
 * @ Description: Code instrumentation.
 */

/* ===== USAGE ===== 
Below is an example program demonstrating the use of daoProfile to instrument
code. If DAO_PROFILE_ENABLED is not defined, all DAO_PROFILE_xx macros expand
to empy expressions so that no overhead is incurred.

#define DAO_PROFILE_ENABLED // <-- or pass on cli with: -DDAO_PROFILE_ENABLED
#include <daoProfile.hpp>

int main() {
    DAO_PROFILE(prof, std::chrono::nanoseconds, "block1", "block2")

    for(int i = 0; i < 10; ++i) {
        DAO_PROFILE_NEW_FRAME(prof)
    
        DAO_PROFILE_START(prof, "block1")
        for(int x = 0; x < 10000000; x++) continue;
        DAO_PROFILE_STOP(prof, "block1")
    
        DAO_PROFILE_START(prof, "block2")
        for(int x = 0; x < 100000; x++) continue;
        DAO_PROFILE_STOP(prof, "block2")
    
        DAO_PROFILE_EXPORT(prof)
    }

    DAO_PROFILE_EXPORT(prof)
}
*/

#ifndef _DAOPROFILE_H
#define _DAOPROFILE_H

/* ------------------------------------------------------------------------------- */

#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <string>
#include <chrono>
#include <vector>

/* ------------------------------------------------------------------------------- */

#ifdef DAO_PROFILE_ENABLED
#define DAO_PROFILE(cxtSymbol, chronoUnit,...) daoProfile<chronoUnit> cxtSymbol{#cxtSymbol ".csv", {__VA_ARGS__}};
#define DAO_PROFILE_NEW_FRAME(cxt) cxt.newFrame();
#define DAO_PROFILE_START(cxt, blkName) cxt.start(blkName);
#define DAO_PROFILE_STOP(cxt, blkName) cxt.stop(blkName);
#define DAO_PROFILE_EXPORT(cxt) cxt.save();
#else
#define DAO_PROFILE(cxtSymbol, chronoUnit,...)
#define DAO_PROFILE_NEW_FRAME(cxt)
#define DAO_PROFILE_START(cxt, blkName)
#define DAO_PROFILE_STOP(cxt, blkName)
#define DAO_PROFILE_EXPORT(cxt)
#endif

/* ------------------------------------------------------------------------------- */

template <typename T>
class daoProfile {
    using Timepoint = std::chrono::high_resolution_clock::time_point;

    public:
    daoProfile(const std::string &outputFile, const std::vector<std::string> &blocks)
     : m_outputFile(outputFile), m_blocks(blocks)
    {
    }

    inline void newFrame() { m_frames.emplace_back(); }

    inline
    void start(const std::string &blockName) {
        if(std::find(m_blocks.begin(), m_blocks.end(), blockName) != m_blocks.end()) {
            auto t0 = std::chrono::high_resolution_clock::now();
            m_timers[blockName] = t0;
        }
    }

    inline
    void stop(const std::string &blockName) {
        if(std::find(m_blocks.begin(), m_blocks.end(), blockName) != m_blocks.end()) {
            auto t1 = std::chrono::high_resolution_clock::now();
            const auto &t0 = m_timers.at(blockName);
            m_frames.back()[blockName] = std::chrono::duration_cast<T>(t1 - t0);
        }
    }

    void save() {
        std::ofstream csv(m_outputFile);

        // write out block names.
        for(size_t i = 0; i < m_blocks.size(); ++i) {
            csv << m_blocks[i];
            if(i + 1 < m_blocks.size()) { 
                csv << ",";
            }
        }
        csv << "\n";

        // write out block units.
        std::string units = get_unit_string();
        for(size_t i = 0; i < m_blocks.size(); ++i) {
            csv << units;
            if(i + 1 < m_blocks.size()) { 
                csv << ",";
            }
        }
        csv << ",# Units\n";

        // write out frame results.
        for(const auto &frame : m_frames) {
            for(size_t i = 0; i < m_blocks.size(); ++i) {
                const std::string &block = m_blocks[i];
                const auto &result = frame.find(block) == frame.end() ? -1 : frame.at(block).count();
                csv << result;
                if(i + 1 < m_blocks.size()) { 
                    csv << ",";
                }
            }
            csv << "\n";
        }
        
        csv.close();
    }

    private:
    inline std::string get_unit_string() {
        if constexpr (std::is_same_v<T, std::chrono::nanoseconds>) {
            return "ns";
        } else if constexpr (std::is_same_v<T, std::chrono::microseconds>) {
            return "us";
        } else if constexpr (std::is_same_v<T, std::chrono::milliseconds>) {
            return "ms";
        } else if constexpr (std::is_same_v<T, std::chrono::seconds>) {
            return "s";
        } else if constexpr (std::is_same_v<T, std::chrono::minutes>) {
            return "min";
        } else if constexpr (std::is_same_v<T, std::chrono::hours>) {
            return "h";
        } else {
            return "?";
        }
    }

    std::string m_outputFile;
    std::vector<std::string> m_blocks; // array of block names that will be timed across frames.
    std::unordered_map<std::string, Timepoint> m_timers; // holds the initial timers for each block for the current frame.
    std::vector<std::unordered_map<std::string, T>> m_frames; // array of frame results where each entry holds the results for the defined blocks.
};

#endif // _DAOPROFILE_H