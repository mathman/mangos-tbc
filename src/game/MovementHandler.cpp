/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "Corpse.h"
#include "Player.h"
#include "MapManager.h"
#include "Transports.h"
#include "BattleGround/BattleGround.h"
#include "WaypointMovementGenerator.h"
#include "MapPersistentStateMgr.h"
#include "ObjectMgr.h"

void WorldSession::HandleMoveWorldportAckOpcode(WorldPacket & /*recv_data*/)
{
    DEBUG_LOG("WORLD: got MSG_MOVE_WORLDPORT_ACK.");
    HandleMoveWorldportAckOpcode();
}

void WorldSession::HandleMoveWorldportAckOpcode()
{
    // ignore unexpected far teleports
    if (!GetPlayer()->IsBeingTeleportedFar())
        return;

    // get start teleport coordinates (will used later in fail case)
    WorldLocation old_loc = GetPlayer()->GetWorldLocation();

    // get the teleport destination
    WorldLocation& loc = GetPlayer()->GetTeleportDest();

    // possible errors in the coordinate validity check (only cheating case possible)
    if (!MapManager::IsValidMapCoord(loc.m_mapId, loc.m_positionX, loc.m_positionY, loc.m_positionZ, loc.m_orientation))
    {
        sLog.outError("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far to a not valid location "
                      "(map:%u, x:%f, y:%f, z:%f) We port him to his homebind instead..",
                      GetPlayer()->GetGuidStr().c_str(), loc.m_mapId, loc.m_positionX, loc.m_positionY, loc.m_positionZ);
        // stop teleportation else we would try this again and again in LogoutPlayer...
        GetPlayer()->SetSemaphoreTeleportFar(false);
        // and teleport the player to a valid place
        GetPlayer()->TeleportToHomebind();
        return;
    }

    // get the destination map entry, not the current one, this will fix homebind and reset greeting
    MapEntry const* mEntry = sMapStore.LookupEntry(loc.m_mapId);

    Map* map = NULL;

    // prevent crash at attempt landing to not existed battleground instance
    if (mEntry->IsBattleGroundOrArena())
    {
        if (GetPlayer()->GetBattleGroundId())
            map = sMapMgr.FindMap(loc.m_mapId, GetPlayer()->GetBattleGroundId());

        if (!map)
        {
            DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far to nonexisten battleground instance "
                       " (map:%u, x:%f, y:%f, z:%f) Trying to port him to his previous place..",
                       GetPlayer()->GetGuidStr().c_str(), loc.m_mapId, loc.m_positionX, loc.m_positionY, loc.m_positionZ);

            GetPlayer()->SetSemaphoreTeleportFar(false);

            // Teleport to previous place, if cannot be ported back TP to homebind place
            if (!GetPlayer()->TeleportTo(old_loc))
            {
                DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s cannot be ported to his previous place, teleporting him to his homebind place...",
                           GetPlayer()->GetGuidStr().c_str());
                GetPlayer()->TeleportToHomebind();
            }
            return;
        }
    }

    InstanceTemplate const* mInstance = ObjectMgr::GetInstanceTemplate(loc.m_mapId);

    // reset instance validity, except if going to an instance inside an instance
    if (GetPlayer()->m_InstanceValid == false && !mInstance)
        GetPlayer()->m_InstanceValid = true;

    GetPlayer()->SetSemaphoreTeleportFar(false);

    // relocate the player to the teleport destination
    if (!map)
        map = sMapMgr.CreateMap(loc.m_mapId, GetPlayer());

    GetPlayer()->SetMap(map);
    GetPlayer()->Relocate(loc.m_positionX, loc.m_positionY, loc.m_positionZ, loc.m_orientation);

    GetPlayer()->SendInitialPacketsBeforeAddToMap();
    // the CanEnter checks are done in TeleporTo but conditions may change
    // while the player is in transit, for example the map may get full
    if (!GetPlayer()->GetMap()->Add(GetPlayer()))
    {
        // if player wasn't added to map, reset his map pointer!
        GetPlayer()->ResetMap();

        DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far but couldn't be added to map "
                   " (map:%u, x:%f, y:%f, z:%f) Trying to port him to his previous place..",
                   GetPlayer()->GetGuidStr().c_str(), loc.m_mapId, loc.m_positionX, loc.m_positionY, loc.m_positionZ);

        // Teleport to previous place, if cannot be ported back TP to homebind place
        if (!GetPlayer()->TeleportTo(old_loc))
        {
            DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s cannot be ported to his previous place, teleporting him to his homebind place...",
                       GetPlayer()->GetGuidStr().c_str());
            GetPlayer()->TeleportToHomebind();
        }
        return;
    }

    // battleground state prepare (in case join to BG), at relogin/tele player not invited
    // only add to bg group and object, if the player was invited (else he entered through command)
    if (_player->InBattleGround())
    {
        // cleanup setting if outdated
        if (!mEntry->IsBattleGroundOrArena())
        {
            // We're not in BG
            _player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
            // reset destination bg team
            _player->SetBGTeam(TEAM_NONE);
        }
        // join to bg case
        else if (BattleGround* bg = _player->GetBattleGround())
        {
            if (_player->IsInvitedForBattleGroundInstance(_player->GetBattleGroundId()))
                bg->AddPlayer(_player);
        }
    }

    GetPlayer()->SendInitialPacketsAfterAddToMap();

    // flight fast teleport case
    if (GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
    {
        if (!_player->InBattleGround())
        {
            // short preparations to continue flight
            FlightPathMovementGenerator* flight = (FlightPathMovementGenerator*)(GetPlayer()->GetMotionMaster()->top());
            flight->Reset(*GetPlayer());
            return;
        }

        // battleground state prepare, stop flight
        GetPlayer()->GetMotionMaster()->MovementExpired();
        GetPlayer()->m_taxi.ClearTaxiDestinations();
    }

    if (mEntry->IsRaid() && mInstance)
    {
        if (time_t timeReset = sMapPersistentStateMgr.GetScheduler().GetResetTimeFor(mEntry->MapID))
        {
            uint32 timeleft = uint32(timeReset - time(NULL));
            GetPlayer()->SendInstanceResetWarning(mEntry->MapID, timeleft);
        }
    }

    // mount allow check
    if (!mEntry->IsMountAllowed())
        _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    // honorless target
    if (GetPlayer()->pvpInfo.inHostileArea)
        GetPlayer()->CastSpell(GetPlayer(), 2479, true);

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    // lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

void WorldSession::HandleMoveTeleportAckOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("MSG_MOVE_TELEPORT_ACK");

    ObjectGuid guid;

    recv_data >> guid;

    uint32 counter, time;
    recv_data >> counter >> time;
    DEBUG_LOG("Guid: %s", guid.GetString().c_str());
    DEBUG_LOG("Counter %u, time %u", counter, time / IN_MILLISECONDS);

    Unit* mover = _player->GetMover();
    Player* plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    if (!plMover || !plMover->IsBeingTeleportedNear())
        return;

    if (guid != plMover->GetObjectGuid())
        return;

    plMover->SetSemaphoreTeleportNear(false);

    uint32 old_zone = plMover->GetZoneId();

    WorldLocation const& dest = plMover->GetTeleportDest();

    plMover->SetPosition(dest.m_positionX, dest.m_positionY, dest.m_positionZ, dest.m_orientation, true);

    uint32 newzone, newarea;
    plMover->GetZoneAndAreaId(newzone, newarea);
    plMover->UpdateZone(newzone, newarea);

    // new zone
    if (old_zone != newzone)
    {
        // honorless target
        if (plMover->pvpInfo.inHostileArea)
            plMover->CastSpell(plMover, 2479, true);
    }

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    // lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

void WorldSession::HandleMovementOpcodes(WorldPacket& recv_data)
{
    Opcodes opcode = recv_data.GetOpcode();
    if (!sLog.HasLogFilter(LOG_FILTER_PLAYER_MOVES))
    {
        DEBUG_LOG("WORLD: Received opcode %s (%u, 0x%X)", LookupOpcodeName(opcode), opcode, opcode);
        recv_data.hexlike();
    }

    Unit* mover = _player->GetMover();
    Player* plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if (plMover && plMover->IsBeingTeleported())
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    /* extract packet */
    MovementInfo movementInfo;
    movementInfo.Read(recv_data);
    /*----------------*/

    if (!VerifyMovementInfo(movementInfo))
        return;

    // fall damage generation (ignore in flight case that can be triggered also at lags in moment teleportation to another map).
    if (opcode == MSG_MOVE_FALL_LAND && plMover && !plMover->IsTaxiFlying())
        plMover->HandleFall(movementInfo);

    /* process position-change */
    HandleMoverRelocation(movementInfo);

    if (plMover)
        plMover->UpdateFallInformationIfNeed(movementInfo, opcode);

    WorldPacket data(opcode, recv_data.size());
    data << mover->GetPackGUID();             // write guid
    movementInfo.Write(data);                               // write data
    mover->SendMessageToSetExcept(&data, _player);
}

void WorldSession::HandleForceSpeedChangeAckOpcodes(WorldPacket& recv_data)
{
    Opcodes opcode = recv_data.GetOpcode();
    DEBUG_LOG("WORLD: Received %s (%u, 0x%X) opcode", recv_data.GetOpcodeName(), opcode, opcode);

    /* extract packet */
    ObjectGuid guid;
    MovementInfo movementInfo;
    float  newspeed;

    recv_data >> guid;
    recv_data >> Unused<uint32>();                          // counter or moveEvent
    movementInfo.Read(recv_data);
    recv_data >> newspeed;

    // now can skip not our packet
    if (_player->GetObjectGuid() != guid)
        return;
    /*----------------*/

    // client ACK send one packet for mounted/run case and need skip all except last from its
    // in other cases anti-cheat check can be fail in false case
    UnitMoveType move_type;
    UnitMoveType force_move_type;

    static char const* move_type_name[MAX_MOVE_TYPE] = {  "Walk", "Run", "RunBack", "Swim", "SwimBack", "TurnRate", "Flight", "FlightBack" };

    switch (opcode)
    {
        case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:          move_type = MOVE_WALK;          force_move_type = MOVE_WALK;        break;
        case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:           move_type = MOVE_RUN;           force_move_type = MOVE_RUN;         break;
        case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:      move_type = MOVE_RUN_BACK;      force_move_type = MOVE_RUN_BACK;    break;
        case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:          move_type = MOVE_SWIM;          force_move_type = MOVE_SWIM;        break;
        case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:     move_type = MOVE_SWIM_BACK;     force_move_type = MOVE_SWIM_BACK;   break;
        case CMSG_FORCE_TURN_RATE_CHANGE_ACK:           move_type = MOVE_TURN_RATE;     force_move_type = MOVE_TURN_RATE;   break;
        case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:        move_type = MOVE_FLIGHT;        force_move_type = MOVE_FLIGHT;      break;
        case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:   move_type = MOVE_FLIGHT_BACK;   force_move_type = MOVE_FLIGHT_BACK; break;
        default:
            sLog.outError("WorldSession::HandleForceSpeedChangeAck: Unknown move type opcode: %u", opcode);
            return;
    }

    // skip all forced speed changes except last and unexpected
    // in run/mounted case used one ACK and it must be skipped.m_forced_speed_changes[MOVE_RUN} store both.
    if (_player->m_forced_speed_changes[force_move_type] > 0)
    {
        --_player->m_forced_speed_changes[force_move_type];
        if (_player->m_forced_speed_changes[force_move_type] > 0)
            return;
    }

    if (!_player->GetTransport() && fabs(_player->GetSpeed(move_type) - newspeed) > 0.01f)
    {
        if (_player->GetSpeed(move_type) > newspeed)        // must be greater - just correct
        {
            sLog.outError("%sSpeedChange player %s is NOT correct (must be %f instead %f), force set to correct value",
                          move_type_name[move_type], _player->GetName(), _player->GetSpeed(move_type), newspeed);
            _player->SetSpeedRate(move_type, _player->GetSpeedRate(move_type), true);
        }
        else                                                // must be lesser - cheating
        {
            BASIC_LOG("Player %s from account id %u kicked for incorrect speed (must be %f instead %f)",
                      _player->GetName(), _player->GetSession()->GetAccountId(), _player->GetSpeed(move_type), newspeed);
            _player->GetSession()->KickPlayer();
        }
    }
}

void WorldSession::HandleSetActiveMoverOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_SET_ACTIVE_MOVER");

    ObjectGuid guid;
    recv_data >> guid;

    if (_player->GetMover()->GetObjectGuid() != guid)
    {
        sLog.outError("HandleSetActiveMoverOpcode: incorrect mover guid: mover is %s and should be %s",
                      _player->GetMover()->GetGuidStr().c_str(), guid.GetString().c_str());
        return;
    }
}

void WorldSession::HandleMoveNotActiveMoverOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_MOVE_NOT_ACTIVE_MOVER");
    recv_data.hexlike();

    ObjectGuid old_mover_guid;
    MovementInfo mi;

    recv_data >> old_mover_guid;
    mi.Read(recv_data);

    if (_player->GetMover()->GetObjectGuid() == old_mover_guid)
    {
        sLog.outError("HandleMoveNotActiveMover: incorrect mover guid: mover is %s and should be %s instead of %s",
                      _player->GetMover()->GetGuidStr().c_str(),
                      _player->GetGuidStr().c_str(),
                      old_mover_guid.GetString().c_str());
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    _player->m_movementInfo = mi;
}

void WorldSession::HandleMountSpecialAnimOpcode(WorldPacket& /*recvdata*/)
{
    // DEBUG_LOG("WORLD: Received opcode CMSG_MOUNTSPECIAL_ANIM");

    WorldPacket data(SMSG_MOUNTSPECIAL_ANIM, 8);
    data << GetPlayer()->GetObjectGuid();

    GetPlayer()->SendMessageToSet(&data, false);
}

void WorldSession::HandleMoveKnockBackAck(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_MOVE_KNOCK_BACK_ACK");

    Unit* mover = _player->GetMover();
    Player* plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if (plMover && plMover->IsBeingTeleported())
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    ObjectGuid guid;
    MovementInfo movementInfo;

    recv_data >> guid;
    recv_data >> Unused<uint32>();                          // knockback packets counter
    movementInfo.Read(recv_data);

    if (!VerifyMovementInfo(movementInfo, guid))
        return;

    HandleMoverRelocation(movementInfo);

    WorldPacket data(MSG_MOVE_KNOCK_BACK, recv_data.size() + 15);
    data << mover->GetObjectGuid();
    _player->BuildMovementPacket(&data);
    data << movementInfo.GetJumpInfo().sinAngle;
    data << movementInfo.GetJumpInfo().cosAngle;
    data << movementInfo.GetJumpInfo().xyspeed;
    data << movementInfo.GetJumpInfo().velocity;
    mover->SendMessageToSetExcept(&data, _player);
}

