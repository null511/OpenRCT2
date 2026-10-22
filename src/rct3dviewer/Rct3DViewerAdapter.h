/*****************************************************************************
 * Copyright (c) 2026 OpenRCT2 developers
 *
 * This file is part of OpenRCT2.
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include <stdint.h>

#if defined(_WIN32)
	#define RCT3D_OPENRCT2_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
	#define RCT3D_OPENRCT2_EXPORT __attribute__((visibility("default")))
#else
	#define RCT3D_OPENRCT2_EXPORT
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define RCT3D_OPENRCT2_ABI_VERSION 15u

typedef struct Rct3DOpenRCT2Context Rct3DOpenRCT2Context;

typedef struct Rct3DOpenRCT2SurfaceData
{
	uint32_t terrain_surface_id_hash;
	uint8_t base_z;
	uint8_t slope_flags;
	uint8_t north_corner_height;
	uint8_t east_corner_height;
	uint8_t south_corner_height;
	uint8_t west_corner_height;
	uint16_t water_height;
	uint8_t grass_state;
	uint8_t can_grow_grass;
	uint8_t reserved[2];
} Rct3DOpenRCT2SurfaceData;

#pragma pack(push, 1)
typedef struct Rct3DOpenRCT2PathData
{
	int32_t tile_x;
	int32_t tile_y;
	uint32_t surface_id_hash;
	uint32_t railings_id_hash;
	uint8_t base_z;
	uint8_t slope_direction;
	uint8_t edges;
	uint8_t corners;
	uint8_t flags;
	uint8_t railings_support_type;
	uint8_t support_orientation;
	uint32_t addition_id_hash;
	uint8_t addition_flags;
} Rct3DOpenRCT2PathData;
#pragma pack(pop)

typedef struct Rct3DOpenRCT2VehicleData
{
	uint16_t entity_id;
	int32_t x;
	int32_t y;
	int32_t z;
	uint8_t orientation;
	uint8_t pitch;
	uint8_t roll;
	uint8_t reserved;
	float pitch_radians;
	float roll_radians;
} Rct3DOpenRCT2VehicleData;

#pragma pack(push, 1)
typedef struct Rct3DOpenRCT2TrackData
{
	int32_t tile_x;
	int32_t tile_y;
	uint16_t track_type;
	uint8_t sequence;
	uint8_t direction;
	uint8_t base_height;
	uint8_t main_palette_index;
	uint8_t additional_palette_index;
	uint8_t sequence_count;
	int8_t sequence_offset_x;
	int8_t sequence_offset_y;
	int8_t sequence_offset_z;
	int8_t rotation_begin;
	int8_t rotation_end;
	uint16_t ride_id;
} Rct3DOpenRCT2TrackData;

typedef struct Rct3DOpenRCT2PaletteEntry
{
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t alpha;
} Rct3DOpenRCT2PaletteEntry;
#pragma pack(pop)

#define RCT3D_OPENRCT2_PATH_FLAG_SLOPED (1u << 0)
#define RCT3D_OPENRCT2_PATH_FLAG_QUEUE (1u << 1)
#define RCT3D_OPENRCT2_PATH_ADDITION_FLAG_LAMP (1u << 0)
#define RCT3D_OPENRCT2_PATH_ADDITION_FLAG_BROKEN (1u << 1)
#define RCT3D_OPENRCT2_PATH_ADDITION_FLAG_BENCH (1u << 2)

typedef enum Rct3DOpenRCT2Status
{
	RCT3D_OPENRCT2_OK = 0,
	RCT3D_OPENRCT2_INVALID_ARGUMENT = 1,
	RCT3D_OPENRCT2_INVALID_STATE = 2,
	RCT3D_OPENRCT2_INITIALIZATION_FAILED = 3,
	RCT3D_OPENRCT2_PARK_LOAD_FAILED = 4,
	RCT3D_OPENRCT2_BUFFER_TOO_SMALL = 5,
	RCT3D_OPENRCT2_WRONG_THREAD = 6,
	RCT3D_OPENRCT2_INTERNAL_ERROR = 7
} Rct3DOpenRCT2Status;

RCT3D_OPENRCT2_EXPORT uint32_t rct3d_openrct2_get_abi_version(void);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_initialize(
	const char* openrct2_path_utf8, const char* rct2_path_utf8, const char* rct1_path_utf8,
	Rct3DOpenRCT2Context** out_context);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_load_park(
	Rct3DOpenRCT2Context* context, const char* park_path_utf8);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_get_map_dimensions(
	Rct3DOpenRCT2Context* context, int32_t* width, int32_t* height);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_copy_surface_data(
	Rct3DOpenRCT2Context* context, Rct3DOpenRCT2SurfaceData* output, uint64_t capacity);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_get_path_count(
	Rct3DOpenRCT2Context* context, uint64_t* count);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_copy_path_data(
	Rct3DOpenRCT2Context* context, Rct3DOpenRCT2PathData* output, uint64_t capacity);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_get_track_count(
	Rct3DOpenRCT2Context* context, uint64_t* count);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_copy_track_data(
	Rct3DOpenRCT2Context* context, Rct3DOpenRCT2TrackData* output, uint64_t capacity);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_copy_palette_data(
	Rct3DOpenRCT2Context* context, Rct3DOpenRCT2PaletteEntry* output, uint64_t capacity);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_advance_simulation(Rct3DOpenRCT2Context* context);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_get_time_of_day_hours(
	Rct3DOpenRCT2Context* context, float* hours);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_get_vehicle_count(
	Rct3DOpenRCT2Context* context, uint64_t* count);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_copy_vehicle_data(
	Rct3DOpenRCT2Context* context, Rct3DOpenRCT2VehicleData* output, uint64_t capacity);
RCT3D_OPENRCT2_EXPORT const char* rct3d_openrct2_get_last_error(void);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_shutdown(Rct3DOpenRCT2Context* context);

#ifdef __cplusplus
}
#endif
