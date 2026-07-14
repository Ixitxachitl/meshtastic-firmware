#include "Game.h"

#if HAS_SCREEN && BASEUI_HAS_GAMES

#if GAMES_ANNOUNCE_HIGH_SCORE

#include "GamesModule.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/generated/meshtastic/game.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

int32_t Game::nextBroadcastIntervalMs() const
{
    const uint32_t now = millis();
    if (lastBroadcastMs_ == 0)
        return (now >= broadcastInitialMs()) ? 0 : static_cast<int32_t>(broadcastInitialMs() - now);
    const uint32_t elapsed = now - lastBroadcastMs_;
    return (elapsed >= BROADCAST_INTERVAL_MS) ? 0 : static_cast<int32_t>(BROADCAST_INTERVAL_MS - elapsed);
}

int32_t Game::meshTick(GamesModule &host)
{
    const int32_t ms = nextBroadcastIntervalMs();
    if (ms == 0) {
        broadcastAllScores(host);
        lastBroadcastMs_ = millis();
        return static_cast<int32_t>(BROADCAST_INTERVAL_MS);
    }
    return ms;
}

void Game::broadcastAllScores(GamesModule &host)
{
#if GAME_DEMO_MODE
    return;
#endif
    if (!service)
        return;
    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    lb.game = static_cast<meshtastic_GameType>(gameType());
    lb.entries_count = 0;
    for (uint8_t i = 0; i < HighScoreTableBase::HS_COUNT; i++) {
        if (scores().scoreAt(i) == 0)
            break;
        auto &e = lb.entries[lb.entries_count];
        e.node_num = scores().nodeNumAt(i);
        strncpy(e.short_name, scores().nameAt(i), sizeof(e.short_name) - 1);
        e.short_name[sizeof(e.short_name) - 1] = '\0';
        e.score = scores().scoreAt(i);
        e.score_id = scores().scoreIdAt(i);
        lb.entries_count++;
    }
    if (lb.entries_count == 0)
        return;
    meshtastic_MeshPacket *p = host.gameAllocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = 0;
    pb_ostream_t stream = pb_ostream_from_buffer(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes));
    if (pb_encode(&stream, meshtastic_GameLeaderboard_fields, &lb)) {
        p->decoded.payload.size = static_cast<pb_size_t>(stream.bytes_written);
        service->sendToMesh(p);
        LOG_INFO("Games: %s broadcast table (%u entries)", name(), lb.entries_count);
    } else {
        LOG_WARN("Games: %s pb_encode table failed", name());
        packetPool.release(p);
    }
}

ProcessMessage Game::handleReceived(const meshtastic_MeshPacket &mp, GamesModule &host)
{
    auto isIgnored = [](NodeNum num) -> bool {
        if (!nodeDB || num == 0)
            return false;
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(num);
        return n && nodeInfoLiteIsIgnored(n);
    };
    if (isIgnored(mp.from))
        return ProcessMessage::CONTINUE;

    meshtastic_GameLeaderboard lb = meshtastic_GameLeaderboard_init_default;
    pb_istream_t stream = pb_istream_from_buffer(mp.decoded.payload.bytes, mp.decoded.payload.size);
    if (!pb_decode(&stream, meshtastic_GameLeaderboard_fields, &lb))
        return ProcessMessage::CONTINUE;
    if (lb.game != static_cast<meshtastic_GameType>(gameType()) || lb.entries_count == 0)
        return ProcessMessage::CONTINUE;

    bool changed = false;
    for (pb_size_t i = 0; i < lb.entries_count; i++) {
        auto &e = lb.entries[i];
        if (e.score == 0)
            continue;
        e.short_name[sizeof(e.short_name) - 1] = '\0';
        const NodeNum nodeNum = (e.node_num != 0) ? e.node_num : mp.from;
        bool dummy = false;
        const int rank = scores().insert(e.score, e.short_name, nodeNum, dummy, e.score_id);
        if (rank >= 0) {
            changed = true;
            LOG_INFO("Games: %s remote score %lu from 0x%08x placed at rank %d", name(), static_cast<unsigned long>(e.score),
                     nodeNum, rank + 1);
        }
    }
    if (changed)
        scores().save();

    // Only broadcast our table back when we actually absorbed new entries. Replying whenever
    // their scores don't beat ours (the old "theyHadOutrankedEntry" path) caused a feedback
    // storm: two nodes with stable, non-overlapping tables would ping each other on every
    // received packet, bypassing the 12-hour BROADCAST_INTERVAL_MS entirely. The periodic
    // broadcast is sufficient to eventually propagate a superior local table.
    if (changed) {
        broadcastAllScores(host); // no-op in GAME_DEMO_MODE
        lastBroadcastMs_ = millis();
    }
    return ProcessMessage::CONTINUE;
}

#endif // GAMES_ANNOUNCE_HIGH_SCORE

#endif // HAS_SCREEN && BASEUI_HAS_GAMES
