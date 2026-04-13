# mod-ac-aegis Design

## 1. Module Positioning

### Suggested Name

mod-ac-aegis

The name reflects the module's role: a defensive shield built on top of
AzerothCore's own legal movement semantics, not a detached heuristic sandbox.

### Design Goals

mod-ac-aegis is designed to detect and handle six behavior families that matter
in live private server operations:

- speed hack
- teleport
- wall or door clipping
- fly or air suspension
- wall climb or super jump
- fixed-position gathering AFK botting

The module explicitly targets the kind of trainer behavior commonly exposed by
WoWAdminPanel-style tools: coordinate relocation, flight state abuse, movement
envelope bypass, geometry crossing, and repetitive automation around resource
farming.

### Core Modification Requirement

The design is intended to ship without AzerothCore core edits.

It depends on existing hook and engine capabilities:

- MovementHandlerScript::OnPlayerMove
- PlayerScript passive anticheat hooks
- teleport lifecycle hooks
- loot and gathering-related player hooks
- Map::GetHeight
- Map::GetObjectHitPos
- Map::CanReachPositionAndGetValidCoords
- PathGenerator
- BanMgr

### Why This Is Better Than Copying an Existing Module

Directly copying mod-ac-sentinel leaves known blind spots around steep-climb and
low-gravity jump abuse. Directly copying mod-anticheat preserves stronger raw
coverage but also preserves a higher false-positive profile because it reacts
too aggressively to packet-level symptoms without enough semantic grace and
geometry confirmation.

mod-ac-aegis keeps the stricter trigger coverage, but moves heavy confidence
building into a second stage that consults AzerothCore's map and reachability
systems before escalating beyond rollback or observation.

## 2. Architectural Principles

### Reuse AzerothCore Legality First

The module does not define a separate concept of legal movement if AzerothCore
already exposes one. Legal speed comes from server speed state. Legal flight
comes from core can-fly state, auras, and ack windows. Legal reachability comes
from map collision and reachability helpers. Legal teleports are learned from
the existing teleport lifecycle.

### Keep Hot Path Light

The high-frequency path only performs cheap tests:

- recent sample deltas
- current legal speed lookup
- movement flags
- grace windows
- limited slope and ground comparisons

Collision raycasts and reachability checks run only after the movement segment
already looks suspicious.

### Do Not Overreact to One Noisy Packet

Single strong events may justify rollback, but not immediate irreversible
punishment. Debuff, jail, kick, and ban depend on accumulated risk and a
persistent offense ladder.

### Geometry Must Be Backed by Core Systems

Wall and door crossing are not judged by coordinate math alone. The design uses
VMAP and dynamic object collision together with core reachability helpers. This
is essential for closed-instance-door and model-edit scenarios.

## 3. System Architecture

The module is organized around the movement and evidence pipeline rather than a
flat detector list.

### Packet Entry Fast Filter Layer

This layer receives movement and related state transitions. Its purpose is to
sample state, enforce grace windows, and decide whether a suspicious segment is
worth deeper inspection.

Inputs:

- movement packets through MovementHandlerScript
- server movement-authorization changes through PlayerScript anticheat hooks
- jump opcode state through PlayerScript anticheat hooks
- spell cast, teleport, and map change hooks
- loot and gathering hooks for AFK behavior

### Player Semantic State Layer

This layer keeps a short movement ring buffer and a semantic context for each
online player:

- recent positions and timestamps
- recent legal speed
- recent legitimate mobility windows
- recent can-fly, mount ack, and root ack timings
- last known safe rollback position
- gather-bot action window
- persistent punishment state loaded from DB

This layer is where noise reduction lives.

### Geometry and Reachability Review Layer

Only suspicious segments enter this layer. It answers a narrower question: is
the segment merely unusual, or does AzerothCore itself consider the line or
destination blocked or unreachable?

Inputs to this layer are already filtered for transport, swim, fall, legal fly,
and teleport grace.

### Evidence Aggregation and Risk Layer

Every confirmed or semantically strong event produces an evidence object with:

- cheat family
- evidence level
- risk contribution
- whether rollback is recommended
- optional geometry confirmation details

Risk decays over time but offense history does not disappear immediately.

### Automatic Response and Repeat-Offense Layer

This layer transforms risk and offense history into actions:

- log and GM notification
- rollback
- debuff
- jail
- kick
- temporary ban
- permanent ban

The action ladder is intentionally conservative about bans and aggressive about
rollback. This keeps gameplay protected while reducing irreversible mistakes.

## 4. AzerothCore Capability Mapping

### Movement Entry

MovementHandler.cpp already routes movement through passive anticheat hooks and
OnPlayerMove. This is the correct place for lightweight sample-based detection.

### Passive Anticheat Hooks