void WorldSession::SendKnockBack(float angle, float horizontalSpeed, float verticalSpeed)
{
    float vsin = sin(angle);
    float vcos = cos(angle);

    WorldPacket data(SMSG_MOVE_KNOCK_BACK, 9 + 4 + 4 + 4 + 4 + 4);
    data << GetPlayer()->GetPackGUID();
    data << uint32(0);                                  // Sequence
    data << float(vcos);                                // x direction
    data << float(vsin);                                // y direction
    data << float(horizontalSpeed);                     // Horizontal speed
    data << float(-verticalSpeed);                      // Z Movement speed (vertical)
    SendPacket(&data);
}

void WorldSession::HandleMoveHoverAck(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_MOVE_HOVER_ACK");

    MovementInfo movementInfo;

    recv_data >> Unused<uint64>();                          // guid
    recv_data >> Unused<uint32>();                          // unk
    movementInfo.Read(recv_data);
    recv_data >> Unused<uint32>();                          // unk2
}

void WorldSession::HandleMoveWaterWalkAck(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_MOVE_WATER_WALK_ACK");

    MovementInfo movementInfo;

    recv_data.read_skip<uint64>();                          // guid
    recv_data.read_skip<uint32>();                          // unk
    movementInfo.Read(recv_data);
    recv_data >> Unused<uint32>();                          // unk2
}

