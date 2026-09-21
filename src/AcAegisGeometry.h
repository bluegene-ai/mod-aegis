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
    bool hitValid = false;
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

    // Returns true when the segment is blocked. hitX/hitY/hitZ receive the real
    // collision hit position when the collision trees report one; when they do not
    // (which should not normally happen for a segment reported as blocked) the
    // segment midpoint is used as a fallback and hitPointValid is set to false.
    bool RaycastStaticAndDynamic(Player* player,
        float startX, float startY, float startZ,
        float endX, float endY, float endZ,
        float& hitX, float& hitY, float& hitZ,
        bool* hitPointValid = nullptr) const;

    AegisGeometryResult CheckShortSegment(Player* player,
        AegisMoveSample const& from,
        AegisMoveSample const& to,
        bool allowReachability) const;

    AegisGeometryResult CheckLongPath(Player* player,
        AegisMoveSample const& from,
        AegisMoveSample const& to) const;

private:
    void SanitizeGroundHeight(Player* player, float x, float y, float z, float& groundZ) const;
};

#endif