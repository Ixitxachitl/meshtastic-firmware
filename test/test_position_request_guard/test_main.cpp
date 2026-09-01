// A position request is a POSITION_APP packet with want_response and no coordinates. RX is
// promiscuous, so every node in earshot runs one through PositionModule: none of them may store
// its empty lat/lon over what they already know about the sender (or about themselves, for our
// own broadcast request looping back through Router::sendLocal).
#include "MeshTypes.h"
#include "TestUtil.h"
#include <unity.h>

#if ARCH_PORTDUINO
#define POSREQ_TEST_ENTRY extern "C"
#else
#define POSREQ_TEST_ENTRY
#endif

#if !MESHTASTIC_EXCLUDE_GPS

#include "mesh/Channels.h"
#include "mesh/MeshModule.h"
#include "mesh/NodeDB.h"
#include "mesh/RadioInterface.h"
#include "mesh/Router.h"
#include "modules/PositionModule.h"
#include "modules/RoutingModule.h"
#include "support/MockMeshService.h"
#include <cstring>
#include <memory>
#include <pb_encode.h>
#include <vector>

namespace
{
constexpr NodeNum kLocalNode = 0x11111111;
constexpr NodeNum kRemoteNode = 0x22222222;
constexpr NodeNum kThirdNode = 0x33333333;

constexpr int32_t kStoredLat = 374221234;
constexpr int32_t kStoredLon = -1220845678;
constexpr uint32_t kStoredTime = 1700000000;

class TestNodeDB : public NodeDB
{
  public:
    void clearTestNodes()
    {
        testNodes.clear();
        meshNodes = &testNodes;
        numMeshNodes = 0;
    }

    void addNode(NodeNum num)
    {
        meshtastic_NodeInfoLite node = meshtastic_NodeInfoLite_init_zero;
        node.num = num;
        testNodes.push_back(node);
        meshNodes = &testNodes;
        numMeshNodes = testNodes.size();
    }