PlayerScript exposes movement-related hooks that provide server semantic state:

- can-fly set by server
- mount ack
- root ack
- jump opcode state
- updated movement info
- pre-movement validation hook

These hooks let the module reason about whether a movement flag is expected or
contradicting server state.

### Collision, Height, and Reachability

The following AzerothCore capabilities are reused directly:

- Map::GetHeight for ground reference
- Map::GetObjectHitPos for dynamic object collision, including doors and WMOs
- VMAP object hit tests for static collision
- Map::CanReachPositionAndGetValidCoords for collision-aware and slope-aware
  reachability validation
- PathGenerator for path existence and path length comparison when needed

### Flight and Teleport Semantics

Legal flight is not inferred from a custom whitelist alone. It is derived from:

- server-side can-fly changes
- fly auras
- mounted flight speed auras
- recent mount-related ack state

Legal teleport is learned from:

- OnPlayerBeforeTeleport
- IsBeingTeleported semantics
- map change grace windows

## 5. Cheat Coverage Through the Main Pipeline

### Speed

Speed belongs to the packet entry and semantic state layers. The detector uses
the server-authoritative speed for the current movement mode and compares it to
observed 2D travel over time.

Key trigger:

- observed 2D velocity exceeds legal speed envelope plus tolerance

Noise control:

- ignore very small and very stale packet intervals
- ignore transport, vehicle, and teleport grace windows
- use 2D instead of 3D distance to avoid stairs and slope inflation

Evidence strength:

- medium for modest but repeatable excess
- strong only for clearly impossible envelope violations

Speed evidence usually contributes risk and notification first. It does not
ban by itself on the first incident.

### Teleport

Teleport starts in the fast filter layer. Long-distance impossible bursts and
large single-axis jumps become evidence immediately.

Key trigger:

- movement burst far beyond legal speed envelope
- or coordinate jump with large axis delta

Noise control:

- teleport grace from server teleports
- map change grace
- mobility spell grace

Strong evidence:

- large coordinate jump well beyond legal travel envelope

A strong teleport event can trigger immediate rollback on the first incident,
but the punishment ladder above rollback still respects risk and offense
history.

### Wall or Door Clipping

This behavior is intentionally not judged on packet math alone. A suspicious
segment is first selected in the fast layer, then the geometry layer checks:

- static VMAP collision hit
- dynamic object collision hit
- reachability through Map::CanReachPositionAndGetValidCoords

Key trigger:

- short or medium segment that is not a teleport burst, but crosses blocked
  geometry while still appearing walk-like from the packet side

Noise control:

- cooldown between geometry checks
- skip swim, fall, transport, and flight states
- skip segments already classified as teleport bursts

Strong evidence:

- blocked ray plus unreachable destination
- blocked dynamic collision through door or gameobject space

### Fly or Air Suspension

Fly handling belongs to both the semantic state layer and the geometry layer.
The module checks for a contradiction between movement flags and legal flight
state, then confirms air height above ground.

Key trigger:

- flying or can-fly style flags without server authorization or flight aura
- sustained horizontal air movement above normal ground clearance
- disable-gravity style air suspension without a legal reason

Noise control:

- can-fly grace window
- mounted-flight aura checks
- recent mount ack checks
- skip taxi flight, vehicles, and transports

Strong evidence:

- significant height above ground plus illegal fly state plus sustained air
  motion

### Wall Climb or Super Jump

This is where the design explicitly diverges from mod-ac-sentinel.

Climb is modeled as repeated steep uphill motion that is too short and too
vertical to be normal walking, while not being explained by jump, fall, swim,
vehicle, or legal fly state.

Super jump is modeled separately as an abnormal vertical burst or apex above the
local ground reference shortly after jump intent.

Key trigger:

- repeated steep non-falling uphill segments
- or abnormal jump apex over local ground within a short jump window

Noise control:

- require ground-relative validation
- require short timing window
- require repetition for climb escalation
- exclude legal jump or fall states where appropriate

Strong evidence:

- repeated steep blocked climb segments
- jump apex far beyond normal local ground envelope

### Fixed-Position Gathering AFK Botting

This behavior does not come from movement packets first. It comes from loot and
gather-related hooks and uses movement state only for confirmation.

Key trigger:

- repeated gather or loot actions inside a long window
- while the total displacement stays under a very small radius

Noise control:

- long observation window
- minimum gap between counted actions
- suppression after allowed contextual actions or whitelisted spells

Strong evidence:

- long-duration fixed-position repeated gathering with very low movement

This is intentionally configured to be conservative by default because manual
farming can resemble automation over short windows.

## 6. Automatic Handling and Repeat Offense

### Action Chain

The full action chain is built into the design:

- record event summary
- notify GMs
- rollback to last safe position
- apply debuff
- jail
- kick
- ban

