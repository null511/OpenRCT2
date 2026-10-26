/*****************************************************************************
 * Copyright (c) 2026 OpenRCT2 developers
 *
 * This file is part of OpenRCT2.
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "Rct3DViewerAdapter.h"

#include "../openrct2/Context.h"
#include "../openrct2/Date.h"
#include "../openrct2/Game.h"
#include "../openrct2/entity/Guest.h"
#include "../openrct2/entity/EntityList.h"
#include "../openrct2/GameState.h"
#include "../openrct2/management/Marketing.h"
#include "../openrct2/OpenRCT2.h"
#include "../openrct2/PlatformEnvironment.h"
#include "../openrct2/drawing/ColourMap.h"
#include "../openrct2/drawing/Drawing.Sprite.h"
#include "../openrct2/drawing/Palette.h"
#include "../openrct2/drawing/PaletteType.h"
#include "../openrct2/object/WaterEntry.h"
#include "../openrct2/object/Object.h"
#include "../openrct2/object/ObjectManager.h"
#include "../openrct2/object/FootpathObject.h"
#include "../openrct2/object/FootpathRailingsObject.h"
#include "../openrct2/object/FootpathSurfaceObject.h"
#include "../openrct2/object/PathAdditionEntry.h"
#include "../openrct2/object/TerrainEdgeObject.h"
#include "../openrct2/paint/tile_element/Paint.Path.h"
#include "../openrct2/object/TerrainSurfaceObject.h"
#include "../openrct2/ride/Vehicle.h"
#include "../openrct2/ride/VehicleGeometry.h"
#include "../openrct2/ride/Ride.h"
#include "../openrct2/ride/TrackData.h"
#include "../openrct2/ride/ted/TrackElementDescriptor.h"
#include "../openrct2/ride/RideColour.h"
#include "../openrct2/world/Footpath.h"
#include "../openrct2/world/Map.h"
#include "../openrct2/world/tile_element/Slope.h"
#include "../openrct2/world/tile_element/PathElement.h"
#include "../openrct2/world/tile_element/EntranceElement.h"
#include "../openrct2/world/tile_element/SurfaceElement.h"
#include "../openrct2/world/tile_element/TrackElement.h"
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

static_assert(sizeof(Rct3DOpenRCT2SurfaceData) == 20);
static_assert(sizeof(Rct3DOpenRCT2PathData) == 28);
static_assert(sizeof(Rct3DOpenRCT2VehicleData) == 36);
static_assert(sizeof(Rct3DOpenRCT2PeepData) == 24);
static_assert(sizeof(Rct3DOpenRCT2TrackData) == 30);
static_assert(sizeof(Rct3DOpenRCT2RideEntranceData) == 14);
static_assert(sizeof(Rct3DOpenRCT2PaletteEntry) == 4);

struct Rct3DOpenRCT2Context
{
    std::unique_ptr<OpenRCT2::IContext> openrct2;
    std::thread::id ownerThread;
    bool parkLoaded = false;
    bool previousHeadless = false;
    bool previousNoGraphics = false;
    bool processSettingsCaptured = false;
    bool paletteG1Loaded = false;
    bool paletteGxLoaded = false;
    uint16_t peepEntityId = UINT16_MAX;
};

namespace
{
    std::mutex gAdapterMutex;
    Rct3DOpenRCT2Context* gActiveContext = nullptr;
    thread_local std::string gLastError;

    class ScopedNoGraphicsOverride
    {
    public:
        ScopedNoGraphicsOverride()
            : _previousValue(gOpenRCT2NoGraphics)
        {
            gOpenRCT2NoGraphics = false;
        }

        ~ScopedNoGraphicsOverride()
        {
            gOpenRCT2NoGraphics = _previousValue;
        }

        ScopedNoGraphicsOverride(const ScopedNoGraphicsOverride&) = delete;
        ScopedNoGraphicsOverride& operator=(const ScopedNoGraphicsOverride&) = delete;

    private:
        bool _previousValue;
    };

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

    uint32_t HashTerrainEdgeIdentifier(std::string_view identifier)
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
        if (additionEntry != nullptr && additionEntry->flags.has(OpenRCT2::PathAdditionFlag::isBench))
        {
            additionFlags |= RCT3D_OPENRCT2_PATH_ADDITION_FLAG_BENCH;
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

    Rct3DOpenRCT2TrackData MakeTrackData(const OpenRCT2::TrackElement& track, int32_t tileX, int32_t tileY)
    {
        const auto* ride = OpenRCT2::GetRide(track.getRideIndex());
        if (ride == nullptr)
        {
            throw std::runtime_error("A track element refers to a missing ride.");
        }

        const auto colourScheme = track.getColourScheme();
        if (colourScheme >= kNumRideColourSchemes)
        {
            throw std::runtime_error("A track element has an invalid colour scheme.");
        }

        const auto& colours = ride->trackColours[colourScheme];
        const auto mainPaletteIndex = OpenRCT2::Drawing::getColourMapShade(colours.main, 6);
        const auto additionalPaletteIndex = OpenRCT2::Drawing::getColourMapShade(colours.additional, 6);
        const auto supportPaletteIndex = OpenRCT2::Drawing::getColourMapShade(colours.supports, 6);
        const auto& descriptor = OpenRCT2::TrackMetadata::GetTrackElementDescriptor(track.getTrackType());
        const auto getRollSign = [](OpenRCT2::TrackMetadata::TrackRoll roll) -> int8_t {
            switch (roll)
            {
                case OpenRCT2::TrackMetadata::TrackRoll::left:
                    return 1;
                case OpenRCT2::TrackMetadata::TrackRoll::right:
                    return -1;
                default:
                    return 0;
            }
        };
        const auto sequence = track.getSequenceIndex();
        if (sequence >= descriptor.sequenceData.numSequences)
        {
            throw std::runtime_error("A track element has an invalid sequence index.");
        }
        const auto& sequenceOrigin = descriptor.sequenceData.sequences[0].clearance;
        const auto& sequenceClearance = descriptor.sequenceData.sequences[sequence].clearance;
        const auto offsetX = (sequenceClearance.x - sequenceOrigin.x) / 32;
        const auto offsetY = (sequenceClearance.y - sequenceOrigin.y) / 32;
        const auto offsetZ = (sequenceClearance.z - sequenceOrigin.z) / 8;
        const auto splineZBegin = (descriptor.coordinates.zBegin - sequenceOrigin.z) / 8;
        const auto splineZEnd = (descriptor.coordinates.zEnd - sequenceOrigin.z) / 8;
        if ((sequenceClearance.x - sequenceOrigin.x) % 32 != 0 || (sequenceClearance.y - sequenceOrigin.y) % 32 != 0
            || (sequenceClearance.z - sequenceOrigin.z) % 8 != 0
            || (descriptor.coordinates.zBegin - sequenceOrigin.z) % 8 != 0
            || (descriptor.coordinates.zEnd - sequenceOrigin.z) % 8 != 0
            || offsetX < std::numeric_limits<int8_t>::min() || offsetX > std::numeric_limits<int8_t>::max()
            || offsetY < std::numeric_limits<int8_t>::min() || offsetY > std::numeric_limits<int8_t>::max()
            || offsetZ < std::numeric_limits<int8_t>::min() || offsetZ > std::numeric_limits<int8_t>::max()
            || splineZBegin < std::numeric_limits<int8_t>::min() || splineZBegin > std::numeric_limits<int8_t>::max()
            || splineZEnd < std::numeric_limits<int8_t>::min() || splineZEnd > std::numeric_limits<int8_t>::max())
        {
            throw std::runtime_error("OpenRCT2 returned an unsupported track sequence tile offset.");
        }
        return {
            tileX,
            tileY,
            static_cast<uint16_t>(track.getTrackType()),
            sequence,
            track.getDirection(),
            track.baseHeight,
            static_cast<uint8_t>(mainPaletteIndex),
            static_cast<uint8_t>(additionalPaletteIndex),
            static_cast<uint8_t>(supportPaletteIndex),
            descriptor.sequenceData.numSequences,
            static_cast<int8_t>(offsetX),
            static_cast<int8_t>(offsetY),
            static_cast<int8_t>(offsetZ),
            descriptor.coordinates.rotationBegin,
            descriptor.coordinates.rotationEnd,
            static_cast<int8_t>(splineZBegin),
            static_cast<int8_t>(splineZEnd),
            track.getRideIndex().ToUnderlying(),
            static_cast<uint16_t>(track.getRideType()),
            getRollSign(descriptor.definition.rollStart),
            getRollSign(descriptor.definition.rollEnd),
        };
    }

    template<typename TCallback>
    bool ForEachTrack(TCallback&& callback)
    {
        {
            ScopedNoGraphicsOverride graphicsDataAccess;
            OpenRCT2::Drawing::initColourMaps();
        }

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
                    const auto* track = element->asTrack();
                    // Maze tiles use the track element's mazeEntry storage, which overlaps
                    // the ride sequence field. That byte is maze data, not a sequence index.
                    if (track != nullptr && track->getTrackType() != OpenRCT2::TrackElemType::maze
                        && !callback(MakeTrackData(*track, x, y)))
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

    template<typename TCallback>
    bool ForEachRideEntrance(TCallback&& callback)
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
                    const auto* entrance = element->asEntrance();
                    if (entrance != nullptr && !entrance->isGhost()
                        && (entrance->getEntranceType() == OpenRCT2::EntranceType::rideEntrance
                            || entrance->getEntranceType() == OpenRCT2::EntranceType::rideExit))
                    {
                        const auto entranceType = entrance->getEntranceType() == OpenRCT2::EntranceType::rideEntrance ? 0u : 1u;
                        const Rct3DOpenRCT2RideEntranceData data{
                            x,
                            y,
                            static_cast<uint16_t>(entrance->getRideIndex().ToUnderlying()),
                            static_cast<uint8_t>(entrance->getStationIndex().ToUnderlying()),
                            static_cast<uint8_t>(entranceType),
                            entrance->baseHeight,
                            static_cast<uint8_t>(entrance->getDirection()),
                        };
                        if (!callback(data))
                        {
                            return false;
                        }
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
        const auto* trainHead = vehicle.GetHead();
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
            vehicle.ride.ToUnderlying(),
            trainHead == nullptr ? vehicle.id.ToUnderlying() : trainHead->id.ToUnderlying(),
            vehicle.current_station.ToUnderlying(),
            static_cast<uint8_t>(vehicle.status),
        };
    }

    OpenRCT2::Guest* GetPeep(Rct3DOpenRCT2Context* context, uint16_t entityId)
    {
        if (context->peepEntityId != entityId)
        {
            return nullptr;
        }
        return OpenRCT2::getGameState().entities.tryGetEntity<OpenRCT2::Guest>(EntityId::FromUnderlying(entityId));
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

        if (context->processSettingsCaptured)
        {
            gOpenRCT2NoGraphics = false;
        }
        context->openrct2.reset();
        if (context->paletteG1Loaded)
        {
            GfxUnloadG1();
            context->paletteG1Loaded = false;
        }
        if (context->paletteGxLoaded)
        {
            GfxUnloadG2PalettesFontsTracks();
            context->paletteGxLoaded = false;
        }
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
        const char* openrct2_path_utf8, const char* rct2_path_utf8, const char* rct1_path_utf8,
        Rct3DOpenRCT2Context** out_context)
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

            if (openrct2_path_utf8 == nullptr || rct2_path_utf8 == nullptr || rct1_path_utf8 == nullptr
                || openrct2_path_utf8[0] == '\0' || rct2_path_utf8[0] == '\0' || rct1_path_utf8[0] == '\0')
            {
                return SetError(
                    RCT3D_OPENRCT2_INVALID_ARGUMENT, "OpenRCT2, RCT2, and RCT1 data paths are all required.");
            }

            if (gActiveContext != nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "Only one OpenRCT2 context can be active in this process.");
            }

            if (!std::filesystem::is_directory(std::filesystem::u8path(openrct2_path_utf8)))
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The OpenRCT2 data directory does not exist.");
            }
            if (!std::filesystem::is_directory(std::filesystem::u8path(rct2_path_utf8)))
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The RCT2 data directory does not exist.");
            }
            if (!std::filesystem::is_directory(std::filesystem::u8path(rct1_path_utf8)))
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The RCT1 data directory does not exist.");
            }

            context = std::make_unique<Rct3DOpenRCT2Context>();
            context->ownerThread = std::this_thread::get_id();
            context->previousHeadless = gOpenRCT2Headless;
            context->previousNoGraphics = gOpenRCT2NoGraphics;
            context->processSettingsCaptured = true;

            gOpenRCT2Headless = true;
            gOpenRCT2NoGraphics = true;
            gCustomOpenRCT2DataPath = openrct2_path_utf8;
            gCustomRCT2DataPath = rct2_path_utf8;
            gCustomRCT1DataPath = rct1_path_utf8;

            context->openrct2 = OpenRCT2::CreateContext();
            context->openrct2->GetPlatformEnvironment().SetBasePath(OpenRCT2::DirBase::rct2, rct2_path_utf8);
            context->openrct2->GetPlatformEnvironment().SetBasePath(OpenRCT2::DirBase::rct1, rct1_path_utf8);
            context->openrct2->GetPlatformEnvironment().SetBasePath(OpenRCT2::DirBase::openrct2, openrct2_path_utf8);
            gActiveContext = context.get();

            if (!context->openrct2->Initialise())
            {
                ReleaseContextResources(context.get());
                return SetError(RCT3D_OPENRCT2_INITIALIZATION_FAILED, "OpenRCT2 context initialization failed.");
            }

            if (!GfxLoadG1(context->openrct2->GetPlatformEnvironment()))
            {
                ReleaseContextResources(context.get());
                return SetError(RCT3D_OPENRCT2_INITIALIZATION_FAILED, "OpenRCT2's base palette data could not be loaded.");
            }
            context->paletteG1Loaded = true;
            GfxLoadG2PalettesFontsTracks();
            context->paletteGxLoaded = true;

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

            bool parkLoaded = false;
            {
                // No-graphics mode skips object image/palette data while loading the park.
                // Keep the process headless, but temporarily allow that data to be read.
                ScopedNoGraphicsOverride graphicsDataAccess;
                parkLoaded = context->openrct2->LoadParkFromFile(park_path_utf8);
            }
            if (!parkLoaded)
            {
                context->parkLoaded = false;
                return SetError(RCT3D_OPENRCT2_PARK_LOAD_FAILED, "OpenRCT2 failed to load the park file.");
            }

            context->parkLoaded = true;
            context->peepEntityId = UINT16_MAX;
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
                    const auto* edgeObject = OpenRCT2::TerrainEdgeObject::GetById(surface->getEdgeObjectIndex());
                    const auto edgeIdHash = edgeObject == nullptr ? 0u : HashTerrainEdgeIdentifier(edgeObject->GetIdentifier());
                    const auto waterHeight = surface->getWaterHeight();
                    const auto grassState = static_cast<uint8_t>(surface->getGrassLength() & 0x7);

                    output[index] = {
                        surfaceIdHash,
                        edgeIdHash,
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

    int32_t rct3d_openrct2_get_track_count(Rct3DOpenRCT2Context* context, uint64_t* count)
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
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The track count output pointer is null.");
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            *count = 0;
            if (!ForEachTrack([&count](const Rct3DOpenRCT2TrackData&) {
                    ++*count;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while counting track.");
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
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Counting track raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_copy_track_data(
        Rct3DOpenRCT2Context* context, Rct3DOpenRCT2TrackData* output, uint64_t capacity)
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
            if (!ForEachTrack([&required](const Rct3DOpenRCT2TrackData&) {
                    ++required;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while counting track.");
            }
            if (capacity < required)
            {
                return SetError(RCT3D_OPENRCT2_BUFFER_TOO_SMALL, "The track data buffer is smaller than the loaded track count.");
            }
            if (required > 0 && output == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The track data output pointer is null.");
            }

            uint64_t index = 0;
            if (!ForEachTrack([&](const Rct3DOpenRCT2TrackData& track) {
                    output[index++] = track;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while copying track.");
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
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Copying track raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_get_ride_entrance_count(Rct3DOpenRCT2Context* context, uint64_t* count)
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
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The ride entrance count output pointer is null.");
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            *count = 0;
            if (!ForEachRideEntrance([&count](const Rct3DOpenRCT2RideEntranceData&) {
                    ++*count;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while counting ride entrances.");
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
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Counting ride entrances raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_copy_ride_entrance_data(
        Rct3DOpenRCT2Context* context, Rct3DOpenRCT2RideEntranceData* output, uint64_t capacity)
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
            if (!ForEachRideEntrance([&required](const Rct3DOpenRCT2RideEntranceData&) {
                    ++required;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while counting ride entrances.");
            }
            if (capacity < required)
            {
                return SetError(RCT3D_OPENRCT2_BUFFER_TOO_SMALL, "The ride entrance data buffer is smaller than the loaded entrance count.");
            }
            if (required > 0 && output == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The ride entrance data output pointer is null.");
            }

            uint64_t index = 0;
            if (!ForEachRideEntrance([&](const Rct3DOpenRCT2RideEntranceData& entrance) {
                    output[index++] = entrance;
                    return true;
                }))
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "A map tile has no tile elements while copying ride entrances.");
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
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Copying ride entrance data raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_copy_palette_data(
        Rct3DOpenRCT2Context* context, Rct3DOpenRCT2PaletteEntry* output, uint64_t capacity)
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
            if (capacity < 256)
            {
                return SetError(RCT3D_OPENRCT2_BUFFER_TOO_SMALL, "The palette buffer must contain 256 entries.");
            }
            if (output == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The palette output pointer is null.");
            }

            {
                ScopedNoGraphicsOverride graphicsDataAccess;
                OpenRCT2::Drawing::LoadPalette();
                const auto& palette = OpenRCT2::Drawing::gPalette;
                for (uint32_t index = 0; index < palette.size(); ++index)
                {
                    const auto& colour = palette[index];
                    output[index] = { colour.red, colour.green, colour.blue, 255 };
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
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Copying the OpenRCT2 palette raised an unknown error.");
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

            OpenRCT2::gameStateTick();
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

    int32_t rct3d_openrct2_get_pause_state(Rct3DOpenRCT2Context* context, int32_t* is_paused)
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
            if (is_paused == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The pause state output pointer is null.");
            }

            *is_paused = GameIsPaused() ? 1 : 0;
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Reading the pause state raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_toggle_pause(Rct3DOpenRCT2Context* context, int32_t* is_paused)
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
            if (is_paused == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The pause state output pointer is null.");
            }

            PauseToggle();
            *is_paused = GameIsPaused() ? 1 : 0;
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Toggling the pause state raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_get_time_of_day_hours(Rct3DOpenRCT2Context* context, float* hours)
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
            if (hours == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The time-of-day output pointer is null.");
            }
            if (!context->parkLoaded)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "No park is loaded.");
            }

            const auto& date = OpenRCT2::getGameState().date;
            const auto daysInMonth = OpenRCT2::Date::GetDaysInMonth(date.GetMonth());
            const auto ticksWithinDay = (static_cast<uint32_t>(date.monthTicks) * daysInMonth) & 0xFFFF;
            *hours = static_cast<float>(ticksWithinDay) * (24.0f / 65536.0f);
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Reading the simulated time of day raised an unknown error.");
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

    int32_t rct3d_openrct2_create_peep(
        Rct3DOpenRCT2Context* context, int32_t x, int32_t y, int32_t z, uint16_t* entity_id)
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
            if (entity_id == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The peep entity output pointer is null.");
            }
            if (context->peepEntityId != UINT16_MAX)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "A Peep Mode guest already exists.");
            }

            auto* peep = OpenRCT2::Guest::generate(CoordsXYZ{ x, y, z });
            if (peep == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "OpenRCT2 could not create a guest for Peep Mode.");
            }

            peep->outsideOfPark = false;
            peep->setState(OpenRCT2::PeepState::picked);
            peep->subState = 0;
            peep->moveTo(CoordsXYZ{ x, y, z });
            context->peepEntityId = peep->id.ToUnderlying();
            *entity_id = context->peepEntityId;
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Creating a Peep Mode guest raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_move_peep(
        Rct3DOpenRCT2Context* context, uint16_t entity_id, int32_t x, int32_t y, int32_t z, uint8_t direction)
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
            auto* peep = GetPeep(context, entity_id);
            if (peep == nullptr || peep->state != OpenRCT2::PeepState::picked)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "The Peep Mode guest is not available for walking.");
            }

            const auto& mapSize = OpenRCT2::getGameState().mapSize;
            if (x < 0 || y < 0 || x >= mapSize.x * kCoordsXYStep
                || y >= mapSize.y * kCoordsXYStep)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The Peep Mode guest position is outside the map.");
            }

            peep->moveTo(CoordsXYZ{ x, y, z });
            peep->orientation = static_cast<uint8_t>((direction & 0x3) << 3);
            peep->peepDirection = direction & 0x3;
            peep->updateSpriteBoundingBox();
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Moving the Peep Mode guest raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_enter_ride(
        Rct3DOpenRCT2Context* context, uint16_t entity_id, uint16_t ride_id, uint8_t station_index)
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
            auto* peep = GetPeep(context, entity_id);
            auto* ride = OpenRCT2::GetRide(RideId::FromUnderlying(ride_id));
            if (peep == nullptr || peep->state != OpenRCT2::PeepState::picked || ride == nullptr
                || ride->status != OpenRCT2::RideStatus::open || station_index >= ride->numStations)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "The guest or requested open ride station is unavailable.");
            }

            const auto stationId = StationIndex::FromUnderlying(station_index);
            auto& station = ride->getStation(stationId);
            if (station.entrance.isNull())
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "The requested ride station has no entrance.");
            }

            peep->currentRide = ride->id;
            peep->currentRideStation = stationId;
            peep->guestHeadingToRideId = ride->id;
            peep->giveItem(ShopItem::voucher);
            peep->voucherType = VOUCHER_TYPE_RIDE_FREE;
            peep->voucherRideId = ride->id;
            peep->daysInQueue = 0;
            peep->timeInQueue = 0;
            peep->rideSubState = OpenRCT2::PeepRideSubState::atQueueFront;
            peep->setState(OpenRCT2::PeepState::queuing);
            ride->queueInsertGuestAtFront(stationId, peep);
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Entering a ride queue raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_get_peep_data(
        Rct3DOpenRCT2Context* context, uint16_t entity_id, Rct3DOpenRCT2PeepData* output)
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
                return SetError(RCT3D_OPENRCT2_INVALID_ARGUMENT, "The peep data output pointer is null.");
            }
            auto* peep = GetPeep(context, entity_id);
            if (peep == nullptr)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "The Peep Mode guest no longer exists.");
            }

            *output = {
                peep->id.ToUnderlying(),
                peep->x,
                peep->y,
                peep->z,
                static_cast<uint8_t>(peep->state),
                peep->currentRide.ToUnderlying(),
                peep->currentRideStation.ToUnderlying(),
                peep->currentTrain,
                peep->currentCar,
                peep->currentSeat,
                [&peep]() {
                const auto* ride = OpenRCT2::GetRide(peep->currentRide);
                if (ride == nullptr || peep->currentTrain >= std::size(ride->vehicles))
                {
                    return EntityId::GetNull().ToUnderlying();
                }
                return ride->vehicles[peep->currentTrain].ToUnderlying();
            }(),
                0,
            };
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Reading Peep Mode guest data raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_take_control_peep(
        Rct3DOpenRCT2Context* context, uint16_t entity_id, int32_t x, int32_t y, int32_t z, uint8_t direction)
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
            auto* peep = GetPeep(context, entity_id);
            if (peep == nullptr || peep->state != OpenRCT2::PeepState::walking)
            {
                return SetError(RCT3D_OPENRCT2_INVALID_STATE, "The guest has not completed the ride exit.");
            }

            peep->setState(OpenRCT2::PeepState::picked);
            peep->subState = 0;
            peep->guestHeadingToRideId = RideId::GetNull();
            peep->resetPathfindGoal();
            peep->moveTo(CoordsXYZ{ x, y, z });
            peep->orientation = static_cast<uint8_t>((direction & 0x3) << 3);
            peep->peepDirection = direction & 0x3;
            peep->updateSpriteBoundingBox();
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Taking control of the Peep Mode guest raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_release_peep(Rct3DOpenRCT2Context* context, uint16_t entity_id)
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
            auto* peep = GetPeep(context, entity_id);
            if (peep == nullptr)
            {
                context->peepEntityId = UINT16_MAX;
                return RCT3D_OPENRCT2_OK;
            }

            if (peep->state == OpenRCT2::PeepState::picked)
            {
                peep->setState(OpenRCT2::PeepState::walking);
                peep->action = OpenRCT2::PeepActionType::walking;
                peep->subState = 0;
                peep->resetPathfindGoal();
            }
            context->peepEntityId = UINT16_MAX;
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Releasing the Peep Mode guest raised an unknown error.");
        }
    }

    int32_t rct3d_openrct2_remove_peep(Rct3DOpenRCT2Context* context, uint16_t entity_id)
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
            auto* peep = GetPeep(context, entity_id);
            if (peep != nullptr)
            {
                peep->remove();
            }
            context->peepEntityId = UINT16_MAX;
            return RCT3D_OPENRCT2_OK;
        }
        catch (const std::exception& error)
        {
            gLastError = error.what();
            return static_cast<int32_t>(RCT3D_OPENRCT2_INTERNAL_ERROR);
        }
        catch (...)
        {
            return SetError(RCT3D_OPENRCT2_INTERNAL_ERROR, "Removing the Peep Mode guest raised an unknown error.");
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
