/*****************************************************************************
 * Copyright (c) 2026 OpenRCT2 developers
 *
 * This file is part of OpenRCT2.
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "Rct3DViewerAdapter.h"

#include "../openrct2/Context.h"
#include "../openrct2/entity/EntityList.h"
#include "../openrct2/GameState.h"
#include "../openrct2/OpenRCT2.h"
#include "../openrct2/PlatformEnvironment.h"
#include "../openrct2/object/Object.h"
#include "../openrct2/object/ObjectManager.h"
#include "../openrct2/object/FootpathObject.h"
#include "../openrct2/object/FootpathRailingsObject.h"
#include "../openrct2/object/FootpathSurfaceObject.h"
#include "../openrct2/object/PathAdditionEntry.h"
#include "../openrct2/paint/tile_element/Paint.Path.h"
#include "../openrct2/object/TerrainSurfaceObject.h"
#include "../openrct2/ride/Vehicle.h"
#include "../openrct2/ride/VehicleGeometry.h"
#include "../openrct2/world/Footpath.h"
#include "../openrct2/world/Map.h"
#include "../openrct2/world/tile_element/Slope.h"
#include "../openrct2/world/tile_element/PathElement.h"
#include "../openrct2/world/tile_element/SurfaceElement.h"
#include "../openrct2/world/tile_element/TileElement.h"

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

static_assert(sizeof(Rct3DOpenRCT2SurfaceData) == 16);
static_assert(sizeof(Rct3DOpenRCT2PathData) == 28);
static_assert(sizeof(Rct3DOpenRCT2VehicleData) == 28);

struct Rct3DOpenRCT2Context
{
    std::unique_ptr<OpenRCT2::IContext> openrct2;
    std::thread::id ownerThread;
    bool parkLoaded = false;
    bool previousHeadless = false;
    bool previousNoGraphics = false;
    bool processSettingsCaptured = false;
    std::string previousOpenRCT2DataPath;
    std::string previousRCT2DataPath;
};

namespace
{
    std::mutex gAdapterMutex;
    Rct3DOpenRCT2Context* gActiveContext = nullptr;
    thread_local std::string gLastError;

    uint32_t HashTerrainSurfaceIdentifier(std::string_view identifier)
    {
        uint32_t hash = 2166136261u;
        for (const auto character : identifier)
        {
            hash ^= static_cast<uint8_t>(character);
            hash *= 16777619u;
        }
        return hash;
    }

    uint32_t HashPathObjectIdentifier(std::string_view identifier)
    {
        uint32_t hash = 2166136261u;
        for (const auto character : identifier)
        {
            hash ^= static_cast<uint8_t>(character);
            hash *= 16777619u;
        }
        return hash;
    }

    Rct3DOpenRCT2PathData MakePathData(const OpenRCT2::PathElement& path, int32_t tileX, int32_t tileY)
    {
        const auto* surfaceObject = path.hasLegacyPathEntry()
            ? static_cast<const OpenRCT2::Object*>(path.getLegacyPathEntry())
            : static_cast<const OpenRCT2::Object*>(path.getSurfaceEntry());
        const auto* railingsObject = path.hasLegacyPathEntry()
            ? static_cast<const OpenRCT2::Object*>(path.getLegacyPathEntry())
            : static_cast<const OpenRCT2::Object*>(path.getRailingsEntry());
        const auto* railingsDescriptor = path.getRailingsDescriptor();
        const auto* additionEntry = path.getAdditionEntry();
        const auto* additionObject = additionEntry == nullptr
            ? nullptr
            : OpenRCT2::GetContext()->GetObjectManager().GetLoadedObject(
                  OpenRCT2::ObjectType::pathAdditions, path.getAdditionEntryIndex());

        uint8_t flags = 0;
        if (path.isSloped())
        {
            flags |= RCT3D_OPENRCT2_PATH_FLAG_SLOPED;
        }
        if (path.isQueue())
        {
            flags |= RCT3D_OPENRCT2_PATH_FLAG_QUEUE;
        }

        uint8_t additionFlags = 0;
        if (additionEntry != nullptr && additionEntry->flags.has(OpenRCT2::PathAdditionFlag::lamp))
        {
            additionFlags |= RCT3D_OPENRCT2_PATH_ADDITION_FLAG_LAMP;
        }
        if (path.isBroken())
        {
            additionFlags |= RCT3D_OPENRCT2_PATH_ADDITION_FLAG_BROKEN;
        }

        return {
            tileX,
            tileY,
            surfaceObject == nullptr ? 0u : HashPathObjectIdentifier(surfaceObject->GetIdentifier()),
            railingsObject == nullptr ? 0u : HashPathObjectIdentifier(railingsObject->GetIdentifier()),
            path.baseHeight,
            static_cast<uint8_t>(path.getSlopeDirection()),
            path.getEdges(),
            path.getCorners(),
            flags,
            railingsDescriptor == nullptr ? static_cast<uint8_t>(OpenRCT2::RailingEntrySupportType::box)
                                           : static_cast<uint8_t>(railingsDescriptor->supportType),
            static_cast<uint8_t>(OpenRCT2::GetPathSupportOrientation(path.getEdges())),
            additionObject == nullptr ? 0u : HashPathObjectIdentifier(additionObject->GetIdentifier()),
            additionFlags,
        };
    }

    template<typename TCallback>
    bool ForEachPath(TCallback&& callback)
    {
        const auto& mapSize = OpenRCT2::getGameState().mapSize;
        for (int32_t y = 0; y < mapSize.y; ++y)
        {
            for (int32_t x = 0; x < mapSize.x; ++x)
            {
                const TileCoordsXY tilePosition(x, y);
                auto* element = OpenRCT2::MapGetFirstElementAt(tilePosition);
                if (element == nullptr)
                {
                    return false;
                }

                while (true)
                {
                    const bool lastForTile = element->isLastForTile();
                    const auto* path = element->asPath();
                    if (path != nullptr && !callback(MakePathData(*path, x, y)))
                    {
                        return false;
                    }
                    if (lastForTile)
                    {
                        break;
                    }
                    ++element;
                }
            }
        }
        return true;
    }

    bool VehicleHasTrackPitchAndRoll(const OpenRCT2::Vehicle& vehicle)
    {
        switch (vehicle.status)
        {
            case OpenRCT2::Vehicle::Status::travellingDodgems:
            case OpenRCT2::Vehicle::Status::swinging:
            case OpenRCT2::Vehicle::Status::rotating:
            case OpenRCT2::Vehicle::Status::ferrisWheelRotating:
            case OpenRCT2::Vehicle::Status::simulatorOperating:
            case OpenRCT2::Vehicle::Status::showingFilm:
            case OpenRCT2::Vehicle::Status::spaceRingsOperating:
            case OpenRCT2::Vehicle::Status::topSpinOperating:
            case OpenRCT2::Vehicle::Status::hauntedHouseOperating:
            case OpenRCT2::Vehicle::Status::doingCircusShow:
            case OpenRCT2::Vehicle::Status::crookedHouseOperating:
            case OpenRCT2::Vehicle::Status::operating1A:
            case OpenRCT2::Vehicle::Status::stopping1B:
            case OpenRCT2::Vehicle::Status::unloadingPassengers1C:
                return false;
            default:
                return true;
        }
    }

    float GetVehiclePitchRadians(const OpenRCT2::Vehicle& vehicle)
    {
        if (!VehicleHasTrackPitchAndRoll(vehicle) || vehicle.pitch >= VehiclePitch::pitchCount)
        {
            return 0.0f;
        }

        const auto pitchVector = OpenRCT2::RideVehicle::Geometry::getPitchVector32(vehicle.pitch);
        return static_cast<float>(std::atan2(static_cast<double>(pitchVector.y), static_cast<double>(pitchVector.x)));
    }

    float GetVehicleRollRadians(const OpenRCT2::Vehicle& vehicle)
    {
        if (!VehicleHasTrackPitchAndRoll(vehicle) || vehicle.roll >= VehicleRoll::rollCount)
        {
            return 0.0f;
        }

        double sign = 1.0;
        switch (vehicle.roll)
        {
            case VehicleRoll::right22:
            case VehicleRoll::right45:
            case VehicleRoll::right67:
            case VehicleRoll::right90:
            case VehicleRoll::right112:
            case VehicleRoll::right135:
            case VehicleRoll::right157:
            case VehicleRoll::uninvertingRight22:
            case VehicleRoll::uninvertingRight45:
                sign = -1.0;
                break;
            default:
                break;
        }

        const auto horizontalComponent = OpenRCT2::RideVehicle::Geometry::getRollHorizontalComponent(vehicle.roll);
        const auto cosine = static_cast<double>(horizontalComponent) / std::numeric_limits<int32_t>::max();
        return static_cast<float>(sign * std::acos(std::clamp(cosine, -1.0, 1.0)));
    }

    Rct3DOpenRCT2VehicleData MakeVehicleData(const OpenRCT2::Vehicle& vehicle)
    {
        return {
            vehicle.id.ToUnderlying(),
            vehicle.x,
            vehicle.y,
            vehicle.z,
            vehicle.orientation,
            static_cast<uint8_t>(vehicle.pitch),
            static_cast<uint8_t>(vehicle.roll),
            0,
            GetVehiclePitchRadians(vehicle),
            GetVehicleRollRadians(vehicle),
        };
    }

    template<typename TCallback>
    void ForEachPositionedVehicle(TCallback&& callback)
    {
        for (auto* vehicle : OpenRCT2::EntityList<OpenRCT2::Vehicle>())
        {
            if (vehicle->x == kLocationNull || vehicle->y == kLocationNull || vehicle->z == kLocationNull)
            {
                continue;
            }
            callback(MakeVehicleData(*vehicle));
        }
    }

    int32_t SetError(Rct3DOpenRCT2Status status, const char* message)
    {
        gLastError = message;
        return static_cast<int32_t>(status);
    }

    void RestoreProcessSettings(const Rct3DOpenRCT2Context& context)
    {
        gOpenRCT2Headless = context.previousHeadless;
        gOpenRCT2NoGraphics = context.previousNoGraphics;
        gCustomOpenRCT2DataPath = context.previousOpenRCT2DataPath;
        gCustomRCT2DataPath = context.previousRCT2DataPath;
    }

    int32_t ValidateContext(Rct3DOpenRCT2Context* context)
    {
        if (context == nullptr || context != gActiveContext || context->openrct2 == nullptr)
        {
            return SetError(RCT3D_OPENRCT2_INVALID_STATE, "OpenRCT2 context is not initialized.");
        }

        if (context->ownerThread != std::this_thread::get_id())
        {
            return SetError(RCT3D_OPENRCT2_WRONG_THREAD, "OpenRCT2 context calls must use its initialization thread.");
        }

        return RCT3D_OPENRCT2_OK;
    }

    void ReleaseContextResources(Rct3DOpenRCT2Context* context)
    {
        if (context == nullptr)
        {
            return;
        }

        context->openrct2.reset();
        if (context->processSettingsCaptured)
        {
            RestoreProcessSettings(*context);
        }
        if (gActiveContext == context)
        {
            gActiveContext = nullptr;
        }
    }

    void DisposeContext(Rct3DOpenRCT2Context* context)
    {
        if (context == nullptr)
        {
            return;
        }

        ReleaseContextResources(context);
        delete context;
    }
}

extern "C"
{
    uint32_t rct3d_openrct2_get_abi_version(void)
    {
        return RCT3D_OPENRCT2_ABI_VERSION;
    }

    int32_t rct3d_openrct2_initialize(
        const char* openrct2_path_utf8, const char* rct2_path_utf8, Rct3DOpenRCT2Context** out_context)
    {
        std::unique_ptr<Rct3DOpenRCT2Context> context;
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            if (out_context == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The output context pointer is null.");
            }
            *out_context = nullptr;

            if (openrct2_path_utf8 == nullptr || rct2_path_utf8 == nullptr || openrct2_path_utf8[0] == '\0'
                || rct2_path_utf8[0] == '\0')
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "Both OpenRCT2 and RCT2 data paths are required.");
            }

            if (gActiveContext != nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "Only one OpenRCT2 context can be active in this process.");
            }

            if (!std::filesystem::is_directory(std::filesystem::u8path(openrct2_path_utf8))
                || !std::filesystem::is_directory(std::filesystem::u8path(rct2_path_utf8)))
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "An OpenRCT2 or RCT2 data directory does not exist.");
            }

            context = std::make_unique<Rct3DOpenRCT2Context>();
            context->ownerThread = std::this_thread::get_id();
            context->previousHeadless = gOpenRCT2Headless;
            context->previousNoGraphics = gOpenRCT2NoGraphics;
            context->previousOpenRCT2DataPath = gCustomOpenRCT2DataPath;
            context->previousRCT2DataPath = gCustomRCT2DataPath;
            context->processSettingsCaptured = true;

            gOpenRCT2Headless = true;
            gOpenRCT2NoGraphics = true;
            gCustomOpenRCT2DataPath = openrct2_path_utf8;
            gCustomRCT2DataPath = rct2_path_utf8;

            context->openrct2 = OpenRCT2::CreateContext();
            context->openrct2->GetPlatformEnvironment().SetBasePath(OpenRCT2::DirBase::rct2, rct2_path_utf8);
            context->openrct2->GetPlatformEnvironment().SetBasePath(OpenRCT2::DirBase::openrct2, openrct2_path_utf8);
            gActiveContext = context.get();

            if (!context->openrct2->Initialise())
            {
                ReleaseContextResources(context.get());
                return SetError(RCT3D_OPENRCT2_INITIALIZATION_FAILED, "OpenRCT2 context initialization failed.");
            }

            *out_context = context.release();
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            if (context != nullptr)
            {
                ReleaseContextResources(context.get());
                context.reset();
            }
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INITIALIZATION_FAILED);
        }
        catch (...)
        {
            if (context != nullptr)
            {
                ReleaseContextResources(context.get());
                context.reset();
            }
            return SetError(RCT3D_OPENRCT2_INITIALIZATION_FAILED, "OpenRCT2 context initialization raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_load_park(Rct3DOpenRCT2Context* context, const char* park_path_utf8)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }
            if (park_path_utf8 == nullptr || park_path_utf8[0] == '\0')
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The park file path is empty.");
            }

            if (!context->openrct2->LoadParkFromFile(park_path_utf8))
            {
                context->parkLoaded = false;
                return SetError(RCT3D_OPENRCT2_PARK_LOAD_FAILED, "OpenRCT2 failed to load the park file.");
            }

            context->parkLoaded = true;
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_PARK_LOAD_FAILED);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_PARK_LOAD_FAILED, "OpenRCT2 park loading raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_get_map_dimensions(Rct3DOpenRCT2Context* context, int32_t* width, int32_t* height)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }
            if (width == nullptr || height == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "Map dimension output pointer is null.");
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            const auto& mapSize = OpenRCT2::getGameState().mapSize;
            *width = mapSize.x;
            *height = mapSize.y;
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Reading map dimensions raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_copy_surface_data(
        Rct3DOpenRCT2Context* context, Rct3DOpenRCT2SurfaceData* output, uint64_t capacity)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }
            if (output == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The surface data output pointer is null.");
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            const auto& mapSize = OpenRCT2::getGameState().mapSize;
            const uint64_t required = static_cast<uint64_t>(mapSize.x) * static_cast<uint64_t>(mapSize.y);
            if (mapSize.x <= 0 || mapSize.y <= 0 || capacity < required)
            {
                return SetError(RCT3D_OPENRCT2_BUFFER_TOO_SMALL, "The surface data buffer is smaller than the loaded map.");
            }

            for (int32_t y = 0; y < mapSize.y; ++y)
            {
                for (int32_t x = 0; x < mapSize.x; ++x)
                {
                    const auto* surface = OpenRCT2::MapGetSurfaceElementAt(TileCoordsXY{ x, y });
                    if (surface == nullptr)
                    {
                        return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A loaded map tile has no surface element.");
                    }

                    const auto index = static_cast<uint64_t>(y) * static_cast<uint64_t>(mapSize.x)
                        + static_cast<uint64_t>(x);
                    const auto cornerHeights = OpenRCT2::GetSlopeRelativeCornerHeights(surface->getSlope());
                    const auto* surfaceObject = OpenRCT2::TerrainSurfaceObject::GetById(surface->getSurfaceObjectIndex());
                    const auto surfaceIdHash = surfaceObject == nullptr ? 0u : HashTerrainSurfaceIdentifier(surfaceObject->GetIdentifier());
                    const auto waterHeight = surface->getWaterHeight();
                    const auto grassState = static_cast<uint8_t>(surface->getGrassLength() & 0x7);

                    output[index] = {
                        surfaceIdHash,
                        surface->baseHeight,
                        surface->getSlope(),
                        cornerHeights.bottom,
                        cornerHeights.left,
                        cornerHeights.top,
                        cornerHeights.right,
                        static_cast<uint16_t>(waterHeight > surface->getBaseZ() ? waterHeight : 0),
                        grassState,
                        static_cast<uint8_t>(surface->canGrassGrow()),
                        { 0, 0 },
                    };
                }
            }

            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Copying map surface data raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_get_path_count(Rct3DOpenRCT2Context* context, uint64_t* count)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }
            if (count == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The path count output pointer is null.");
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            *count = 0;
            if (!ForEachPath([&count](const Rct3DOpenRCT2PathData&) {
                    ++*count;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while counting paths.");
            }
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Counting map paths raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_copy_path_data(
        Rct3DOpenRCT2Context* context, Rct3DOpenRCT2PathData* output, uint64_t capacity)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            const auto& mapSize = OpenRCT2::getGameState().mapSize;
            if (mapSize.x <= 0 || mapSize.y <= 0)
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "The loaded map dimensions are invalid.");
            }

            uint64_t required = 0;
            if (!ForEachPath([&required](const Rct3DOpenRCT2PathData&) {
                    ++required;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while counting paths.");
            }
            if (capacity < required)
            {
                return SetError(RCT3D_OPENRCT2_BUFFER_TOO_SMALL, "The path data buffer is smaller than the loaded path count.");
            }
            if (required > 0 && output == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The path data output pointer is null.");
            }

            uint64_t index = 0;
            if (!ForEachPath([&](const Rct3DOpenRCT2PathData& path) {
                    output[index++] = path;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while copying paths.");
            }
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Copying map path data raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_advance_simulation(Rct3DOpenRCT2Context* context)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            OpenRCT2::gameStateUpdateLogic();
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Advancing the simulation raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_get_vehicle_count(Rct3DOpenRCT2Context* context, uint64_t* count)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }
            if (count == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The vehicle count output pointer is null.");
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            *count = 0;
            ForEachPositionedVehicle([&count](const Rct3DOpenRCT2VehicleData&) { ++*count; });
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Counting positioned ride vehicles raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_copy_vehicle_data(
        Rct3DOpenRCT2Context* context, Rct3DOpenRCT2VehicleData* output, uint64_t capacity)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            uint64_t required = 0;
            ForEachPositionedVehicle([&required](const Rct3DOpenRCT2VehicleData&) { ++required; });
            if (capacity < required)
            {
                return SetError(RCT3D_OPENRCT2_BUFFER_TOO_SMALL, "The vehicle data buffer is smaller than the positioned vehicle count.");
            }
            if (required > 0 && output == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The vehicle data output pointer is null.");
            }

            uint64_t index = 0;
            ForEachPositionedVehicle([&](const Rct3DOpenRCT2VehicleData& vehicle) { output[index++] = vehicle; });
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Copying positioned ride vehicle data raised an unknown error.");
        }
    }

    const char* rct3d_openrct2_get_last_error(void)
    {
        return gLastError.c_str();
    }

    int32_t rct3d_openrct2_shutdown(Rct3DOpenRCT2Context* context)
    {
        try
        {
            std::lock_guard<std::mutex> lock(gAdapterMutex);
            gLastError.clear();

            if (context == nullptr)
            {
                return RCT3D_OPENRCT2_OK;
            }

            const auto state = ValidateContext(context);
            if (state != RCT3D_OPENRCT2_OK)
            {
                return state;
            }

            DisposeContext(context);
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Shutting down OpenRCT2 raised an unknown error.");
        }
    }
}