void WorldSession::HandleSummonResponseOpcode(WorldPacket& recv_data)
{
    if (!_player->isAlive() || _player->isInCombat())
        return;

    ObjectGuid summonerGuid;
    bool agree;
    recv_data >> summonerGuid;
    recv_data >> agree;

    _player->SummonIfPossible(agree);
}

bool WorldSession::VerifyMovementInfo(MovementInfo const& movementInfo, ObjectGuid const& guid) const
{
    // ignore wrong guid (player attempt cheating own session for not own guid possible...)
    if (guid != _player->GetMover()->GetObjectGuid())
        return false;

    return VerifyMovementInfo(movementInfo);
}

bool WorldSession::VerifyMovementInfo(MovementInfo const& movementInfo) const
{
    if (!MaNGOS::IsValidMapCoord(movementInfo.GetPos()->m_positionX, movementInfo.GetPos()->m_positionY, movementInfo.GetPos()->m_positionZ, movementInfo.GetPos()->m_orientation))
        return false;

    if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
    {
        // transports size limited
        // (also received at zeppelin/lift leave by some reason with t_* as absolute in continent coordinates, can be safely skipped)
        if (movementInfo.GetTransportPos()->m_positionX > 50 || movementInfo.GetTransportPos()->m_positionY > 50 || movementInfo.GetTransportPos()->m_positionZ > 100)
            return false;

        if (!MaNGOS::IsValidMapCoord(movementInfo.GetPos()->m_positionX + movementInfo.GetTransportPos()->m_positionX, movementInfo.GetPos()->m_positionY + movementInfo.GetTransportPos()->m_positionY,
            movementInfo.GetPos()->m_positionZ + movementInfo.GetTransportPos()->m_positionZ, movementInfo.GetPos()->m_orientation + movementInfo.GetTransportPos()->m_orientation))
        {
            return false;
        }
    }

    return true;
}

