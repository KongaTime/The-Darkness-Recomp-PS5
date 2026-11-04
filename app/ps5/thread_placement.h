#pragma once
#include <array>
#include <cstdint>

namespace DarkRecomp::Ps5 {
struct ThreadPlacementResult {
    enum Status { invalidMask, queryFailed, setFailed, applied } status=invalidMask;
    std::array<uint64_t,2> target{},before{},after{};
    std::array<int,2> setCodes{},rollbackCodes{};
};
// Debug-only, reversible placement of our two registered threads. A zero
// request restores that thread's original mask. Never expand its allowed set.
template<class Get,class Set>
ThreadPlacementResult applyThreadPlacement(const std::array<uint64_t,2>& original,
                                           const std::array<uint64_t,2>& request,Get get,Set set) {
    ThreadPlacementResult result;
    for(unsigned i=0;i<2;++i) {
        result.target[i]=request[i]?request[i]:original[i];
        if(!original[i] || !result.target[i] || (result.target[i]&~original[i]))return result;
    }
    for(unsigned i=0;i<2;++i)if(get(i,result.before[i])) {
        result.status=ThreadPlacementResult::queryFailed;return result;
    }
    result.status=ThreadPlacementResult::applied;
    for(unsigned i=0;i<2;++i)if((result.setCodes[i]=set(i,result.target[i]))!=0) {
        result.status=ThreadPlacementResult::setFailed;
        for(unsigned j=0;j<2;++j)result.rollbackCodes[j]=set(j,result.before[j]);
        break;
    }
    for(unsigned i=0;i<2;++i)get(i,result.after[i]);
    return result;
}
}
