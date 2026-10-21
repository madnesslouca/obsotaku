#include "../../frontend/utility/StudioVodRouting.hpp"
#include <iostream>
#include <stdexcept>
static int checks;
static void check(bool value, const char *name)
{
    if (!value) throw std::runtime_error(name);
    ++checks;
}
int main()
{
    try {
        auto plan = ResolveStudioVodRouting({}, {});
        check(plan.liveMask == 1 && plan.vodMask == 2, "default live 1 clean 2");
        check(!plan.configured && !plan.conflict && !plan.invalid, "disabled VOD clearly a preview");
        plan = ResolveStudioVodRouting({0,1,true}, {});
        check(plan.configured && plan.vodMask == 2, "enabled VOD");
        plan = ResolveStudioVodRouting({0,0,true}, {});
        check(plan.conflict, "same live/VOD track refused");
        plan = ResolveStudioVodRouting({0,1,true}, {{3,2,true},{0,1,true}});
        check(plan.liveMask == 9 && plan.vodMask == 6 && !plan.conflict, "multistream union");
        check(MergeStudioVodRouting(255, plan.vodMask, 0, plan.liveMask) == 249, "exclude both VOD mixes without changing other bits");
        check(MergeStudioVodRouting(249, plan.vodMask, plan.vodMask, plan.liveMask) == 255, "include both VOD mixes");
        check(MergeStudioVodRouting(251, plan.vodMask, 0, plan.liveMask) == 249, "partially included VOD goes fully excluded");
        plan = ResolveStudioVodRouting({0,1,true}, {{1,2,true}});
        check(plan.conflict, "one destination VOD used by another live refused");
        check(MergeStudioVodRouting(255, 2, 0, 3) == 255, "currently live mix never cleared");
        check(MergeStudioVodRouting(247, 2, 0, 1) == 245, "unrelated recording track edit preserved");
        check(MergeStudioVodRouting(245, 2, 2, 1) == 247, "undo restores only VOD bit");
        check(MergeStudioVodRouting(245, 2, 2, 3) == 245, "undo protects track newly used live");
        check(MergeStudioVodRouting(255, 255, 0, 1) == 193, "never edits beyond six supported mixes");
        check(ResolveStudioVodRouting({-1,1,true}, {}).invalid, "invalid profile live index");
        check(ResolveStudioVodRouting({0,6,false}, {}).invalid, "invalid preview track refused");
        check(ResolveStudioVodRouting({}, {{7,1,false}}).invalid, "invalid destination live index");
        check(ResolveStudioVodRouting({}, {{0,-1,true}}).invalid, "invalid enabled VOD index");
        check(!ResolveStudioVodRouting({}, {{0,-1,false}}).invalid, "disabled destination VOD irrelevant");
        check(ResolveStudioVodRouting({5,4,true}, {}).liveMask == 32, "last available native mix");
        std::cout << "{\"passed\":true,\"checks\":" << checks << "}\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