void WorldSession::HandleMoverRelocation(MovementInfo& movementInfo)
{
    movementInfo.UpdateTime(WorldTimer::getMSTime());

    Unit* mover = _player->GetMover();

    if (Player* plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL)
    {
        if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
        {
            if (!plMover->m_transport)
            {
                // elevators also cause the client to send MOVEFLAG_ONTRANSPORT - just unmount if the guid can be found in the transport list
                for (MapManager::TransportSet::const_iterator iter = sMapMgr.m_Transports.begin(); iter != sMapMgr.m_Transports.end(); ++iter)
                {
                    if ((*iter)->GetObjectGuid() == movementInfo.GetTransportGuid())
                    {
                        plMover->m_transport = (*iter);
                        (*iter)->AddPassenger(plMover);
                        break;
                    }
                }
            }
        }
        else if (plMover->m_transport)               // if we were on a transport, leave
        {
            plMover->m_transport->RemovePassenger(plMover);
            plMover->m_transport = NULL;
            movementInfo.ClearTransportData();
        }

        if (movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) != plMover->IsInWater())
        {
            // now client not include swimming flag in case jumping under water
            plMover->SetInWater(!plMover->IsInWater() || plMover->GetTerrain()->IsUnderWater(movementInfo.GetPos()->m_positionX, movementInfo.GetPos()->m_positionY, movementInfo.GetPos()->m_positionZ));
        }

        plMover->SetPosition(movementInfo.GetPos()->m_positionX, movementInfo.GetPos()->m_positionY, movementInfo.GetPos()->m_positionZ, movementInfo.GetPos()->m_orientation);
        plMover->m_movementInfo = movementInfo;

        if (movementInfo.GetPos()->m_positionZ < -500.0f)
        {
            if (plMover->GetBattleGround()
                    && plMover->GetBattleGround()->HandlePlayerUnderMap(_player))
            {
                // do nothing, the handle already did if returned true
            }
            else
            {
                // NOTE: this is actually called many times while falling
                // even after the player has been teleported away
                // TODO: discard movement packets after the player is rooted
                if (plMover->isAlive())
                {
                    plMover->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, plMover->GetMaxHealth());
                    // pl can be alive if GM/etc
                    if (!plMover->isAlive())
                    {
                        // change the death state to CORPSE to prevent the death timer from
                        // starting in the next player update
                        plMover->KillPlayer();
                        plMover->BuildPlayerRepop();
                    }
                }

                // cancel the death timer here if started
                plMover->RepopAtGraveyard();
            }
        }
    }
    else                                                    // creature charmed
    {
        if (mover->IsInWorld())
            mover->GetMap()->CreatureRelocation((Creature*)mover, movementInfo.GetPos()->m_positionX, movementInfo.GetPos()->m_positionY, movementInfo.GetPos()->m_positionZ, movementInfo.GetPos()->m_orientation);
    }
}
