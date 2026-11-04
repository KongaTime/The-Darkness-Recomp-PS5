#pragma once
#include "world_mesh.h"

namespace DarkRecomp::Native {
// Internal queue/pool capability. Only seal snapshots populated by capture,
// after the queue has finished assigning images. Public mutable captures never
// receive this tag, and aliases prevent exclusive immutable queue ownership.
class QueuedWorldDrawAccess {
public:
    static std::shared_ptr<const WorldDraw> seal(std::shared_ptr<WorldDraw>&& draw) {
        if (draw) draw->queueOwnership_.value = draw.use_count()==1;
        return std::move(draw);
    }
    static bool owned(const WorldDraw& draw) {return draw.queueOwnership_.value;}
    static void recycled(WorldDraw& draw) {draw.queueOwnership_.value=false;}
};
}
