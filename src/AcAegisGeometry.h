#ifndef MOD_AC_AEGIS_GEOMETRY_H
#define MOD_AC_AEGIS_GEOMETRY_H

#include <string>

#include "AcAegisTypes.h"

class Player;

struct AegisGeometryResult
{
    bool checked = false;
    bool blocked = false;
    bool reachable = true;
    bool pathExists = true;
    float hitX = 0.0f;
    float hitY = 0.0f;
    float hitZ = 0.0f;
    float pathLength = 0.0f;
    float directDistance = 0.0f;
    std::string reason;
};

class AcAegisGeometry
{
public:
    bool GetGroundHeight(Player* player, float x, float y, float z, float& groundZ) const;

    bool RaycastStaticAndDynamic(Player* player,
        float startX, float startY, float startZ,
        float endX, float endY, float endZ,
        float& hitX, float& hitY, float& hitZ) const;

    AegisGeometryResult CheckShortSegment(Player* player,
        AegisMoveSample const& from,
        AegisMoveSample const& to,
        bool allowReachability) const;

    AegisGeometryResult CheckLongPath(Player* player,
        AegisMoveSample const& from,
        AegisMoveSample const& to) const;
};

#endif