#include "renderer/engine/vertex_constant_ranges.h"
#include <cassert>
#include <cstdio>
#include <random>
using DarkRecomp::Native::VertexConstantRanges;
int main() {
    std::mt19937 random(2771);
    for(unsigned trial=0;trial<100000;++trial) {
        VertexConstantRanges ranges;std::array<bool,256> expected{},actual{};
        const unsigned count=random()%33;
        for(unsigned n=0;n<count;++n) {
            const unsigned first=random()%256,length=random()%(257-first);
            ranges.add(first,length);
            for(unsigned row=first;row<first+length;++row)expected[row]=true;
        }
        assert(ranges.valid);ranges.merge();
        unsigned previousEnd=0;
        for(unsigned n=0;n<ranges.count;++n) {
            const auto [first,length]=ranges.values[n];
            assert(length && first+length<=256 && (!n || first>previousEnd));
            for(unsigned row=first;row<first+length;++row)actual[row]=true;
            previousEnd=first+length;
        }
        assert(expected==actual);
        const auto before=ranges; ranges.merge();
        assert(before.count==ranges.count);
        for(unsigned n=0;n<ranges.count;++n)
            assert(before.values[n].first==ranges.values[n].first && before.values[n].count==ranges.values[n].count);
    }
    VertexConstantRanges overlap;overlap.add(0,4);overlap.add(1,1);overlap.add(4,4);overlap.add(7,2);overlap.merge();
    assert(overlap.count==1 && overlap.values[0].first==0 && overlap.values[0].count==9);
    VertexConstantRanges overflow;
    for(unsigned n=0;n<33;++n)overflow.add(n,1);
    assert(!overflow.valid);overflow.merge();assert(!overflow.valid);
    for(const auto [first,count]: {std::pair{256u,1u},std::pair{255u,2u},std::pair{1u,UINT32_MAX}}) {
        VertexConstantRanges invalid;invalid.add(first,count);assert(!invalid.valid);
    }
    std::puts("Vertex ranges: 100000 randomized unions, overlaps, adjacency, bounds, overflow and idempotent merge pass.");
}