  private:
    std::vector<meshtastic_NodeInfoLite> testNodes;
};

class NullRadio : public RadioInterface
{
  public:
    ErrorCode send(meshtastic_MeshPacket *packet) override
    {
        packetPool.release(packet);
        return ERRNO_OK;
    }
    uint32_t getPacketTime(uint32_t, bool = false) override { return 0; }
};

struct SavedGlobals {
    meshtastic_LocalConfig config;
    meshtastic_LocalModuleConfig moduleConfig;
    meshtastic_ChannelFile channelFile;
    meshtastic_MyNodeInfo myNodeInfo;
    meshtastic_Position localPosition;
    NodeDB *nodeDB;
    Router *router;
    MeshService *service;
    RoutingModule *routingModule;
    PositionModule *positionModule;
    concurrency::Lock *cryptLock;
};

SavedGlobals saved;
TestNodeDB *testNodeDB = nullptr;
Router *testRouter = nullptr;
MockMeshService *testService = nullptr;
RoutingModule *testRoutingModule = nullptr;
PositionModule *testPositionModule = nullptr;

void installChannels()
{
    memset(&channelFile, 0, sizeof(channelFile));
    channelFile.channels_count = 1;
    meshtastic_Channel &primary = channelFile.channels[0];
    primary.index = 0;
    primary.role = meshtastic_Channel_Role_PRIMARY;
    primary.has_settings = true;
    strncpy(primary.settings.name, "posreq", sizeof(primary.settings.name) - 1);
    primary.settings.has_module_settings = true;
    primary.settings.module_settings.position_precision = 32;
    primary.settings.psk.size = 16;
    for (size_t i = 0; i < primary.settings.psk.size; ++i)
        primary.settings.psk.bytes[i] = static_cast<uint8_t>(0x40 + i);
    channels.onConfigChanged();
}

meshtastic_Position storedPosition()
{
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.has_latitude_i = true;
    pos.latitude_i = kStoredLat;
    pos.has_longitude_i = true;
    pos.longitude_i = kStoredLon;
    pos.altitude = 42;
    pos.time = kStoredTime;
    pos.location_source = meshtastic_Position_LocSource_LOC_INTERNAL;
    pos.precision_bits = 32;
    return pos;
}

/// Encodes to a zero-length payload, which is exactly what a client's "request position" sends.
meshtastic_MeshPacket makeRequest(NodeNum from, NodeNum to)
{
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.from = from;
    packet.to = to;
    packet.id = 0x51000001;
    packet.hop_start = 3;
    packet.hop_limit = 3;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = meshtastic_PortNum_POSITION_APP;
    packet.decoded.want_response = true;
    packet.decoded.payload.size = 0;
    return packet;
}

meshtastic_MeshPacket makeReport(NodeNum from, NodeNum to, const meshtastic_Position &position)
{
    meshtastic_MeshPacket packet = makeRequest(from, to);
    packet.decoded.want_response = false;
    packet.decoded.payload.size = pb_encode_to_bytes(packet.decoded.payload.bytes, sizeof(packet.decoded.payload.bytes),
                                                     &meshtastic_Position_msg, &position);
    TEST_ASSERT_GREATER_THAN_UINT32(0, packet.decoded.payload.size);
    return packet;
}

#if !MESHTASTIC_EXCLUDE_POSITIONDB
meshtastic_PositionLite readBack(NodeNum node)
{
    meshtastic_PositionLite got = meshtastic_PositionLite_init_zero;
    TEST_ASSERT_TRUE(nodeDB->copyNodePosition(node, got));
    return got;
}
#endif

// The request is addressed to a third node; we only overhear it. Before the guard this stored
// lat=0 lon=0 for the requester on every node in earshot, and persisted it to nodes.proto.
void test_overheard_request_keeps_the_senders_position()
{
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    nodeDB->updatePosition(kRemoteNode, storedPosition());
    meshtastic_MeshPacket request = makeRequest(kRemoteNode, kThirdNode);
    MeshModule::callModules(request, RX_SRC_RADIO);

    const meshtastic_PositionLite got = readBack(kRemoteNode);
    TEST_ASSERT_EQUAL_INT32(kStoredLat, got.latitude_i);
    TEST_ASSERT_EQUAL_INT32(kStoredLon, got.longitude_i);
    TEST_ASSERT_EQUAL_UINT32(kStoredTime, got.time);
#endif
}

// Same, for the broadcast form (a client that requests position without naming a destination).
void test_broadcast_request_keeps_the_senders_position()
{
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    nodeDB->updatePosition(kRemoteNode, storedPosition());
    meshtastic_MeshPacket request = makeRequest(kRemoteNode, NODENUM_BROADCAST);
    MeshModule::callModules(request, RX_SRC_RADIO);

    const meshtastic_PositionLite got = readBack(kRemoteNode);
    TEST_ASSERT_EQUAL_INT32(kStoredLat, got.latitude_i);
    TEST_ASSERT_EQUAL_INT32(kStoredLon, got.longitude_i);
#endif
}

// Our own broadcast request is looped back to the modules by Router::sendLocal(), so it arrives
// as a packet from us: it must not clear the fix we are about to answer with.
void test_our_own_broadcast_request_keeps_our_local_position()
{
    nodeDB->updatePosition(kLocalNode, storedPosition(), RX_SRC_LOCAL); // as our own GPS fix would
    meshtastic_MeshPacket request = makeRequest(kLocalNode, NODENUM_BROADCAST);
    MeshModule::callModules(request, RX_SRC_USER);

    TEST_ASSERT_EQUAL_INT32(kStoredLat, localPosition.latitude_i);
    TEST_ASSERT_EQUAL_INT32(kStoredLon, localPosition.longitude_i);
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    const meshtastic_PositionLite got = readBack(kLocalNode);
    TEST_ASSERT_EQUAL_INT32(kStoredLat, got.latitude_i);
    TEST_ASSERT_EQUAL_INT32(kStoredLon, got.longitude_i);
#endif
}

// The EUD's time-only packet (issue #900) still sets the time - the guard must not swallow it.
void test_time_only_packet_still_updates_time()
{
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    nodeDB->updatePosition(kRemoteNode, storedPosition());

    meshtastic_Position timeOnly = meshtastic_Position_init_zero;
    timeOnly.time = kStoredTime + 4242;
    meshtastic_MeshPacket packet = makeReport(kRemoteNode, NODENUM_BROADCAST, timeOnly);
    MeshModule::callModules(packet, RX_SRC_RADIO);

    const meshtastic_PositionLite got = readBack(kRemoteNode);
    TEST_ASSERT_EQUAL_UINT32(kStoredTime + 4242, got.time);
    TEST_ASSERT_EQUAL_INT32(kStoredLat, got.latitude_i);
    TEST_ASSERT_EQUAL_INT32(kStoredLon, got.longitude_i);
#endif
}

// The guard is about *empty* positions only: a real report still lands, coordinates and all.
void test_real_position_report_is_still_stored()
{
#if !MESHTASTIC_EXCLUDE_POSITIONDB
    nodeDB->updatePosition(kRemoteNode, storedPosition());

    meshtastic_Position moved = storedPosition();
    moved.latitude_i = kStoredLat + 1000;
    moved.longitude_i = kStoredLon - 1000;
    moved.time = kStoredTime + 60;
    meshtastic_MeshPacket packet = makeReport(kRemoteNode, NODENUM_BROADCAST, moved);
    MeshModule::callModules(packet, RX_SRC_RADIO);

    const meshtastic_PositionLite got = readBack(kRemoteNode);
    TEST_ASSERT_EQUAL_INT32(kStoredLat + 1000, got.latitude_i);
    TEST_ASSERT_EQUAL_INT32(kStoredLon - 1000, got.longitude_i);
    TEST_ASSERT_EQUAL_UINT32(kStoredTime + 60, got.time);
#endif
}
} // namespace

