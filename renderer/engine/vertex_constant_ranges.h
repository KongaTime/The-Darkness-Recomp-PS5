#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace DarkRecomp::Native {
// Preparation recipes describe immutable, owned banks. Merging overlapping
// reads is valid only for such a bank, never for concurrent live guest reads.
// Adapted from upstream d0b6e6a (PR #144).
struct VertexConstantRanges {
    struct Range {uint16_t first=0,count=0;};
    std::array<Range,32> values{};
    unsigned count=0;
    bool valid=true;
    void add(unsigned first,unsigned length) noexcept {
        if(!valid)return;
        if(first>=256 || length>256-first || count==values.size()) {valid=false;return;}
        if(length)values[count++]={uint16_t(first),uint16_t(length)};
    }
    void merge() noexcept {
        if(!valid || count<2)return;
        std::sort(values.begin(),values.begin()+count,[](const Range& a,const Range& b) {
            return a.first!=b.first?a.first<b.first:a.count<b.count;
        });
        unsigned merged=0;
        for(unsigned n=0;n<count;++n) {
            const auto range=values[n];const unsigned end=range.first+range.count;
            if(merged && range.first<=values[merged-1].first+values[merged-1].count) {
                auto& previous=values[merged-1];
                if(end>previous.first+previous.count)previous.count=uint16_t(end-previous.first);
            } else values[merged++]=range;
        }
        count=merged;
    }
};
}
