#ifndef MOD_AC_AEGIS_GEOMETRY_H
#define MOD_AC_AEGIS_GEOMETRY_H

#include <string>

#include "AcAegisTypes.h"

class GameObject;
class Player;

struct AegisGeometryResult
{
    bool checked = false;
    bool blocked = false;
    bool reachable = true;
    bool pathExists = true;
    bool hitValid = false;
    // True only when the nearest collision hit could be attributed to the static (vmap) tree.
    // The navmesh is baked from static geometry only - dynamic gameobjects (doors, gates,
    // elevators) are not part of it - so its "walkable" verdict may only excuse a segment
    // whose blocker is static. An unattributable block counts as "not static", which keeps
    // the stricter pre-existing behaviour instead of silently excusing the segment.
    bool staticBlocker = true;
    float hitX = 0.0f;
    float hitY = 0.0f;
    float hitZ = 0.0f;
    float pathLength = 0.0f;
    float directDistance = 0.0f;
    std::string reason;
};

struct AegisDoorCrossResult
{
    GameObject* door = nullptr;
    bool crossed = false;
    bool collisionConfirmed = false;    // the collision ray hit this door's own slab
    float lateralOffset = 0.0f;
    float hitPlaneDistance = 0.0f;
};

class AcAegisGeometry
{
public:
    bool GetGroundHeight(Player* player, float x, float y, float z, float& groundZ) const;

    // Returns true when the segment is blocked. hitX/hitY/hitZ receive the real
    // collision hit position when the collision trees report one; when they do not
    // (which should not normally happen for a segment reported as blocked) the
    // segment midpoint is used as a fallback and hitPointValid is set to false.
    // staticBlocker reports whether that hit came from the static (vmap) tree; it is
    // false when the hit came from a gameobject or could not be attributed at all.
    bool RaycastStaticAndDynamic(Player* player,
        float startX, float startY, float startZ,
        float endX, float endY, float endZ,
        float& hitX, float& hitY, float& hitZ,
        bool* hitPointValid = nullptr,
        bool* staticBlocker = nullptr) const;

    AegisGeometryResult CheckShortSegment(Player* player,
        AegisMoveSample const& from,
        AegisMoveSample const& to,
        bool allowReachability) const;

    AegisGeometryResult CheckLongPath(Player* player,
        AegisMoveSample const& from,
        AegisMoveSample const& to) const;

    // Reports whether the segment crosses the extent of a door that is closed right now.
    // Doors are modelled from their base upward, and the check works from the door's own
    // position/orientation, so it needs no collision model at all: it keeps detecting the
    // "client MPQ removed the door collision" cheat even when the door's model is missing,
    // disabled for LoS or baked open in the vmaps. collisionConfirmed tells whether the
    // collision ray's own hit sits on this door's slab (the two layers agreeing).
    AegisDoorCrossResult CheckClosedDoorCross(Player* player,
        AegisMoveSample const& from,
        AegisMoveSample const& to,
        float hitX, float hitY, float hitZ, bool hitValid) const;

private:
    void SanitizeGroundHeight(Player* player, float x, float y, float z, float& groundZ) const;
};

#endif