### Action Priorities

Rollback has higher operational priority than punishment escalation because it
immediately protects the world state.

Default order of escalation:

1. notification and rollback
2. debuff
3. jail
4. kick
5. temporary ban
6. permanent ban

### Repeat Offense Rules

Persistent offense state is stored per character with account linkage for ban
actions.

Suggested default progression:

1. first punished incident: debuff and observation tail
2. second punished incident: jail
3. third punished incident: kick
4. fourth punished incident: temporary ban
5. fifth punished incident: permanent ban, if enabled

### Why Single Misreports Do Not Directly Ban

Ban execution requires both:

- high current risk
- and repeated offense history

By default, automatic bans also require strong evidence. This keeps obvious
teleport or noclip abuse actionable while reducing the chance of banning on one
isolated false signal.

## 7. Configuration Design

The namespace is AcAegis and is grouped by system layer rather than only by
detector names.

Major groups:

- AcAegis.Global
- AcAegis.Log
- AcAegis.Sampling
- AcAegis.Accuracy
- AcAegis.Detector
- AcAegis.Geometry
- AcAegis.Risk
- AcAegis.AutoAction
- AcAegis.Offense
- AcAegis.Persistence

Recommended defaults:

- speed, teleport, noclip, fly, climb enabled
- AFK gather detection enabled but conservative
- rollback, debuff, jail, kick enabled
- ban enabled with strong-evidence and repeat-offense guardrails

## 8. Persistence Design

### In-Memory Only

- recent movement samples
- grace windows
- pending risk score
- current gather action window

### Event Summaries in Characters DB

- cheat family
- evidence level
- risk delta and total risk after event
- map and position context
- short detail text

### Persistent Punishment State in Characters DB

- offense count
- offense tier
- current punishment stage
- punishment expiry timestamps
- last cheat family and reason

### Ban Storage Strategy

The module does not invent a new ban subsystem. It records its own offense state
but delegates real ban execution to AzerothCore's BanMgr and therefore reuses
the core auth and characters ban ecosystem.

## 9. Performance and Stability

High-frequency path:

- speed envelope
- teleport burst math
- flight flag validation
- slope and local-ground checks

Throttled path:

- geometry raycasts
- reachability validation
- path generation

Controls used to keep the server stable:

- geometry cooldown per player
- strict grace windows to avoid noisy rechecks
- short event summaries instead of full packet dumps
- persistent storage only for summarized events and offense state

Need to verify:

- exact VMAP and dynamic collision behavior for every instance door variant
- live thresholds against real latency and slope-heavy map areas

## 10. Relationship to Existing Implementations

### What Is Reused Conceptually

From mod-ac-sentinel:

- sampled state model
- progressive punishment ladder
- geometry-backed noclip validation
- risk aggregation

From mod-anticheat:

- stronger single-event trigger philosophy
- explicit climb and vertical anomaly attention
- heavier focus on packet-level movement contradictions

### What Is Deliberately Not Copied

- raw one-hit escalation from noisy packet symptoms
- over-reliance on packet math without geometry confirmation for clipping
- weaker treatment of wall climb and low-gravity super jump

### Main Improvements

- climb and super-jump are first-class citizens
- speed detection uses 2D envelope semantics to reduce slope false positives
- clipping detection is built around core collision and reachability rather than
  packet symptoms alone
- bans remain automatic-capable but require stronger confidence structure

## 11. Implementation Blueprint Summary

The implementation is split into:

- configuration
- types and context
- geometry helper
- persistence helper
- manager
- script registration

The first practical milestone is not every advanced detector. It is the set
that already provides useful enforcement without core edits:

- speed
- teleport
- noclip through wall or door
- fly
- climb and super jump
- progressive punishment and offense persistence

AFK gather automation rides on top of the same context model and lands in the
same module release.

## 12. Optional Future Enhancements

If core edits are allowed later, the smallest valuable hook additions would be:

- a dedicated callback after server-side speed changes are acknowledged
- a dedicated hook for movement correction and rollback telemetry
- a dedicated hook for gather-node interaction start and completion

These would reduce ambiguity for speed, anti-knockback differentiation, and AFK
gather automation classification.

## Summary

This design is viable without core edits.

Largest false-positive risk:

- unusual terrain, stairs, ramps, or complex doors causing geometry ambiguity

Largest false-negative risk:

- cheats that mimic legal server-authorized state changes closely enough to stay
  under both packet and geometry thresholds

Best first three implementation targets:

1. teleport plus rollback
2. noclip through wall or door with geometry confirmation
3. climb and super-jump handling with local-ground validation

Suggested default punishment tiers:

- first punished incident: debuff plus rollback when applicable
- repeated punished incident: jail, then kick
- severe repeat offender: temporary ban, then permanent ban