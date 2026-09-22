#include "rtengine/refreshmap.h"

#include <cstdlib>
#include <iostream>

int main()
{
    using namespace rtengine;
    const auto* mapper = RefreshMapper::getInstance();
    const ProcEvent events[] = {
        EvExpComp, EvToneCurve1, EvToneCurve2, EvRGBMasterCurve,
        EvRGBrCurve, EvRGBgCurve, EvRGBbCurve, EvRGBEnabled,
        EvRGBrCurveLumamode
    };
    const char* value = std::getenv("STEEP_LEGACY_TONE");
    const bool legacy = value && value[0] != '\0' && value[0] != '0';
    for (const auto& event : events) {
        const int flags = mapper->getAction(event);
        if (flags != (legacy ? AUTOEXP : TONE)) {
            std::cerr << "Unexpected refresh for event " << int(event) << '\n';
            return 1;
        }
    }
    // Geometry and white balance must still invalidate upstream work.
    if (!(mapper->getAction(EvROTDegree) & M_TRANSFORM)
        || !(mapper->getAction(EvWBTemp) & M_INIT)) {
        std::cerr << "Upstream refresh was lost\n";
        return 1;
    }
    std::cout << "Interactive refresh tests passed (legacy=" << legacy << ")\n";
}
