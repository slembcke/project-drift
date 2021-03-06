#define DRIFT_SUBSTEPS 4
#define DRIFT_PHYSICS_ITERATIONS 2

#define DRIFT_TICK_HZ 60.0f
#define DRIFT_SUBSTEP_HZ (DRIFT_TICK_HZ*DRIFT_SUBSTEPS)

#define DRIFT_PLAYER_COUNT 1

#define DRIFT_PLAYER_SPEED 300
#define DRIFT_QUICKSLOT_COUNT 5 // Slot 0 is always FLY mode.
#define DRIFT_PLAYER_CARGO_SLOT_COUNT 4
#define DRIFT_PLAYER_SIZE 14

#define DRIFT_POWER_BEAM_RADIUS (DRIFT_PLAYER_SIZE + 2)
#define DRIFT_POWER_EDGE_MIN_LENGTH 128

typedef enum {
	DRIFT_COLLISION_TYPE_TERRAIN,
	DRIFT_COLLISION_TYPE_PLAYER,
	DRIFT_COLLISION_TYPE_ITEM,
	DRIFT_COLLISION_TYPE_BUG,
	DRIFT_COLLISION_TYPE_PLAYER_BULLET,
} DriftCollisionType;
