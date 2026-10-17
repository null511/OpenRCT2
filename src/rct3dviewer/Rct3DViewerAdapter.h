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

#define RCT3D_OPENRCT2_ABI_VERSION 3u

typedef struct Rct3DOpenRCT2Context Rct3DOpenRCT2Context;

typedef struct Rct3DOpenRCT2SurfaceData
{
	uint8_t base_z;
	uint8_t slope_flags;
	uint16_t ground_type;
 uint8_t north_corner_height;
 uint8_t east_corner_height;
 uint8_t south_corner_height;
 uint8_t west_corner_height;
 uint8_t map_colour;
 uint8_t reserved;
} Rct3DOpenRCT2SurfaceData;

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
	const char* openrct2_path_utf8, const char* rct2_path_utf8, Rct3DOpenRCT2Context** out_context);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_load_park(
	Rct3DOpenRCT2Context* context, const char* park_path_utf8);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_get_map_dimensions(
	Rct3DOpenRCT2Context* context, int32_t* width, int32_t* height);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_copy_surface_data(
	Rct3DOpenRCT2Context* context, Rct3DOpenRCT2SurfaceData* output, uint64_t capacity);
RCT3D_OPENRCT2_EXPORT const char* rct3d_openrct2_get_last_error(void);
RCT3D_OPENRCT2_EXPORT int32_t rct3d_openrct2_shutdown(Rct3DOpenRCT2Context* context);

#ifdef __cplusplus
}
#endif
