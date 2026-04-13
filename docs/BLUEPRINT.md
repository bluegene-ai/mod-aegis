# mod-ac-aegis Blueprint

## Directory Structure

modules/mod-ac-aegis/

- conf/AcAegis.conf.dist
- data/sql/db-characters/base/001_ac_aegis_tables.sql
- docs/PLAN.md
- docs/DESIGN.md
- docs/BLUEPRINT.md
- src/AcAegis.cpp
- src/AcAegisConfig.h
- src/AcAegisConfig.cpp
- src/AcAegisGeometry.h
- src/AcAegisGeometry.cpp
- src/AcAegisPersistence.h
- src/AcAegisPersistence.cpp
- src/AcAegisMgr.h
- src/AcAegisMgr.cpp
- src/AcAegisScripts.h
- src/AcAegisScripts.cpp
- src/AcAegisTypes.h

## Main Classes

### AcAegisConfig

- loads and sanitizes configuration
- keeps thresholds, grace windows, actions, and offense policy centralized

### AcAegisGeometry

- provides ground lookup
- performs VMAP and dynamic object collision raycasts
- validates suspicious movement reachability against core map helpers

### AcAegisPersistence

- ensures required schema exists
- loads and saves offense state
- inserts compact event summaries

### AcAegisMgr

- owns online player contexts
- samples movement
- runs detection pipeline
- aggregates risk
- drives automatic responses

### Script Layer

- registers PlayerScript, MovementHandlerScript, and WorldScript bindings
- feeds movement, teleport, flight, jump, and gather-related events into the
  manager

## MVP Scope

The MVP in this implementation is intentionally practical rather than maximal.

Included in MVP:

- speed
- teleport
- noclip through wall or door
- fly or air suspension
- climb and super jump
- fixed-position gather AFK botting
- persistent offense ladder
- rollback, debuff, jail, kick, and ban actions

Deferred for future iterations:

- deep async geometry queues
- GM command suite
- richer analytics dashboards
- per-zone threshold overrides

## Implementation Order

1. shared types and config
2. geometry and persistence helpers
3. manager with sample ingestion and risk decay
4. movement detectors
5. gather AFK detector
6. punishment and ban ladder
7. script registration and startup wiring
8. validation and threshold review

## Minimum Feature Set Required For Real Deployment

To satisfy the requirement of no-core-edit deployment with automatic handling
and repeat-offense escalation, the following must all exist in stage one:

- movement sample ring buffer
- legal-speed based speed detector
- coordinate and burst teleport detector
- geometry-backed noclip detector
- fly contradiction detector
- climb or super-jump detector
- offense persistence table
- automatic rollback, debuff, jail, kick, and ban chain

## Verification Checklist

- module loads without core edits
- schema auto-ensure works
- movement hooks fire and populate context
- safe-position rollback works after strong teleport and clipping events
- jail blocks jail escape teleports
- repeated punished incidents escalate across relogs
- ban path reuses AzerothCore BanMgr