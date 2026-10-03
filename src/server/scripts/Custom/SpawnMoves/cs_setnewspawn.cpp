/*
 * .setnewspawn — move a spawn to where the GM stands, live, and record the move.
 *
 *   .setnewspawn          selected creature
 *   .setnewspawn go       nearest gameobject
 *   .setnewspawn undo     put the last recorded move back
 *   .setnewspawn list     show the last few moves
 *
 * The live move reuses the core's .npc move / .gobject move logic (world.dest is updated too, but
 * world.dest is rebuilt from TDB on `build all --rebuild`, so that part is temporary). The durable
 * record is one JSON line per move in SpawnMoves.File; modules/spawn_overrides/apply_moves.py turns
 * those lines into datascript edits.
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

using namespace Trinity::ChatCommands;

namespace
{
    std::string MovesFile()
    {
        return sConfigMgr->GetStringDefault("SpawnMoves.File",
            "/root/tswow-server/tswow-install/modules/spawn_overrides/moves.jsonl");
    }

    std::string JsonEscape(std::string const& s)
    {
        std::string out;
        for (char c : s)
        {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
        return out;
    }

    std::string Pos(Position const& p)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[%.3f,%.3f,%.3f,%.4f]", p.GetPositionX(), p.GetPositionY(), p.GetPositionZ(), p.GetOrientation());
        return buf;
    }

    struct Move
    {
        std::string kind;     // "creature" | "gameobject"
        uint32 spawn = 0;
        uint32 entry = 0;
        uint32 map = 0;
        Position from;
        Position to;
    };

    bool Record(ChatHandler* handler, Move const& m, std::string const& name, bool undo)
    {
        std::ofstream f(MovesFile(), std::ios::app);
        if (!f)
        {
            handler->PSendSysMessage("setnewspawn: moved live, but could not write %s. The move will be lost at the next rebuild.", MovesFile().c_str());
            return false;
        }
        f << "{\"t\":" << time(nullptr)
          << ",\"by\":\"" << JsonEscape(handler->GetSession()->GetPlayer()->GetName()) << "\""
          << ",\"kind\":\"" << m.kind << "\",\"spawn\":" << m.spawn << ",\"entry\":" << m.entry
          << ",\"name\":\"" << JsonEscape(name) << "\",\"map\":" << m.map
          << ",\"old\":" << Pos(m.from) << ",\"new\":" << Pos(m.to)
          << ",\"undo\":" << (undo ? "true" : "false") << "}\n";
        return true;
    }

    // Same steps as .npc move: grid + DB + respawn.
    bool MoveCreature(ChatHandler* handler, uint32 spawnId, Position const& to, Creature* live)
    {
        CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
        if (!data)
            return false;
        sObjectMgr->RemoveCreatureFromGrid(spawnId, data);
        const_cast<CreatureData*>(data)->spawnPoint.Relocate(to);
        sObjectMgr->AddCreatureToGrid(spawnId, data);

        WorldDatabasePreparedStatement* stmt = WorldDatabase.GetPreparedStatement(WORLD_UPD_CREATURE_POSITION);
        stmt->setFloat(0, to.GetPositionX());
        stmt->setFloat(1, to.GetPositionY());
        stmt->setFloat(2, to.GetPositionZ());
        stmt->setFloat(3, to.GetOrientation());
        stmt->setUInt32(4, spawnId);
        WorldDatabase.Execute(stmt);

        if (!live)
        {
            Map* map = handler->GetSession()->GetPlayer()->GetMap();
            auto bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
            if (bounds.first != bounds.second)
                live = bounds.first->second;
        }
        if (live)
            live->DespawnOrUnsummon(0s, 1s);
        return true;
    }

    // Same steps as .gobject move: relocate, save, delete and reload (the 3.3.5a client caches
    // deleted objects by guid, so a plain relocate would not show).
    bool MoveGameObject(ChatHandler* handler, GameObject* object, Position to)
    {
        uint32 spawnId = object->GetSpawnId();
        Map* map = object->GetMap();
        to.SetOrientation(object->GetOrientation());
        object->Relocate(to);
        sObjectMgr->RemoveGameobjectFromGrid(spawnId, object->GetGameObjectData());
        object->SaveToDB();
        sObjectMgr->AddGameobjectToGrid(spawnId, object->GetGameObjectData());
        object->Delete();
        GameObject* fresh = new GameObject();
        if (!fresh->LoadFromDB(spawnId, map, true))
        {
            delete fresh;
            handler->PSendSysMessage("setnewspawn: gameobject %u moved but failed to respawn; it will appear after a restart.", spawnId);
        }
        return true;
    }

    // Last `n` lines of the moves file (small file; read whole).
    std::vector<std::string> LastLines(size_t n)
    {
        std::ifstream f(MovesFile());
        std::vector<std::string> lines;
        for (std::string l; std::getline(f, l);)
            if (!l.empty())
                lines.push_back(l);
        if (lines.size() > n)
            lines.erase(lines.begin(), lines.end() - n);
        return lines;
    }

    // Minimal field readers for our own fixed-format lines.
    std::string JsonField(std::string const& line, std::string const& key)
    {
        std::string k = "\"" + key + "\":";
        size_t p = line.find(k);
        if (p == std::string::npos)
            return "";
        p += k.size();
        if (line[p] == '"')
            return line.substr(p + 1, line.find('"', p + 1) - p - 1);
        size_t e = line[p] == '[' ? line.find(']', p) + 1 : line.find_first_of(",}", p);
        return line.substr(p, e - p);
    }

    bool ParsePos(std::string const& s, Position& out)
    {
        float x, y, z, o;
        if (sscanf(s.c_str(), "[%f,%f,%f,%f]", &x, &y, &z, &o) != 4)
            return false;
        out.Relocate(x, y, z, o);
        return true;
    }

    void Warn(ChatHandler* handler, Creature* c)
    {
        if (!c)
            return;
        if (c->GetWaypointPath() || c->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE)
            handler->PSendSysMessage("setnewspawn: warning, %s walks a waypoint path. The path did not move with it.", c->GetName().c_str());
        if (c->GetFormation())
            handler->PSendSysMessage("setnewspawn: warning, %s is in a formation. Its group members did not move.", c->GetName().c_str());
    }
}

class setnewspawn_commandscript : public CommandScript
{
public:
    setnewspawn_commandscript() : CommandScript("setnewspawn_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable sub =
        {
            { "",     HandleCreature,   rbac::RBAC_PERM_COMMAND_NPC_MOVE,     Console::No },
            { "go",   HandleGameObject, rbac::RBAC_PERM_COMMAND_GOBJECT_MOVE, Console::No },
            { "undo", HandleUndo,       rbac::RBAC_PERM_COMMAND_NPC_MOVE,     Console::No },
            { "list", HandleList,       rbac::RBAC_PERM_COMMAND_NPC_MOVE,     Console::No },
        };
        static ChatCommandTable root = { { "setnewspawn", sub } };
        return root;
    }

    static bool HandleCreature(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Creature* c = handler->getSelectedCreature();
        if (!c || !c->GetSpawnId())
        {
            handler->SendSysMessage("setnewspawn: target a spawned creature (summons and temporary creatures have no spawn to move).");
            handler->SetSentErrorMessage(true);
            return false;
        }
        CreatureData const* data = sObjectMgr->GetCreatureData(c->GetSpawnId());
        if (!data || data->mapId != player->GetMapId())
            return false;

        Move m{ "creature", uint32(c->GetSpawnId()), c->GetEntry(), player->GetMapId(), data->spawnPoint, player->GetPosition() };
        Warn(handler, c);
        std::string name = c->GetName();
        MoveCreature(handler, m.spawn, m.to, c);
        Record(handler, m, name, false);
        handler->PSendSysMessage("setnewspawn: %s (spawn %u) moved here and recorded.", name.c_str(), m.spawn);
        return true;
    }

    static bool HandleGameObject(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        GameObject* go = handler->GetNearbyGameObject();
        if (!go || !go->GetSpawnId())
        {
            handler->SendSysMessage("setnewspawn: no spawned gameobject near you. Stand next to it and try again.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        Position to = player->GetPosition();
        to.SetOrientation(go->GetOrientation());
        Move m{ "gameobject", uint32(go->GetSpawnId()), go->GetEntry(), player->GetMapId(), go->GetPosition(), to };
        std::string name = go->GetGOInfo()->name;
        MoveGameObject(handler, go, to);
        Record(handler, m, name, false);
        handler->PSendSysMessage("setnewspawn: %s (spawn %u) moved here and recorded.", name.c_str(), m.spawn);
        return true;
    }

    static bool HandleUndo(ChatHandler* handler)
    {
        std::vector<std::string> lines = LastLines(1);
        if (lines.empty())
        {
            handler->SendSysMessage("setnewspawn: nothing to undo.");
            return true;
        }
        std::string const& l = lines.back();
        Move m;
        m.kind = JsonField(l, "kind");
        m.spawn = uint32(std::stoul(JsonField(l, "spawn")));
        m.entry = uint32(std::stoul(JsonField(l, "entry")));
        m.map = uint32(std::stoul(JsonField(l, "map")));
        Position back, cur;
        if (!ParsePos(JsonField(l, "old"), back) || !ParsePos(JsonField(l, "new"), cur))
            return false;
        Player* player = handler->GetSession()->GetPlayer();
        if (player->GetMapId() != m.map)
        {
            handler->PSendSysMessage("setnewspawn: the last move was on map %u; go there to undo it.", m.map);
            return true;
        }
        m.from = cur;
        m.to = back;
        if (m.kind == "creature")
            MoveCreature(handler, m.spawn, back, nullptr);
        else if (GameObject* go = handler->GetObjectFromPlayerMapByDbGuid(m.spawn))
            MoveGameObject(handler, go, back);
        else
        {
            handler->PSendSysMessage("setnewspawn: gameobject %u is not loaded near you; move closer to undo.", m.spawn);
            return true;
        }
        Record(handler, m, JsonField(l, "name"), true);
        handler->PSendSysMessage("setnewspawn: %s (spawn %u) put back.", JsonField(l, "name").c_str(), m.spawn);
        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        std::vector<std::string> lines = LastLines(8);
        if (lines.empty())
            handler->SendSysMessage("setnewspawn: no moves recorded yet.");
        for (std::string const& l : lines)
            handler->PSendSysMessage("%s %s %s (spawn %s) -> %s", JsonField(l, "undo") == "true" ? "UNDO" : "MOVE",
                JsonField(l, "kind").c_str(), JsonField(l, "name").c_str(), JsonField(l, "spawn").c_str(), JsonField(l, "new").c_str());
        return true;
    }
};

void AddSC_setnewspawn_commandscript()
{
    new setnewspawn_commandscript();
}
