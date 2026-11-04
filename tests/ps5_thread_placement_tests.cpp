#include "app/ps5/thread_placement.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <utility>

int main() {
    using namespace DarkRecomp::Ps5;
    std::array<uint64_t,2> masks{0x1FFF,0x1FFF};
    const auto original=masks;
    std::vector<std::pair<unsigned,uint64_t>> changes;
    bool failRender=false,failQuery=false;
    auto get=[&](unsigned role,uint64_t& mask){mask=masks[role];return failQuery?-7:0;};
    auto set=[&](unsigned role,uint64_t mask){
        changes.emplace_back(role,mask);
        if(failRender && role==1 && mask==0x10)return -9;
        masks[role]=mask;return 0;
    };
    auto result=applyThreadPlacement(original,{1,0x10},get,set);
    assert(result.status==ThreadPlacementResult::applied && masks[0]==1 && masks[1]==0x10);
    assert(result.before==original && result.after==masks);
    result=applyThreadPlacement(original,{0,0},get,set);
    assert(result.status==ThreadPlacementResult::applied && masks==original);
    changes.clear();
    result=applyThreadPlacement(original,{1,0x8000},get,set);
    assert(result.status==ThreadPlacementResult::invalidMask && changes.empty() && masks==original);
    failQuery=true;
    result=applyThreadPlacement(original,{1,0x10},get,set);
    assert(result.status==ThreadPlacementResult::queryFailed && changes.empty() && masks==original);
    failQuery=false;
    masks={0x3,0x30};const auto before=masks;failRender=true;
    result=applyThreadPlacement(original,{1,0x10},get,set);
    assert(result.status==ThreadPlacementResult::setFailed && masks==before && result.after==before);
    assert(changes.size()==4 && changes[2].first==0 && changes[2].second==before[0] && changes[3].second==before[1]);
    assert(result.setCodes[0]==0 && result.setCodes[1]==-9 && result.rollbackCodes[0]==0 && result.rollbackCodes[1]==0);
    changes.clear();
    result=applyThreadPlacement(std::array<uint64_t,2>{0,0x1FFF},{1,0x10},get,set);
    assert(result.status==ThreadPlacementResult::invalidMask && changes.empty());
    std::puts("Thread placement: allowed masks, restore, failed-query no-op and failed-pair rollback pass.");
}