void setUp(void)
{
    saved.config = config;
    saved.moduleConfig = moduleConfig;
    saved.channelFile = channelFile;
    saved.myNodeInfo = myNodeInfo;
    saved.localPosition = localPosition;
    saved.nodeDB = nodeDB;
    saved.router = router;
    saved.service = service;
    saved.routingModule = routingModule;
    saved.positionModule = positionModule;
    saved.cryptLock = cryptLock;

    memset(&config, 0, sizeof(config));
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    config.lora.override_duty_cycle = true;
    memset(&moduleConfig, 0, sizeof(moduleConfig));
    memset(&myNodeInfo, 0, sizeof(myNodeInfo));
    myNodeInfo.my_node_num = kLocalNode;
    localPosition = meshtastic_Position_init_zero;
    installChannels();

    testNodeDB = new TestNodeDB();
    testNodeDB->clearTestNodes();
    testNodeDB->addNode(kLocalNode); // refreshLocalMeshNode() asserts our own entry exists
    testNodeDB->addNode(kRemoteNode);
    nodeDB = testNodeDB;

    cryptLock = nullptr;
    testRouter = new Router();
    router = testRouter;
    std::unique_ptr<NullRadio> radio(new NullRadio());
    testRouter->addInterface(std::move(radio));

    testService = new MockMeshService();
    service = testService;
    testRoutingModule = new RoutingModule();
    routingModule = testRoutingModule; // a request we can't answer is NAKed through this
    testPositionModule = new PositionModule();
    positionModule = testPositionModule;
}

void tearDown(void)
{
    delete testPositionModule;
    testPositionModule = nullptr;
    delete testRoutingModule;
    testRoutingModule = nullptr;

    // Drain what any reply queued for the (absent) phone so the pools are clean at exit.
    while (auto *status = testService->getQueueStatusForPhone())
        testService->releaseQueueStatusToPool(status);
    while (auto *packet = testService->getForPhone())
        testService->releaseToPool(packet);
    delete testService;
    testService = nullptr;

    delete testRouter;
    testRouter = nullptr;
    delete cryptLock;
    cryptLock = saved.cryptLock;

    delete testNodeDB;
    testNodeDB = nullptr;

    config = saved.config;
    moduleConfig = saved.moduleConfig;
    channelFile = saved.channelFile;
    myNodeInfo = saved.myNodeInfo;
    localPosition = saved.localPosition;
    channels.onConfigChanged();
    nodeDB = saved.nodeDB;
    router = saved.router;
    service = saved.service;
    routingModule = saved.routingModule;
    positionModule = saved.positionModule;
}

POSREQ_TEST_ENTRY void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();

    printf("\n=== Position requests never overwrite a stored position ===\n");
    RUN_TEST(test_overheard_request_keeps_the_senders_position);
    RUN_TEST(test_broadcast_request_keeps_the_senders_position);
    RUN_TEST(test_our_own_broadcast_request_keeps_our_local_position);
    RUN_TEST(test_time_only_packet_still_updates_time);
    RUN_TEST(test_real_position_report_is_still_stored);

    exit(UNITY_END());
}

POSREQ_TEST_ENTRY void loop() {}

#else // MESHTASTIC_EXCLUDE_GPS - no PositionModule to drive

void setUp(void) {}
void tearDown(void) {}

POSREQ_TEST_ENTRY void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END());
}
POSREQ_TEST_ENTRY void loop() {}

#endif
