#include "drift_game.h"

// Read a cell + and offset with wraparound applied.
// Wrapping seems to be faster than branching.
// Also transposes x/y since the jump flood passes happen in a separable manner.
static inline DriftVec3 read_cell(DriftVec3* cells, int i0, int o, uint len){
	int i1 = (i0 + o)&(len - 1);
	DriftVec3 c = cells[i1];
	return (DriftVec3){.x = c.y, .y = c.x - (i1 - i0), .z = c.z};
}

// Apply jump a flooding pass on a horizontal axis and transpose the result.
void DriftSDFFloodRow(DriftVec3* dst, uint dst_stride, DriftVec3* src, uint len, int r){
	for(uint i = 0; i < len; i++){
		// Read the cells and compute distances.
		DriftVec3 c0 = read_cell(src, i, -r, len), c1 = read_cell(src, i,  0, len), c2 = read_cell(src, i, +r, len);
		float d0 = DriftSDFValue(c0), d1 = DriftSDFValue(c1), d2 = DriftSDFValue(c2);
		// Sort and pick the closest one.
		dst[i*dst_stride] = (d1 < d0 && d1 < d2 ? c1 : (d0 < d2 ? c0 : c2));
	}
}
