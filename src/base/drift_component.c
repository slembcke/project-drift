#include <stdlib.h>
#include <string.h>

#include "drift_types.h"
#include "drift_util.h"
#include "drift_mem.h"
#include "drift_table.h"
#include "drift_map.h"
#include "drift_entity.h"
#include "drift_component.h"

DriftComponent* DriftComponentInit(DriftComponent* component, DriftTableDesc desc){
	*component = (DriftComponent){.gc_cursor = 1};
	
	// Add one for the reserved 0 index.
	desc.min_row_capacity += 1;
	
	DriftTableInit(&component->table, desc);
	DriftName name = component->table._names[0];
	DRIFT_ASSERT_WARN(name.str[0] == '@', "Component '%s' should start with a '@'", name);
	name.str[0] = '#';
	DriftMapInit(&component->map, component->table.desc.mem, name.str, desc.min_row_capacity);
	
	// Reserve 0 index;
	component->table.row_count = 1;
	
	return component;
}

void DriftComponentDestroy(DriftComponent* component){
	DriftTableDestroy(&component->table);
	DriftMapDestroy(&component->map);
}

void DriftComponentIO(DriftComponent* component, DriftIO* io){
	DriftTableIO(&component->table, io);
	if(io->read){
		// Fixup the count
		component->count = component->table.row_count - 1;
		// Generate index map.
		DriftEntity* key = DriftComponentGetEntities(component);
		DRIFT_COMPONENT_FOREACH(component, idx) DriftMapInsert(&component->map, key[idx].id, idx);
	}
}

uint DriftComponentAdd(DriftComponent *component, DriftEntity entity){
	DriftTableEnsureCapacity(&component->table, component->table.row_count + 1);
	
	uint idx = component->count = component->table.row_count++;
	uint old_idx = DriftMapInsert(&component->map, entity.id, idx);
	DRIFT_ASSERT(old_idx == 0, "e%d already had a %s", entity.id, component->table.desc.name);
	
	DriftTableClearRow(&component->table, idx);
	DriftComponentGetEntities(component)[idx] = entity;
	
	return idx;
}

void DriftComponentRemove(DriftComponent* component, DriftEntity entity){
	uint dst_idx = DriftComponentFind(component, entity);
	if(dst_idx){
		if(component->cleanup) component->cleanup(component, dst_idx);
		uint src_idx = component->table.row_count = component->count--;
		
		// Update before removing in case src == dst
		DriftMapInsert(&component->map, DriftComponentGetEntities(component)[src_idx].id, dst_idx);
		DriftMapRemove(&component->map, entity.id);
		DriftTableCopyRow(&component->table, dst_idx, src_idx);
	}
}

void DriftComponentGC(DriftComponent* component, DriftEntitySet* entities, uint pressure){
	// Keep probing for dead components until there are none left
	// or 'pressure' valid components are found in a row are encountered.
	for(uint run = 0; component->count > 0 && run < pressure;){
		// Wrap around.
		if(component->gc_cursor >= component->table.row_count) component->gc_cursor = 1;
		
		DriftEntity e = DriftComponentGetEntities(component)[component->gc_cursor];
		if(DriftEntitySetCheck(entities, e)){
			run++;
			component->gc_cursor++;
		} else {
			run = 0;
			DriftComponentRemove(component, e);
			// DRIFT_LOG("GC eid: %u:%u from %s", component->gc_cursor, e.id, component->table.desc.name);
		}
	}
}

static int DriftComponentCountCompare(const void *a, const void *b){
	const DriftComponentJoin* join0 = a;
	const DriftComponentJoin* join1 = b;
	return join0->component->count - join1->component->count;
}

DriftJoin DriftJoinMake(DriftComponentJoin* joins){
	DriftJoin join = {};
	for(uint i = 0; joins[i].variable; i++){
		*joins[i].variable = 0;
		join.joins[i] = joins[i];
		join.count++;
	}
	
	// Sort by component count to minimize the number of joins.
	// TODO can't sort with optional joins...
	// qsort(join.joins, join.count, sizeof(DriftComponentJoin), DriftComponentCountCompare);
	return join;
}

bool DriftJoinNext(DriftJoin* join){
	// Iterate entities from joins[0].
	DriftComponentJoin* joins = join->joins;
	next_entity: if(++*joins[0].variable <= joins[0].component->count){
		join->entity = DriftComponentGetEntities(joins[0].component)[*joins[0].variable];
		for(uint i = 1; i < join->count; i++){
			uint idx = DriftComponentFind(joins[i].component, join->entity);
			// If the component was found, update the join. Otherwise try the next entity.
			if(idx || joins[i].optional) *joins[i].variable = idx; else goto next_entity;
		}
		return true;
	}
	
	// No more matches. Terminate the loop.
	return false;
}

#if DRIFT_DEBUG
typedef struct {
	DriftComponent c;
	DriftEntity* entity;
} EmptyComponent;

typedef struct {
	DriftComponent c;
	DriftEntity* entity;
	uint* values;
	uint* values_copied;
} ValueComponent;

void unit_test_component(void){
	uint n = 256 << 10;
	
	static DriftEntitySet entities = {};
	DriftEntitySetInit(&entities);
	
	EmptyComponent empty = {};
	ValueComponent values = {};
	
	DriftComponentInit(&empty.c, (DriftTableDesc){
		.name = "Empty Component", .mem = DriftSystemMem, .min_row_capacity = n,
		.columns.arr = {DRIFT_DEFINE_COLUMN(empty.entity)},
	});
	
	DriftComponentInit(&values.c, (DriftTableDesc){
		.name = "Value Component", .mem = DriftSystemMem, .min_row_capacity = n,
		.columns.arr = {
			DRIFT_DEFINE_COLUMN(values.entity),
			DRIFT_DEFINE_COLUMN(values.values),
			DRIFT_DEFINE_COLUMN(values.values_copied),
		},
	});
	
	for(uint i = 0; i < n; i++){
		DriftEntity e = DriftEntitySetAquire(&entities, 0);
		
		if(i % 2 == 0){
			DriftComponentAdd(&empty.c, e);
		}
		
		if(i % 3 == 0){
			uint idx = DriftComponentAdd(&values.c, e);
			values.values[idx] = i + 0;
		}
	}
	
	// // Removing the zero entity should do nothing.
	// DriftComponentRemove(&values.c, (DriftEntity){.id = 0});
	// DRIFT_ASSERT(values.c.count == n, "Invalid count.");
	
	uint expected = 0;
	for(uint i = 0; i < n; i += 6) expected += i;
	
	uint empty_idx = -1, value_idx = -1;
	DriftJoin join = DriftJoinMake((DriftComponentJoin[]){
		{&empty_idx, &empty.c},
		{&value_idx, &values.c},
		{},
	});
	
	uint sum = 0;
	while(DriftJoinNext(&join)){
		uint value = values.values[value_idx];
		sum += value;
		values.values_copied[value_idx] = value;
	}
	DRIFT_ASSERT(sum == expected, "Invalid sum.");
	
	DRIFT_LOG("Component tests passed.");
}
#endif
