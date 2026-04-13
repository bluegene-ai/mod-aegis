# mod-ac-aegis Plan

## Goal

Build a new AzerothCore anti-cheat module that combines the strongest parts of
mod-ac-sentinel and mod-anticheat while staying inside the current module and
hook boundaries.

## Constraints

- Target: AzerothCore WotLK 3.3.5a.
- No mandatory core edits.
- Must use existing PlayerScript passive anticheat hooks and
  MovementHandlerScript.
- Characters DB table additions are allowed.
- Automatic handling is required, including ban escalation.
- False positives must be reduced compared to mod-anticheat.
- Missed detections for low gravity super jump and wall climb must be reduced
  compared to mod-ac-sentinel.

## Design Direction

The new module will not merge both existing mods mechanically. It will instead
reuse their proven ideas in a different structure:

1. High-frequency movement entry stays lightweight.
2. AzerothCore legality semantics remain the primary authority.
3. Geometry and reachability are only consulted after suspicious movement.
4. Single events can trigger rollback, but not irreversible punishment.
5. Persistent punishment state is separated from transient movement state.

## Phase Plan

### Phase 1: Context and Interface Mapping

- Confirm current AzerothCore anticheat hooks.
- Confirm map height, collision, and reachability APIs.
- Compare mod-ac-sentinel and mod-anticheat behavior and failure modes.
- Map actual live threat model to WoWAdminPanel-style cheats.

### Phase 2: Formal Design

- Define module identity and component boundaries.
- Define evidence model and punishment ladder.
- Define each target cheat through the system data flow.
- Define persistence and configuration layout.

### Phase 3: Blueprint

- Fix directory structure.
- Fix file-level responsibilities.
- Define MVP and enhanced milestones.
- Define what ships in the no-core-edit implementation.

### Phase 4: Implementation

- Create the new module folder and registration entry.
- Implement configuration, geometry helpers, persistence, and manager.
- Implement six behavior families:
  - speed
  - teleport
  - noclip through walls or doors
  - fly or air suspension
  - climb or super jump
  - fixed-position gathering AFK bot
- Implement progressive punishments and DB persistence.

### Phase 5: Validation

- Check editor diagnostics.
- Attempt project build validation.
- Record any remaining verification gaps.

## Deliverables

- docs/PLAN.md
- docs/DESIGN.md
- docs/BLUEPRINT.md
- full new module under modules/mod-ac-aegis