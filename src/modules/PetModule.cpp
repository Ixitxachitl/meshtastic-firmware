#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_PET

#include "NodeDB.h"
#include "NodeStatus.h"
#include "PetImages.h"
#include "PetModule.h"
#include "PowerStatus.h"
#include "Telemetry/UnitConversions.h"
#include "gps/RTC.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/images.h"
#include "main.h"
#include "mesh/Channels.h"
#include "mesh/generated/meshtastic/telemetry.pb.h"
#include "pb_decode.h"

#if HAS_SCREEN
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#ifndef EXCLUDE_EMOJI
#include "graphics/emotes.h"
#endif
#endif

PetModule *petModule = nullptr;

PetModule::PetModule() : MeshModule("pet"), concurrency::OSThread("PetModule")
{
    bootTime = millis();
    lastActivityTime = bootTime;

    // Start with neutral mood
    currentMood = PetMood::NEUTRAL;
    currentAnimation = PetAnimation::IDLE;

    // Initialize pet position (center of screen area)
    petX = 32;

    // This module is promiscuous - it sees all packets to track activity
    isPromiscuous = true;
}

#if defined(SENSECAP_INDICATOR) || defined(T_DECK)
void PetModule::addMessageLog(const char *line)
{
    strncpy(msgLogBuffer[msgLogHead], line, MSG_LOG_LINE_LEN - 1);
    msgLogBuffer[msgLogHead][MSG_LOG_LINE_LEN - 1] = '\0';
    msgLogHead = (msgLogHead + 1) % MSG_LOG_LINES;
    if (msgLogCount < MSG_LOG_LINES)
        msgLogCount++;
}
#endif

void PetModule::setEnabled(bool enable)
{
    if (enabled != enable) {
        enabled = enable;
#if HAS_SCREEN
        UIFrameEvent e;
        e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
        notifyObservers(&e);
#endif
    }
}

int32_t PetModule::runOnce()
{
    if (!enabled)
        return 5000; // Check again in 5 seconds

    uint32_t now = millis();

    // Update animation frame
    if (now - lastFrameTime >= FRAME_DURATION_MS) {
        advanceFrame();
        lastFrameTime = now;

#if HAS_SCREEN
        // Request screen redraw
        if (screen) {
            UIFrameEvent e;
            e.action = UIFrameEvent::Action::REDRAW_ONLY;
            notifyObservers(&e);
        }
#endif
    }

    // Periodically update mood based on activity
    static uint32_t lastMoodUpdate = 0;
    if (now - lastMoodUpdate >= MOOD_UPDATE_INTERVAL_MS) {
        updateMood();
        lastMoodUpdate = now;
    }

    // Rotate message history bucket every 5 minutes
    if (now - lastBucketRotation >= BUCKET_DURATION_MS) {
        historyBucketIndex = (historyBucketIndex + 1) % MESSAGE_HISTORY_BUCKETS;
        messageHistory[historyBucketIndex] = 0; // Reset new bucket
        lastBucketRotation = now;
    }

    return 100; // Run frequently for smooth animation
}

ProcessMessage PetModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Track message activity
    messagesReceived++;
    lastActivityTime = millis();
    lastMessageTime = lastActivityTime;

    // Increment current bucket for rolling message rate
    if (messageHistory[historyBucketIndex] < UINT16_MAX) {
        messageHistory[historyBucketIndex]++;
    }

    // Calculate hops traveled (hop_start - hop_limit gives hops used)
    if (mp.hop_start > 0 && mp.hop_limit <= mp.hop_start) {
        lastMessageHops = mp.hop_start - mp.hop_limit;
    } else {
        lastMessageHops = 0; // Direct or unknown
    }

    // Track what type of message was received
    switch (mp.decoded.portnum) {
    case meshtastic_PortNum_UNKNOWN_APP:
        lastMessageType = LastMessageType::UNKNOWN;
        break;
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        lastMessageType = LastMessageType::TEXT;
        break;
    case meshtastic_PortNum_REMOTE_HARDWARE_APP:
        lastMessageType = LastMessageType::REMOTE_HW;
        break;
    case meshtastic_PortNum_POSITION_APP:
        lastMessageType = LastMessageType::POSITION;
        break;
    case meshtastic_PortNum_NODEINFO_APP:
        lastMessageType = LastMessageType::NODEINFO;
        break;
    case meshtastic_PortNum_ROUTING_APP:
        lastMessageType = LastMessageType::ROUTING;
        break;
    case meshtastic_PortNum_ADMIN_APP:
        lastMessageType = LastMessageType::ADMIN;
        break;
    case meshtastic_PortNum_TEXT_MESSAGE_COMPRESSED_APP:
        lastMessageType = LastMessageType::TEXT_COMPRESSED;
        break;
    case meshtastic_PortNum_WAYPOINT_APP:
        lastMessageType = LastMessageType::WAYPOINT;
        break;
    case meshtastic_PortNum_AUDIO_APP:
        lastMessageType = LastMessageType::AUDIO;
        break;
    case meshtastic_PortNum_DETECTION_SENSOR_APP:
        lastMessageType = LastMessageType::DETECTION;
        break;
    case meshtastic_PortNum_ALERT_APP:
        lastMessageType = LastMessageType::ALERT;
        break;
    case meshtastic_PortNum_KEY_VERIFICATION_APP:
        lastMessageType = LastMessageType::KEY_VERIFY;
        break;
    case meshtastic_PortNum_REPLY_APP:
        lastMessageType = LastMessageType::REPLY;
        break;
    case meshtastic_PortNum_IP_TUNNEL_APP:
        lastMessageType = LastMessageType::IP_TUNNEL;
        break;
    case meshtastic_PortNum_PAXCOUNTER_APP:
        lastMessageType = LastMessageType::PAXCOUNT;
        break;
    case meshtastic_PortNum_STORE_FORWARD_PLUSPLUS_APP:
        lastMessageType = LastMessageType::STORE_FWD_PP;
        break;
    case meshtastic_PortNum_NODE_STATUS_APP:
        lastMessageType = LastMessageType::NODE_STATUS;
        break;
    case meshtastic_PortNum_SERIAL_APP:
        lastMessageType = LastMessageType::SERIAL_PORT;
        break;
    case meshtastic_PortNum_STORE_FORWARD_APP:
        lastMessageType = LastMessageType::STORE_FWD;
        break;
    case meshtastic_PortNum_RANGE_TEST_APP:
        lastMessageType = LastMessageType::RANGE_TEST;
        break;
    case meshtastic_PortNum_TELEMETRY_APP:
        lastMessageType = LastMessageType::TELEMETRY;
        break;
    case meshtastic_PortNum_ZPS_APP:
        lastMessageType = LastMessageType::ZPS;
        break;
    case meshtastic_PortNum_SIMULATOR_APP:
        lastMessageType = LastMessageType::SIMULATOR;
        break;
    case meshtastic_PortNum_TRACEROUTE_APP:
        lastMessageType = LastMessageType::TRACEROUTE;
        break;
    case meshtastic_PortNum_NEIGHBORINFO_APP:
        lastMessageType = LastMessageType::NEIGHBOR;
        break;
    case meshtastic_PortNum_ATAK_PLUGIN:
        lastMessageType = LastMessageType::ATAK;
        break;
    case meshtastic_PortNum_MAP_REPORT_APP:
        lastMessageType = LastMessageType::MAP_REPORT;
        break;
    case meshtastic_PortNum_POWERSTRESS_APP:
        lastMessageType = LastMessageType::POWERSTRESS;
        break;
    case meshtastic_PortNum_RETICULUM_TUNNEL_APP:
        lastMessageType = LastMessageType::RETICULUM;
        break;
    case meshtastic_PortNum_CAYENNE_APP:
        lastMessageType = LastMessageType::CAYENNE;
        break;
    case meshtastic_PortNum_PRIVATE_APP:
        lastMessageType = LastMessageType::PRIVATE;
        break;
    case meshtastic_PortNum_ATAK_FORWARDER:
        lastMessageType = LastMessageType::ATAK_FWD;
        break;
    default:
        lastMessageType = LastMessageType::UNKNOWN;
        break;
    }

#if defined(SENSECAP_INDICATOR) || defined(T_DECK)
    // Log received message to on-screen buffer with timestamp, signal info, and node name
    {
        char logLine[MSG_LOG_LINE_LEN];
        uint32_t fromNode = mp.from;

        // Get timestamp - use real time if available, otherwise uptime
        char timeBuf[16];
        uint32_t rtc_sec = getValidTime(RTCQuality::RTCQualityDevice, true);
        if (rtc_sec > 0) {
            // Real time available - format based on 12/24 hour setting
            long hms = rtc_sec % SEC_PER_DAY;
            hms = (hms + SEC_PER_DAY) % SEC_PER_DAY;
            int hour = hms / SEC_PER_HOUR;
            int min = (hms % SEC_PER_HOUR) / SEC_PER_MIN;
            int sec = hms % SEC_PER_MIN;
            if (config.display.use_12h_clock) {
                bool isPM = hour >= 12;
                int hour12 = hour % 12;
                if (hour12 == 0)
                    hour12 = 12;
                snprintf(timeBuf, sizeof(timeBuf), "%d:%02d:%02d%s", hour12, min, sec, isPM ? "p" : "a");
            } else {
                snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", hour, min, sec);
            }
        } else {
            // No real time - use uptime as mm:ss or h:mm:ss
            uint32_t secs = millis() / 1000;
            if (secs < 3600) {
                snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
            } else {
                snprintf(timeBuf, sizeof(timeBuf), "%lu:%02lu:%02lu", (unsigned long)(secs / 3600),
                         (unsigned long)((secs % 3600) / 60), (unsigned long)(secs % 60));
            }
        }

        // Try to get long name from NodeDB
        const char *nodeName = nullptr;
        if (nodeDB) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(fromNode);
            if (node && node->has_user && node->user.long_name[0]) {
                nodeName = node->user.long_name;
            }
        }

        // Extract data preview based on message type
        // Use specific type name for telemetry variants
        const char *typeName = getMessageTypeName(lastMessageType);
        char dataBuf[120] = "";
        if (mp.pki_encrypted) {
            // PKI encrypted message - we can't see the contents
            snprintf(dataBuf, sizeof(dataBuf), "[encrypted]");
        } else if (mp.decoded.payload.size > 0) {
            switch (lastMessageType) {
            case LastMessageType::TEXT:
            case LastMessageType::TEXT_COMPRESSED: {
                // Show channel name and text message (no truncation - let the display wrap)
                const char *chanName = channels.getName(mp.channel);
                char textPreview[100];
                size_t j = 0;
                for (size_t i = 0; i < mp.decoded.payload.size && j < sizeof(textPreview) - 1; i++) {
                    char c = ((const char *)mp.decoded.payload.bytes)[i];
                    if (c != '\n' && c != '\r') {
                        textPreview[j++] = c;
                    }
                }
                textPreview[j] = '\0';
                snprintf(dataBuf, sizeof(dataBuf), "#%s \"%s\"", chanName, textPreview);
                break;
            }
            case LastMessageType::TELEMETRY: {
                // Try to decode telemetry for useful data
                meshtastic_Telemetry telem;
                memset(&telem, 0, sizeof(telem));
                if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Telemetry_msg, &telem)) {
                    if (telem.which_variant == meshtastic_Telemetry_device_metrics_tag) {
                        typeName = "Device";
                        // Get device metrics from NodeDB which has the properly decoded values
                        meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNode(mp.from) : nullptr;
                        if (node && node->has_device_metrics) {
                            auto &dm = node->device_metrics;
                            char parts[4][16];
                            int partCount = 0;
                            if (dm.has_battery_level && dm.has_voltage) {
                                snprintf(parts[partCount++], 16, "%u%%/%.1fV", (unsigned int)dm.battery_level, dm.voltage);
                            }
                            if (dm.has_channel_utilization) {
                                snprintf(parts[partCount++], 16, "Ch:%.0f%%", dm.channel_utilization);
                            }
                            if (dm.has_air_util_tx) {
                                snprintf(parts[partCount++], 16, "Air:%.0f%%", dm.air_util_tx);
                            }
                            if (dm.has_uptime_seconds && partCount < 3) {
                                uint32_t up = dm.uptime_seconds;
                                if (up >= 86400) {
                                    snprintf(parts[partCount++], 16, "%lud", (unsigned long)(up / 86400));
                                } else if (up >= 3600) {
                                    snprintf(parts[partCount++], 16, "%luh", (unsigned long)(up / 3600));
                                } else {
                                    snprintf(parts[partCount++], 16, "%lum", (unsigned long)(up / 60));
                                }
                            }
                            dataBuf[0] = '\0';
                            for (int i = 0; i < partCount && i < 4; i++) {
                                if (i > 0)
                                    strncat(dataBuf, " ", sizeof(dataBuf) - strlen(dataBuf) - 1);
                                strncat(dataBuf, parts[i], sizeof(dataBuf) - strlen(dataBuf) - 1);
                            }
                        }
                    } else if (telem.which_variant == meshtastic_Telemetry_environment_metrics_tag) {
                        typeName = "Env";
                        auto &em = telem.variant.environment_metrics;
                        bool useFahrenheit = moduleConfig.telemetry.environment_display_fahrenheit;
                        // Build a compact string with all available environment fields
                        char parts[12][20];
                        int partCount = 0;
                        // Temperature and humidity (combine if both present)
                        if (em.has_temperature) {
                            float temp = useFahrenheit ? UnitConversions::CelsiusToFahrenheit(em.temperature) : em.temperature;
                            if (em.has_relative_humidity) {
                                snprintf(parts[partCount++], 20, "%.1f%s/%.0f%%", temp, useFahrenheit ? "F" : "C",
                                         em.relative_humidity);
                            } else {
                                snprintf(parts[partCount++], 20, "%.1f%s", temp, useFahrenheit ? "F" : "C");
                            }
                        } else if (em.has_relative_humidity) {
                            snprintf(parts[partCount++], 20, "%.0f%%RH", em.relative_humidity);
                        }
                        // Voltage/current from INA sensors (solar monitoring) - prefix with Sol:
                        if (em.has_voltage) {
                            if (em.has_current) {
                                snprintf(parts[partCount++], 20, "Sol:%.1fV/%.0fmA", em.voltage, em.current * 1000);
                            } else {
                                snprintf(parts[partCount++], 20, "Sol:%.1fV", em.voltage);
                            }
                        }
                        // Barometric pressure
                        if (em.has_barometric_pressure) {
                            snprintf(parts[partCount++], 20, "%.0fhPa", em.barometric_pressure);
                        }
                        // IAQ (air quality index)
                        if (em.has_iaq) {
                            snprintf(parts[partCount++], 20, "IAQ:%u", em.iaq);
                        }
                        // Wind speed
                        if (em.has_wind_speed) {
                            snprintf(parts[partCount++], 20, "%.1fm/s", em.wind_speed);
                        }
                        // Lux
                        if (em.has_lux) {
                            snprintf(parts[partCount++], 20, "%.0flux", em.lux);
                        }
                        // Distance
                        if (em.has_distance) {
                            snprintf(parts[partCount++], 20, "%.0fmm", em.distance);
                        }
                        // Radiation
                        if (em.has_radiation) {
                            snprintf(parts[partCount++], 20, "%.1fuR/h", em.radiation);
                        }
                        // Weight
                        if (em.has_weight) {
                            snprintf(parts[partCount++], 20, "%.1fkg", em.weight);
                        }
                        // Soil moisture
                        if (em.has_soil_moisture) {
                            snprintf(parts[partCount++], 20, "Soil:%u%%", em.soil_moisture);
                        }
                        // Join all parts with spaces
                        dataBuf[0] = '\0';
                        for (int i = 0; i < partCount; i++) {
                            if (i > 0)
                                strncat(dataBuf, " ", sizeof(dataBuf) - strlen(dataBuf) - 1);
                            strncat(dataBuf, parts[i], sizeof(dataBuf) - strlen(dataBuf) - 1);
                        }
                    } else if (telem.which_variant == meshtastic_Telemetry_power_metrics_tag) {
                        typeName = "Power";
                        auto &pm = telem.variant.power_metrics;
                        // Show first channel with data
                        if (pm.has_ch1_voltage) {
                            snprintf(dataBuf, sizeof(dataBuf), "%.1fV/%.0fmA", pm.ch1_voltage, pm.ch1_current * 1000);
                        } else if (pm.has_ch2_voltage) {
                            snprintf(dataBuf, sizeof(dataBuf), "%.1fV/%.0fmA", pm.ch2_voltage, pm.ch2_current * 1000);
                        } else {
                            snprintf(dataBuf, sizeof(dataBuf), "Pwr");
                        }
                    } else if (telem.which_variant == meshtastic_Telemetry_air_quality_metrics_tag) {
                        typeName = "AirQual";
                        auto &aq = telem.variant.air_quality_metrics;
                        if (aq.has_pm25_standard) {
                            snprintf(dataBuf, sizeof(dataBuf), "PM2.5:%u", aq.pm25_standard);
                        } else if (aq.has_co2) {
                            snprintf(dataBuf, sizeof(dataBuf), "CO2:%uppm", aq.co2);
                        } else {
                            snprintf(dataBuf, sizeof(dataBuf), "AQI");
                        }
                    } else if (telem.which_variant == meshtastic_Telemetry_local_stats_tag) {
                        typeName = "Stats";
                        auto &ls = telem.variant.local_stats;
                        snprintf(dataBuf, sizeof(dataBuf), "%u nodes", ls.num_online_nodes);
                    } else if (telem.which_variant == meshtastic_Telemetry_health_metrics_tag) {
                        typeName = "Health";
                        auto &hm = telem.variant.health_metrics;
                        if (hm.has_heart_bpm) {
                            snprintf(dataBuf, sizeof(dataBuf), "%ubpm", hm.heart_bpm);
                        } else if (hm.has_spO2) {
                            snprintf(dataBuf, sizeof(dataBuf), "SpO2:%u%%", hm.spO2);
                        }
                    }
                    // else leave dataBuf empty for unknown variants
                }
                break;
            }
            case LastMessageType::POSITION: {
                // Decode position for lat/lon
                meshtastic_Position pos;
                memset(&pos, 0, sizeof(pos));
                if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Position_msg, &pos)) {
                    if (pos.latitude_i != 0 || pos.longitude_i != 0) {
                        float lat = pos.latitude_i * 1e-7;
                        float lon = pos.longitude_i * 1e-7;
                        snprintf(dataBuf, sizeof(dataBuf), "%.4f,%.4f", lat, lon);
                    } else if (pos.altitude != 0) {
                        snprintf(dataBuf, sizeof(dataBuf), "alt:%dm", pos.altitude);
                    }
                }
                break;
            }
            case LastMessageType::NODEINFO: {
                // Decode nodeinfo for short name
                meshtastic_User user;
                memset(&user, 0, sizeof(user));
                if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_User_msg, &user)) {
                    if (user.short_name[0]) {
                        snprintf(dataBuf, sizeof(dataBuf), "%s", user.short_name);
                    }
                }
                break;
            }
            case LastMessageType::WAYPOINT: {
                // Decode waypoint for name
                meshtastic_Waypoint wp;
                memset(&wp, 0, sizeof(wp));
                if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Waypoint_msg, &wp)) {
                    if (wp.name[0]) {
                        snprintf(dataBuf, sizeof(dataBuf), "%.20s", wp.name);
                    }
                }
                break;
            }
            case LastMessageType::NEIGHBOR: {
                // Decode neighbor info for count
                meshtastic_NeighborInfo ni;
                memset(&ni, 0, sizeof(ni));
                if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_NeighborInfo_msg, &ni)) {
                    snprintf(dataBuf, sizeof(dataBuf), "%u neighbors", ni.neighbors_count);
                }
                break;
            }
            case LastMessageType::TRACEROUTE: {
                // Show origin -> destination or origin <- destination with hop count
                // route_back_count > 0 means this is the return path
                char fromStr[12];
                char toStr[12];
                if (nodeDB) {
                    meshtastic_NodeInfoLite *fromNode = nodeDB->getMeshNode(mp.from);
                    meshtastic_NodeInfoLite *toNode = nodeDB->getMeshNode(mp.to);
                    if (fromNode && fromNode->has_user && fromNode->user.short_name[0])
                        snprintf(fromStr, sizeof(fromStr), "%s", fromNode->user.short_name);
                    else
                        snprintf(fromStr, sizeof(fromStr), "!%08x", mp.from);
                    if (toNode && toNode->has_user && toNode->user.short_name[0])
                        snprintf(toStr, sizeof(toStr), "%s", toNode->user.short_name);
                    else
                        snprintf(toStr, sizeof(toStr), "!%08x", mp.to);
                } else {
                    snprintf(fromStr, sizeof(fromStr), "!%08x", mp.from);
                    snprintf(toStr, sizeof(toStr), "!%08x", mp.to);
                }
                meshtastic_RouteDiscovery tr;
                memset(&tr, 0, sizeof(tr));
                if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_RouteDiscovery_msg,
                                         &tr)) {
                    bool isReturn = tr.route_back_count > 0;
                    uint8_t hops = isReturn ? tr.route_back_count : tr.route_count;
                    snprintf(dataBuf, sizeof(dataBuf), "%s%s%s %u hops", fromStr, isReturn ? "<-" : "->", toStr, hops);
                }
                break;
            }
            case LastMessageType::ROUTING: {
                // Decode routing info
                meshtastic_Routing routing;
                memset(&routing, 0, sizeof(routing));
                if (pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Routing_msg, &routing)) {
                    if (routing.which_variant == meshtastic_Routing_error_reason_tag) {
                        const char *errStr = "err";
                        switch (routing.error_reason) {
                        case meshtastic_Routing_Error_NONE:
                            errStr = "OK";
                            break;
                        case meshtastic_Routing_Error_NO_ROUTE:
                            errStr = "no route";
                            break;
                        case meshtastic_Routing_Error_GOT_NAK:
                            errStr = "NAK";
                            break;
                        case meshtastic_Routing_Error_TIMEOUT:
                            errStr = "timeout";
                            break;
                        case meshtastic_Routing_Error_NO_INTERFACE:
                            errStr = "no iface";
                            break;
                        case meshtastic_Routing_Error_MAX_RETRANSMIT:
                            errStr = "max retx";
                            break;
                        case meshtastic_Routing_Error_NO_CHANNEL:
                            errStr = "no chan";
                            break;
                        case meshtastic_Routing_Error_TOO_LARGE:
                            errStr = "too big";
                            break;
                        case meshtastic_Routing_Error_NO_RESPONSE:
                            errStr = "no resp";
                            break;
                        case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
                            errStr = "duty lim";
                            break;
                        case meshtastic_Routing_Error_BAD_REQUEST:
                            errStr = "bad req";
                            break;
                        case meshtastic_Routing_Error_NOT_AUTHORIZED:
                            errStr = "unauth";
                            break;
                        case meshtastic_Routing_Error_PKI_FAILED:
                            errStr = "PKI fail";
                            break;
                        case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
                            errStr = "unk key";
                            break;
                        case meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY:
                            errStr = "bad sess";
                            break;
                        case meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED:
                            errStr = "key unauth";
                            break;
                        default:
                            break;
                        }
                        snprintf(dataBuf, sizeof(dataBuf), "%s", errStr);
                    }
                }
                break;
            }
            case LastMessageType::RANGE_TEST: {
                // Range test is just a text payload
                if (mp.decoded.payload.size > 0 && mp.decoded.payload.size < 20) {
                    snprintf(dataBuf, sizeof(dataBuf), "\"%.16s\"", (const char *)mp.decoded.payload.bytes);
                }
                break;
            }
            case LastMessageType::DETECTION: {
                // Detection sensor - show the detection text
                if (mp.decoded.payload.size > 0) {
                    snprintf(dataBuf, sizeof(dataBuf), "\"%.16s\"", (const char *)mp.decoded.payload.bytes);
                }
                break;
            }
            default:
                // Leave dataBuf empty for other message types
                break;
            }
        }

        // Format: "HH:MM:SS [hops] Type Data Name"
        const char *nodeStr = nodeName ? nodeName : "!????????";
        char nodeIdBuf[12];
        if (!nodeName) {
            snprintf(nodeIdBuf, sizeof(nodeIdBuf), "!%08x", fromNode);
            nodeStr = nodeIdBuf;
        }

        if (dataBuf[0]) {
            snprintf(logLine, sizeof(logLine), "%s [%u] %s %s %s", timeBuf, lastMessageHops, typeName, dataBuf, nodeStr);
        } else {
            snprintf(logLine, sizeof(logLine), "%s [%u] %s %s", timeBuf, lastMessageHops, typeName, nodeStr);
        }
        addMessageLog(logLine);
    }
#endif

    // Boost happiness when receiving messages (pet likes activity!)
    if (happiness < 95) {
        happiness += 5;
    }

    // Gain XP for receiving messages
    experience += 5;

    // Wake up from sleep if sleeping (but only if battery is OK)
    if (currentMood == PetMood::SLEEPING && energy >= 30) {
        // Wake up animation
        currentMood = PetMood::HAPPY;
        currentAnimation = PetAnimation::HAPPY_BOUNCE;
    }

    // Check if this is from a "new" node we haven't counted
    if (nodeDB) {
        uint32_t currentNodes = nodeDB->getNumMeshNodes();
        if (currentNodes > nodesDiscovered) {
            nodesDiscovered = currentNodes;
            // New node discovered - get excited!
            currentMood = PetMood::EXCITED;
            currentAnimation = PetAnimation::EXCITED;
            happiness = 100;  // Max happiness for new discovery!
            experience += 50; // Bonus XP for new node!
        }
    }

    // Check for level up
    while (experience >= xpForNextLevel) {
        experience -= xpForNextLevel;
        level++;
        xpForNextLevel = 100 * level; // Each level requires more XP
        // Level up excitement!
        currentMood = PetMood::EXCITED;
        currentAnimation = PetAnimation::EXCITED;
    }

    // Set animation based on message type (if not already excited from level up or new node)
    if (currentMood != PetMood::EXCITED) {
        switch (lastMessageType) {
        case LastMessageType::TEXT:
        case LastMessageType::TEXT_COMPRESSED:
            // Text message - reading!
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::READING;
            break;
        case LastMessageType::POSITION:
        case LastMessageType::WAYPOINT:
        case LastMessageType::TRACEROUTE:
        case LastMessageType::ROUTING:
            // Movement/navigation - looking around
            currentMood = PetMood::NEUTRAL;
            currentAnimation = PetAnimation::LOOKING;
            break;
        case LastMessageType::NODEINFO:
        case LastMessageType::NEIGHBOR:
            // Node info - wave hello!
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::WAVING;
            break;
        case LastMessageType::TELEMETRY:
            // Telemetry data - thinking/processing
            currentMood = PetMood::NEUTRAL;
            currentAnimation = PetAnimation::THINKING;
            break;
        case LastMessageType::ADMIN:
        case LastMessageType::ALERT:
        case LastMessageType::DETECTION:
            // Important/alert messages - scared!
            currentMood = PetMood::ALERT;
            currentAnimation = PetAnimation::SCARED;
            break;
        case LastMessageType::RANGE_TEST:
            // Range test - running around excited
            currentMood = PetMood::EXCITED;
            currentAnimation = PetAnimation::EXCITED;
            break;
        case LastMessageType::STORE_FWD:
        case LastMessageType::STORE_FWD_PP:
            // Store & forward - eating stored food
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::EATING;
            break;
        case LastMessageType::ATAK:
        case LastMessageType::ATAK_FWD:
        case LastMessageType::MAP_REPORT:
            // Tactical/map - looking around carefully
            currentMood = PetMood::ALERT;
            currentAnimation = PetAnimation::LOOKING;
            break;
        default:
            // Other messages - just alert briefly
            currentMood = PetMood::ALERT;
            currentAnimation = PetAnimation::ALERT;
            break;
        }
    }

    return ProcessMessage::CONTINUE; // Let other modules also process
}

uint32_t PetModule::getUptimeMinutes() const
{
    return (millis() - bootTime) / 60000;
}

void PetModule::updateHappiness()
{
    uint32_t now = millis();
    uint32_t timeSinceActivity = now - lastActivityTime;

    // Energy is battery percent
    if (powerStatus) {
        energy = powerStatus->getBatteryChargePercent();
        if (energy > 100)
            energy = 100;
    }

    // Don't decay happiness while sleeping
    if (currentMood == PetMood::SLEEPING) {
        return;
    }

    // More nuanced happiness decay based on time since last activity
    // Decay faster the longer inactive: ~1 point per minute after first minute
    uint32_t inactiveMinutes = timeSinceActivity / 60000;
    if (inactiveMinutes > 0 && happiness > 5) {
        // Cap decay to prevent going below minimum happiness of 5
        uint8_t maxDecay = happiness - 5;
        uint8_t decay = (inactiveMinutes < maxDecay) ? (uint8_t)inactiveMinutes : maxDecay;
        // Only decay 1 point per mood update cycle to smooth out changes
        if (decay > 0) {
            happiness--;
        }
    }
}

uint8_t PetModule::calculateHealth() const
{
    // Composite health score from multiple factors:
    // 40% happiness, 30% energy (battery), 20% recent activity, 10% network

    // Activity score: high if recent activity, decays over time
    uint32_t timeSinceActivity = millis() - lastActivityTime;
    uint32_t inactiveMinutes = timeSinceActivity / 60000;
    uint8_t activityScore = 0;
    if (messagesReceived > 0) {
        // Start at 100, lose ~1 point per minute of inactivity, min 0
        activityScore = (inactiveMinutes < 100) ? (100 - (uint8_t)inactiveMinutes) : 0;
    }

    // Network score: more nodes = better, caps at 10 nodes for max score
    uint8_t networkScore = (nodesDiscovered >= 10) ? 100 : (uint8_t)(nodesDiscovered * 10);

    // Weighted average
    uint16_t health = (happiness * 40 + energy * 30 + activityScore * 20 + networkScore * 10) / 100;

    return (health > 100) ? 100 : (uint8_t)health;
}

uint16_t PetModule::getMessagesPerMinute() const
{
    // Sum all buckets to get messages in the last minute
    uint16_t total = 0;
    for (uint8_t i = 0; i < MESSAGE_HISTORY_BUCKETS; i++) {
        total += messageHistory[i];
    }
    return total;
}

const uint8_t *PetModule::getSpeedIcon(uint16_t messagesPerMinute) const
{
    // Scale for per-minute traffic: 0=idle, 1-2=low, 3-5=medium, 6-10=high, 10+=very high
    if (messagesPerMinute == 0) {
        return speed0_icon;
    } else if (messagesPerMinute < 3) {
        return speed25_icon;
    } else if (messagesPerMinute < 6) {
        return speed50_icon;
    } else if (messagesPerMinute < 10) {
        return speed75_icon;
    } else {
        return speed100_icon;
    }
}

void PetModule::updateMood()
{
    uint32_t now = millis();
    uint32_t timeSinceActivity = now - lastActivityTime;

    // Update happiness/energy first
    updateHappiness();

    // Calculate composite health for mood decisions
    uint8_t health = calculateHealth();

    // Priority 1: Critical battery states (most important)
    if (energy > 0 && energy < 20) {
        currentMood = PetMood::SLEEPING;
        currentAnimation = PetAnimation::SLEEPING;
        return;
    }
    if (energy >= 20 && energy < 40) {
        currentMood = PetMood::HUNGRY;
        currentAnimation = PetAnimation::SAD;
        return;
    }

    // Priority 2: Extended inactivity - pet sleeps
    if (timeSinceActivity > IDLE_THRESHOLD_MS) {
        currentMood = PetMood::SLEEPING;
        currentAnimation = PetAnimation::SLEEPING;
        return;
    }

    // Priority 3: Emotional states based on health/happiness
    if (health < 25 || happiness < 25) {
        currentMood = PetMood::SAD;
        currentAnimation = PetAnimation::SAD;
        return;
    }

    // Priority 4: Recent activity responses (within 5 seconds)
    if (timeSinceActivity < 5000) {
        currentMood = PetMood::ALERT;
        currentAnimation = PetAnimation::ALERT;
        return;
    }

    // Priority 5: Semi-recent activity (within 30 seconds) - eating
    if (timeSinceActivity < 30000 && messagesReceived > 0) {
        currentMood = PetMood::HAPPY;
        currentAnimation = PetAnimation::EATING;
        return;
    }

    // Priority 6: Getting bored (halfway to sleep threshold)
    if (timeSinceActivity > IDLE_THRESHOLD_MS / 2) {
        currentMood = PetMood::NEUTRAL;
        // Occasionally yawn or scratch when bored
        if (rand() % 10 == 0) {
            currentAnimation = (rand() % 2 == 0) ? PetAnimation::YAWNING : PetAnimation::SCRATCHING;
        } else {
            currentAnimation = PetAnimation::IDLE;
        }
        return;
    }

    // Priority 7: Content state - random behaviors based on happiness
    // Higher happiness = more playful behaviors
    if (happiness > 70) {
        // Very happy - more active behaviors
        uint8_t choice = rand() % 100;
        if (choice < 15) {
            // 15% chance to play
            currentMood = PetMood::HAPPY;
            uint8_t playChoice = rand() % 3;
            if (playChoice == 0) {
                currentAnimation = PetAnimation::PLAYING;
            } else if (playChoice == 1) {
                currentAnimation = PetAnimation::DANCING;
            } else {
                currentAnimation = PetAnimation::HOPPING;
            }
        } else if (choice < 25) {
            // 10% chance to sniff around
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::SNIFFING;
        } else if (choice < 40) {
            // 15% chance to bounce happily
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::HAPPY_BOUNCE;
        } else if (choice < 55) {
            // 15% chance to walk/hop around
            currentMood = PetMood::HAPPY;
            currentAnimation = (rand() % 2 == 0) ? PetAnimation::WALKING : PetAnimation::HOPPING;
        } else {
            // 45% chance to idle happily
            currentMood = PetMood::HAPPY;
            currentAnimation = PetAnimation::IDLE;
        }
    } else if (happiness > 40) {
        // Moderately happy - calmer behaviors
        uint8_t choice = rand() % 100;
        if (choice < 20) {
            // 20% chance to walk
            currentMood = PetMood::NEUTRAL;
            currentAnimation = PetAnimation::WALKING;
        } else if (choice < 30) {
            // 10% chance to look around
            currentMood = PetMood::NEUTRAL;
            currentAnimation = PetAnimation::LOOKING;
        } else {
            // 70% chance to idle
            currentMood = PetMood::NEUTRAL;
            currentAnimation = PetAnimation::IDLE;
        }
    } else {
        // Low happiness but not sad - mostly idle
        currentMood = PetMood::NEUTRAL;
        currentAnimation = PetAnimation::IDLE;
    }
}

void PetModule::updateAnimation()
{
    // Handle walking animation - move pet back and forth within pet box
    // petX ranges from 0 to maxWalkX, which gets scaled to actual walk range in drawFrame
#ifdef SENSECAP_INDICATOR
    const int16_t maxWalkX = 60; // 3x wider box needs 3x more steps to maintain same speed
#elif defined(T_DECK)
    const int16_t maxWalkX = 35; // 1.75x wider box needs 1.75x more steps
#else
    const int16_t maxWalkX = 20;
#endif

    if (currentAnimation == PetAnimation::WALKING || currentAnimation == PetAnimation::HOPPING) {
        petX += petDirection * 2; // Move 2 units per frame for visible movement
        // Bounce within the range
        if (petX >= maxWalkX) {
            petX = maxWalkX;
            petDirection = -1;
        } else if (petX <= 0) {
            petX = 0;
            petDirection = 1;
        }
    } else {
        // Center position when not walking
        petX = maxWalkX / 2;
    }
}

void PetModule::advanceFrame()
{
    animationFrame++;
    if (animationFrame >= getFrameCount()) {
        animationFrame = 0;
    }
    updateAnimation();
}

uint8_t PetModule::getFrameCount() const
{
    // Return frame count based on current animation
    switch (currentAnimation) {
    case PetAnimation::IDLE:
        return PET_IDLE_FRAMES;
    case PetAnimation::WALKING:
        return PET_WALK_FRAMES;
    case PetAnimation::HAPPY_BOUNCE:
        return PET_HAPPY_FRAMES;
    case PetAnimation::SLEEPING:
        return PET_SLEEP_FRAMES;
    case PetAnimation::EXCITED:
        return PET_EXCITED_FRAMES;
    case PetAnimation::EATING:
        return PET_EATING_FRAMES;
    case PetAnimation::PLAYING:
        return PET_PLAYING_FRAMES;
    case PetAnimation::ALERT:
        return PET_ALERT_FRAMES;
    case PetAnimation::SAD:
        return PET_SAD_FRAMES;
    case PetAnimation::READING:
        return PET_READING_FRAMES;
    case PetAnimation::LOOKING:
        return PET_LOOKING_FRAMES;
    case PetAnimation::WAVING:
        return PET_WAVING_FRAMES;
    case PetAnimation::THINKING:
        return PET_THINKING_FRAMES;
    case PetAnimation::SCARED:
        return PET_SCARED_FRAMES;
    case PetAnimation::HOPPING:
        return PET_HOPPING_FRAMES;
    case PetAnimation::SCRATCHING:
        return PET_SCRATCHING_FRAMES;
    case PetAnimation::DANCING:
        return PET_DANCING_FRAMES;
    case PetAnimation::YAWNING:
        return PET_YAWNING_FRAMES;
    case PetAnimation::SNIFFING:
        return PET_SNIFFING_FRAMES;
    default:
        return 1;
    }
}

#if HAS_SCREEN

void PetModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    if (!enabled)
        return;

    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    // Determine scale factor based on screen resolution
    const uint8_t scale = graphics::isHighResolution() ? 2 : 1;

    // Draw header
    const char *titleStr = "Chirpy";
    graphics::drawCommonHeader(display, x, y, titleStr);

    // Get display dimensions
    const int16_t screenW = display->getWidth();
    const int16_t screenH = display->getHeight();
    const int16_t contentY = y + FONT_HEIGHT_SMALL - 2; // Tighter - right below header, shifted up 2px

    // Scaled dimensions
    const int16_t moodSize = 16 * scale;
    const int16_t petW = PET_FRAME_WIDTH * scale;
    const int16_t petH = PET_FRAME_HEIGHT * scale;
    const int16_t lvlIconW = lvl_icon_width * scale;
    const int16_t lvlIconH = lvl_icon_height * scale;

    // Mood indicator in top-left corner (below header)
    drawMoodIndicator(display, x + 1, contentY + 3 * scale, scale);

    // Show level under mood indicator - icon with number below (stays up 1px from mood)
    int16_t lvlIconX = x + 3;
    int16_t lvlIconY = contentY + 3 * scale + moodSize - 1;
    if (scale > 1) {
        drawXbmScaled(display, lvlIconX, lvlIconY, lvl_icon_width, lvl_icon_height, lvl_icon, scale);
    } else {
        display->drawXbm(lvlIconX, contentY + 3 + 16 - 1, lvl_icon_width, lvl_icon_height, lvl_icon);
    }
    char lvlBuf[8];
    snprintf(lvlBuf, sizeof(lvlBuf), "%u", level);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    int16_t lvlNumY = (scale > 1) ? (lvlIconY + lvlIconH - 1) : (contentY + 3 + 16 + lvl_icon_height - 3 - 1);
    display->drawString(lvlIconX + (lvlIconW / 2), lvlNumY, lvlBuf);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Pet area - wider box for pet (shifted down 1px)
#ifdef SENSECAP_INDICATOR
    const int16_t petBoxW = (petW + 32 * scale) * 3; // 3x wider on Indicator
#elif defined(T_DECK)
    const int16_t petBoxW = ((petW + 32 * scale) * 7) / 4;              // 1.75x wider on T-Deck
#else
    const int16_t petBoxW = petW + 32 * scale;
#endif
    const int16_t petBoxH = petH + 4 * scale;
    const int16_t petBoxX = x + 18 * scale; // After mood indicator
    const int16_t petBoxY = contentY + 3 * scale + 1;

    // Draw pet area with border
    drawPetArea(display, petBoxX, petBoxY, petBoxW, petBoxH, scale);

    // Draw the pet at petX position within box
    int16_t walkRange = petBoxW - petW - 4 * scale; // Available walk space
#ifdef SENSECAP_INDICATOR
    int16_t petDrawX = petBoxX + 2 * scale + ((petX * walkRange) / 60); // Match maxWalkX=60
#elif defined(T_DECK)
    int16_t petDrawX = petBoxX + 2 * scale + ((petX * walkRange) / 35); // Match maxWalkX=35
#else
    int16_t petDrawX = petBoxX + 2 * scale + ((petX * walkRange) / 20); // Match maxWalkX=20
#endif
    int16_t petDrawY = petBoxY + (petBoxH - petH) / 2;
    drawPet(display, petDrawX, petDrawY, scale);

    // Stats on the right side - align with top of pet box (shifted down 1px with pet box)
    const int16_t statsX = petBoxX + petBoxW + 3 * scale;
    drawStats(display, statsX, petBoxY - 3, screenW - statsX);

    // Row below pet and stats: Traffic rate with speedometer, msg/hr, hops, and time
    // Shifted up 1px
    int16_t lastY = petBoxY + petBoxH + (scale > 1 ? 3 : -1);
    char buf[48];

    // Calculate icon scale based on font size
    uint8_t iconScale = (FONT_HEIGHT_SMALL >= 16) ? 2 : 1;
    int16_t iconW = speed_icon_width * iconScale;
    int16_t iconH = speed_icon_height * iconScale;
    int16_t textOffset = iconW + 2;

    // Get messages per minute and select appropriate speedometer icon
    uint16_t msgPerMin = getMessagesPerMinute();
    const uint8_t *speedIcon = getSpeedIcon(msgPerMin);

    // Draw speedometer icon (shifted up 1px for alignment)
    drawXbmScaled(display, x, lastY + (FONT_HEIGHT_SMALL - iconH) / 2 - 1, speed_icon_width, speed_icon_height, speedIcon,
                  iconScale);

    // Format: "5/m [hops] Type (time)" - messages per minute, hops, type, time since last
    if (lastMessageTime > 0) {
        uint32_t elapsed = (millis() - lastMessageTime) / 1000; // seconds
        char timeBuf[8];
        if (elapsed < 60) {
            snprintf(timeBuf, sizeof(timeBuf), "%lus", (unsigned long)elapsed);
        } else if (elapsed < 3600) {
            snprintf(timeBuf, sizeof(timeBuf), "%lum", (unsigned long)(elapsed / 60));
        } else {
            snprintf(timeBuf, sizeof(timeBuf), "%luh", (unsigned long)(elapsed / 3600));
        }
        snprintf(buf, sizeof(buf), "%u/m [%u] %s (%s)", msgPerMin, lastMessageHops, getMessageTypeName(lastMessageType), timeBuf);
    } else {
        snprintf(buf, sizeof(buf), "%u/m", msgPerMin);
    }
    display->drawString(x + textOffset, lastY + (iconScale > 1 ? 2 : 0), buf);

    // Status bars directly below Last (shifted up 1px with lastY)
    int16_t barY = lastY + FONT_HEIGHT_SMALL - 1 + (iconScale > 1 ? 4 : 0);
    int16_t barW = (screenW / 2) - 3;

    // Heart for happiness, EXP for experience
    drawStatusBarWithIcon(display, x, barY, barW, happiness, true, scale);
    uint8_t xpPercent = (xpForNextLevel > 0) ? (uint8_t)((experience * 100) / xpForNextLevel) : 0;
    drawStatusBarWithIcon(display, x + barW + 4, barY, barW, xpPercent, false, scale);

#if defined(SENSECAP_INDICATOR) || defined(T_DECK)
    // Draw received message log at bottom of screen (wide screen devices)
    // Wrap long lines with indent, show as many entries as fit
    int16_t logY = barY + (10 * scale) - 6; // Below the status bars, tighter gap
#ifdef T_DECK
    // Use smaller font for T-Deck to fit more lines
    display->setFont(FONT_SMALL_LOCAL);
    int16_t lineHeight = _fontHeight(FONT_SMALL_LOCAL) - 3;
#else
    display->setFont(FONT_SMALL);
    int16_t lineHeight = FONT_HEIGHT_SMALL;
#endif
    int16_t indentX = 24; // Indent for wrapped lines

    // Calculate available width in pixels
    int16_t availableWidth = screenW - x - 2;
    int16_t wrapWidth = availableWidth - indentX;

    // Maximum display lines based on device
#ifdef SENSECAP_INDICATOR
    const int16_t maxDisplayLines = 18;
#else
    const int16_t maxDisplayLines = 10;
#endif
    int16_t currentY = logY;

    // Draw entries from oldest to newest, wrapping as needed
    int16_t entriesToShow = msgLogCount;
    if (entriesToShow > MSG_LOG_LINES)
        entriesToShow = MSG_LOG_LINES;

    // Helper to get number of bytes in a UTF-8 codepoint starting at the given byte
    auto utf8CharLen = [](unsigned char c) -> int16_t {
        if ((c & 0x80) == 0)
            return 1; // ASCII
        if ((c & 0xE0) == 0xC0)
            return 2; // 2-byte sequence
        if ((c & 0xF0) == 0xE0)
            return 3; // 3-byte sequence
        if ((c & 0xF8) == 0xF0)
            return 4; // 4-byte sequence (emoji)
        return 1;     // Invalid, treat as 1
    };

    // Helper lambda to count lines needed for a message using pixel width (word-aware wrapping)
    auto countLinesForMsg = [&](const char *msg) -> int16_t {
        int16_t lines = 0;
        int16_t msgLen = strlen(msg);
        int16_t pos = 0;
        bool first = true;
        while (pos < msgLen) {
            int16_t maxW = first ? availableWidth : wrapWidth;
            // Find how many bytes fit, tracking last word boundary for clean wrap
            int16_t byteCount = 0;
            int16_t lastWordBreak = 0; // Byte count at last space
            char testBuf[128];
            int16_t testPos = pos;
            while (testPos < msgLen && byteCount < (int16_t)sizeof(testBuf) - 4) {
                int16_t charLen = utf8CharLen((unsigned char)msg[testPos]);
                if (testPos + charLen > msgLen)
                    charLen = msgLen - testPos;
                if (byteCount + charLen >= (int16_t)sizeof(testBuf))
                    break;
                // Check for word boundary (space) before adding
                if (msg[testPos] == ' ' && byteCount > 0)
                    lastWordBreak = byteCount;
                memcpy(testBuf + byteCount, msg + testPos, charLen);
                byteCount += charLen;
                testBuf[byteCount] = '\0';
                if (display->getStringWidth(testBuf) > maxW) {
                    byteCount -= charLen;
                    // Back up to last word break if we have one
                    if (lastWordBreak > 0)
                        byteCount = lastWordBreak;
                    break;
                }
                testPos += charLen;
            }
            if (byteCount == 0)
                byteCount = utf8CharLen((unsigned char)msg[pos]);
            pos += byteCount;
            // Skip leading space on next line
            while (pos < msgLen && msg[pos] == ' ')
                pos++;
            lines++;
            first = false;
        }
        return lines > 0 ? lines : 1;
    };

    // First pass: figure out how many entries fit in 18 lines
    int16_t startEntry = 0;
    int16_t totalLines = 0;
    for (int16_t i = 0; i < entriesToShow; i++) {
        int16_t idx = (msgLogHead - entriesToShow + i + MSG_LOG_LINES) % MSG_LOG_LINES;
        totalLines += countLinesForMsg(msgLogBuffer[idx]);
    }

    // If too many lines, skip older entries
    if (totalLines > maxDisplayLines) {
        totalLines = 0;
        for (startEntry = entriesToShow - 1; startEntry >= 0; startEntry--) {
            int16_t idx = (msgLogHead - entriesToShow + startEntry + MSG_LOG_LINES) % MSG_LOG_LINES;
            int16_t linesNeeded = countLinesForMsg(msgLogBuffer[idx]);
            if (totalLines + linesNeeded > maxDisplayLines) {
                startEntry++;
                break;
            }
            totalLines += linesNeeded;
        }
        if (startEntry < 0)
            startEntry = 0;
    }

    // Second pass: actually draw with pixel-accurate wrapping
    int16_t linesDrawn = 0;
    for (int16_t i = startEntry; i < entriesToShow && linesDrawn < maxDisplayLines; i++) {
        int16_t idx = (msgLogHead - entriesToShow + i + MSG_LOG_LINES) % MSG_LOG_LINES;
        bool isNewest = (i == entriesToShow - 1);

        const char *msg = msgLogBuffer[idx];
        int16_t msgLen = strlen(msg);
        int16_t pos = 0;
        bool firstLine = true;

        while (pos < msgLen && linesDrawn < maxDisplayLines) {
            int16_t drawX = firstLine ? x : (x + indentX);
            int16_t maxW = firstLine ? availableWidth : wrapWidth;

            // Find how many bytes fit, tracking last word boundary for clean wrap
            int16_t byteCount = 0;
            int16_t lastWordBreak = 0;
            char lineBuf[128];
            int16_t testPos = pos;
            while (testPos < msgLen && byteCount < (int16_t)sizeof(lineBuf) - 4) {
                int16_t charLen = utf8CharLen((unsigned char)msg[testPos]);
                if (testPos + charLen > msgLen)
                    charLen = msgLen - testPos;
                if (byteCount + charLen >= (int16_t)sizeof(lineBuf))
                    break;
                // Check for word boundary (space) before adding
                if (msg[testPos] == ' ' && byteCount > 0)
                    lastWordBreak = byteCount;
                memcpy(lineBuf + byteCount, msg + testPos, charLen);
                byteCount += charLen;
                lineBuf[byteCount] = '\0';
                if (display->getStringWidth(lineBuf) > maxW) {
                    byteCount -= charLen;
                    // Back up to last word break if we have one
                    if (lastWordBreak > 0)
                        byteCount = lastWordBreak;
                    break;
                }
                testPos += charLen;
            }
            if (byteCount == 0)
                byteCount = utf8CharLen((unsigned char)msg[pos]);
            memcpy(lineBuf, msg + pos, byteCount);
            lineBuf[byteCount] = '\0';

            // Draw with bold effect for newest entry
            if (isNewest) {
                display->drawString(drawX, currentY, lineBuf);
                display->drawString(drawX + 1, currentY, lineBuf);
            } else {
                display->drawString(drawX, currentY, lineBuf);
            }

            pos += byteCount;
            // Skip leading space on next line
            while (pos < msgLen && msg[pos] == ' ')
                pos++;
            currentY += lineHeight;
            linesDrawn++;
            firstLine = false;
        }
    }
#endif
}

// Helper function to draw XBM bitmap scaled 2x (for high-resolution screens)
void PetModule::drawXbmScaled(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *xbm,
                              uint8_t scale)
{
    int16_t bytesPerRow = (width + 7) / 8;

    for (int16_t row = 0; row < height; row++) {
        for (int16_t col = 0; col < width; col++) {
            int16_t byteIndex = row * bytesPerRow + (col / 8);
            int16_t bitIndex = col % 8;
            uint8_t byte = pgm_read_byte(&xbm[byteIndex]);
            bool pixel = (byte >> bitIndex) & 1;

            if (pixel) {
                // Draw scaled pixel (scale x scale block)
                for (uint8_t sy = 0; sy < scale; sy++) {
                    for (uint8_t sx = 0; sx < scale; sx++) {
                        display->setPixel(x + col * scale + sx, y + row * scale + sy);
                    }
                }
            }
        }
    }
}

// Helper function to draw XBM bitmap flipped horizontally (with optional scale)
void PetModule::drawXbmFlipped(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t *xbm,
                               uint8_t scale)
{
    // XBM format: each row is stored as bytes, LSB first within each byte
    // We need to draw pixels from right to left for horizontal flip
    int16_t bytesPerRow = (width + 7) / 8;

    for (int16_t row = 0; row < height; row++) {
        for (int16_t col = 0; col < width; col++) {
            // Calculate which byte and bit this pixel is in
            int16_t byteIndex = row * bytesPerRow + (col / 8);
            int16_t bitIndex = col % 8;

            // Read the pixel (XBM is LSB first)
            uint8_t byte = pgm_read_byte(&xbm[byteIndex]);
            bool pixel = (byte >> bitIndex) & 1;

            if (pixel) {
                // Draw at flipped X position with scale
                int16_t flippedCol = width - 1 - col;
                for (uint8_t sy = 0; sy < scale; sy++) {
                    for (uint8_t sx = 0; sx < scale; sx++) {
                        display->setPixel(x + flippedCol * scale + sx, y + row * scale + sy);
                    }
                }
            }
        }
    }
}

void PetModule::drawPet(OLEDDisplay *display, int16_t x, int16_t y, uint8_t scale)
{
    const uint8_t *frameData = nullptr;
    bool flipHorizontal = false;

    // Select the appropriate animation frame
    switch (currentAnimation) {
    case PetAnimation::IDLE:
        frameData = petIdleFrames[animationFrame % PET_IDLE_FRAMES];
        break;
    case PetAnimation::WALKING:
        frameData = petWalkFrames[animationFrame % PET_WALK_FRAMES];
        // Flip sprite when walking left
        flipHorizontal = (petDirection < 0);
        break;
    case PetAnimation::HAPPY_BOUNCE:
        frameData = petHappyFrames[animationFrame % PET_HAPPY_FRAMES];
        break;
    case PetAnimation::SLEEPING:
        frameData = petSleepFrames[animationFrame % PET_SLEEP_FRAMES];
        break;
    case PetAnimation::EXCITED:
        frameData = petExcitedFrames[animationFrame % PET_EXCITED_FRAMES];
        break;
    case PetAnimation::EATING:
        frameData = petEatingFrames[animationFrame % PET_EATING_FRAMES];
        break;
    case PetAnimation::PLAYING:
        frameData = petPlayingFrames[animationFrame % PET_PLAYING_FRAMES];
        break;
    case PetAnimation::ALERT:
        frameData = petAlertFrames[animationFrame % PET_ALERT_FRAMES];
        break;
    case PetAnimation::SAD:
        frameData = petSadFrames[animationFrame % PET_SAD_FRAMES];
        break;
    case PetAnimation::READING:
        frameData = petReadingFrames[animationFrame % PET_READING_FRAMES];
        break;
    case PetAnimation::LOOKING:
        frameData = petLookingFrames[animationFrame % PET_LOOKING_FRAMES];
        break;
    case PetAnimation::WAVING:
        frameData = petWavingFrames[animationFrame % PET_WAVING_FRAMES];
        break;
    case PetAnimation::THINKING:
        frameData = petThinkingFrames[animationFrame % PET_THINKING_FRAMES];
        break;
    case PetAnimation::SCARED:
        frameData = petScaredFrames[animationFrame % PET_SCARED_FRAMES];
        break;
    case PetAnimation::HOPPING:
        frameData = petHoppingFrames[animationFrame % PET_HOPPING_FRAMES];
        // Flip when moving left
        flipHorizontal = (petDirection < 0);
        break;
    case PetAnimation::SCRATCHING:
        frameData = petScratchingFrames[animationFrame % PET_SCRATCHING_FRAMES];
        break;
    case PetAnimation::DANCING:
        frameData = petDancingFrames[animationFrame % PET_DANCING_FRAMES];
        break;
    case PetAnimation::YAWNING:
        frameData = petYawningFrames[animationFrame % PET_YAWNING_FRAMES];
        break;
    case PetAnimation::SNIFFING:
        frameData = petSniffingFrames[animationFrame % PET_SNIFFING_FRAMES];
        break;
    }

    if (frameData) {
        if (flipHorizontal) {
            drawXbmFlipped(display, x, y, PET_FRAME_WIDTH, PET_FRAME_HEIGHT, frameData, scale);
        } else if (scale > 1) {
            drawXbmScaled(display, x, y, PET_FRAME_WIDTH, PET_FRAME_HEIGHT, frameData, scale);
        } else {
            display->drawXbm(x, y, PET_FRAME_WIDTH, PET_FRAME_HEIGHT, frameData);
        }
    }
}

void PetModule::drawStats(OLEDDisplay *display, int16_t x, int16_t y, int16_t width)
{
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    char buf[24];
    // Calculate icon scale based on font size
    uint8_t iconScale = (FONT_HEIGHT_SMALL >= 16) ? 2 : 1;
    // Add extra padding between rows for high-res screens
    int16_t lineH = FONT_HEIGHT_SMALL - 4 + (iconScale > 1 ? 4 : 0);
    int16_t iconW = messages_icon_width * iconScale;
    int16_t iconH = messages_icon_height * iconScale;
    int16_t textOffset = iconW + 2;                // Gap between icon and text
    int16_t textYOffset = (iconScale > 1) ? 2 : 0; // Move text down 2px for high-res

    // Row 1: Messages received (envelope icon) - icon shifted down 1 pixel
    drawXbmScaled(display, x, y + 1 + (lineH - iconH) / 2, messages_icon_width, messages_icon_height, messages_icon, iconScale);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)messagesReceived);
    display->drawString(x + textOffset, y + textYOffset, buf);

    // Row 2: Nodes: active/total (nodes icon) - icon shifted down 1 pixel
    uint16_t activeNodes = 0;
    uint16_t totalNodes = 0;
    if (nodeStatus) {
        activeNodes = nodeStatus->getNumOnline();
        totalNodes = nodeStatus->getNumTotal();
    }
    drawXbmScaled(display, x, y + 1 + lineH + (lineH - iconH) / 2, nodes_icon_width, nodes_icon_height, nodes_icon, iconScale);
    snprintf(buf, sizeof(buf), "%u/%u", activeNodes, totalNodes);
    display->drawString(x + textOffset, y + lineH + textYOffset, buf);

    // Row 3: Uptime (clock icon) - icon shifted down 1 pixel
    uint32_t totalMins = getUptimeMinutes();
    uint32_t uptimeDays = totalMins / 1440; // 1440 minutes per day
    uint32_t uptimeHours = (totalMins % 1440) / 60;
    uint32_t uptimeMins = totalMins % 60;
    drawXbmScaled(display, x, y + 1 + lineH * 2 + (lineH - iconH) / 2, uptime_icon_width, uptime_icon_height, uptime_icon,
                  iconScale);
    if (uptimeDays > 0) {
        snprintf(buf, sizeof(buf), "%lud%luh", (unsigned long)uptimeDays, (unsigned long)uptimeHours);
    } else if (uptimeHours > 0) {
        snprintf(buf, sizeof(buf), "%luh%02lum", (unsigned long)uptimeHours, (unsigned long)uptimeMins);
    } else {
        snprintf(buf, sizeof(buf), "%lum", (unsigned long)uptimeMins);
    }
    display->drawString(x + textOffset, y + lineH * 2 + textYOffset, buf);
}

void PetModule::drawStatusBarWithIcon(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, uint8_t percent, bool isHeart,
                                      uint8_t scale)
{
    // Draw tiny icon (heart or exp)
    int16_t iconW;
    int16_t iconH;

    if (isHeart) {
        // Draw tiny heart icon
        iconW = heart_icon_width;
        iconH = heart_icon_height;
        if (scale > 1) {
            drawXbmScaled(display, x, y, iconW, iconH, heart_icon, scale);
        } else {
            display->drawXbm(x, y, iconW, iconH, heart_icon);
        }
    } else {
        // Draw EXP icon
        iconW = exp_icon_width;
        iconH = exp_icon_height;
        if (scale > 1) {
            drawXbmScaled(display, x, y, iconW, iconH, exp_icon, scale);
        } else {
            display->drawXbm(x, y, iconW, iconH, exp_icon);
        }
    }

    // Scale icon dimensions for bar positioning
    int16_t scaledIconW = iconW * scale;
    int16_t scaledIconH = iconH * scale;

    // Draw thin bar (scaled height)
    int16_t barX = x + scaledIconW + 2 * scale;
    int16_t barW = width - scaledIconW - 2 * scale;
    int16_t barH = 5 * scale;

    display->drawRect(barX, y, barW, barH);

    // Remove corner pixels for rounded effect
    display->setColor(BLACK);
    display->setPixel(barX, y);
    display->setPixel(barX + barW - 1, y);
    display->setPixel(barX, y + barH - 1);
    display->setPixel(barX + barW - 1, y + barH - 1);
    display->setColor(WHITE);

    // Draw filled portion
    int16_t fillW = ((barW - 2) * percent) / 100;
    if (fillW > 0) {
        display->fillRect(barX + 1, y + 1, fillW, barH - 2);
    }
}

void PetModule::drawPetArea(OLEDDisplay *display, int16_t x, int16_t y, int16_t width, int16_t height, uint8_t scale)
{
    // Draw a box for the pet area (double thick for high resolution)
    display->drawRect(x, y, width, height);
    if (scale > 1) {
        display->drawRect(x + 1, y + 1, width - 2, height - 2);
    }

    // Remove corner pixels for rounded effect
    display->setColor(BLACK);
    display->setPixel(x, y);
    display->setPixel(x + width - 1, y);
    display->setPixel(x, y + height - 1);
    display->setPixel(x + width - 1, y + height - 1);
    if (scale > 1) {
        // Additional corner cleanup for double-thick border
        display->setPixel(x + 1, y);
        display->setPixel(x, y + 1);
        display->setPixel(x + width - 2, y);
        display->setPixel(x + width - 1, y + 1);
        display->setPixel(x, y + height - 2);
        display->setPixel(x + 1, y + height - 1);
        display->setPixel(x + width - 1, y + height - 2);
        display->setPixel(x + width - 2, y + height - 1);
    }
    display->setColor(WHITE);
}

void PetModule::drawMoodIndicator(OLEDDisplay *display, int16_t x, int16_t y, uint8_t scale)
{
#ifndef EXCLUDE_EMOJI
    // Draw mood indicator using emote bitmaps
    const unsigned char *moodBitmap = nullptr;
    int moodWidth = 16;
    int moodHeight = 16;

    switch (currentMood) {
    case PetMood::HAPPY:
        moodBitmap = graphics::grinning_smiling_eyes_2;
        moodWidth = grinning_smiling_eyes_2_width;
        moodHeight = grinning_smiling_eyes_2_height;
        break;
    case PetMood::NEUTRAL:
        moodBitmap = graphics::slightly_smiling;
        moodWidth = slightly_smiling_width;
        moodHeight = slightly_smiling_height;
        break;
    case PetMood::SAD:
        moodBitmap = graphics::loudly_crying_face;
        moodWidth = loudly_crying_face_width;
        moodHeight = loudly_crying_face_height;
        break;
    case PetMood::EXCITED:
        moodBitmap = graphics::grinning;
        moodWidth = grinning_width;
        moodHeight = grinning_height;
        break;
    case PetMood::SLEEPING:
        moodBitmap = graphics::sleeping_face;
        moodWidth = sleeping_face_width;
        moodHeight = sleeping_face_height;
        break;
    case PetMood::HUNGRY:
        moodBitmap = graphics::weary_face;
        moodWidth = weary_face_width;
        moodHeight = weary_face_height;
        break;
    case PetMood::ALERT:
        moodBitmap = graphics::open_mouth;
        moodWidth = open_mouth_width;
        moodHeight = open_mouth_height;
        break;
    }

    if (moodBitmap) {
        if (scale > 1) {
            drawXbmScaled(display, x, y, moodWidth, moodHeight, moodBitmap, scale);
        } else {
            display->drawXbm(x, y, moodWidth, moodHeight, moodBitmap);
        }
    }
#else
    // Fallback to ASCII if emojis are excluded
    const char *moodStr = "";

    switch (currentMood) {
    case PetMood::HAPPY:
        moodStr = ":)";
        break;
    case PetMood::NEUTRAL:
        moodStr = ":|";
        break;
    case PetMood::SAD:
        moodStr = ":(";
        break;
    case PetMood::EXCITED:
        moodStr = ":D";
        break;
    case PetMood::SLEEPING:
        moodStr = "Zz";
        break;
    case PetMood::HUNGRY:
        moodStr = ":O";
        break;
    case PetMood::ALERT:
        moodStr = "!!";
        break;
    }

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(x, y, moodStr);
#endif
}

const char *PetModule::getMessageTypeName(LastMessageType type)
{
    switch (type) {
    case LastMessageType::TEXT:
        return "Text";
    case LastMessageType::TEXT_COMPRESSED:
        return "TextComp";
    case LastMessageType::POSITION:
        return "Position";
    case LastMessageType::TELEMETRY:
        return "Telemetry";
    case LastMessageType::NODEINFO:
        return "NodeInfo";
    case LastMessageType::ROUTING:
        return "Routing";
    case LastMessageType::ADMIN:
        return "Admin";
    case LastMessageType::WAYPOINT:
        return "Waypoint";
    case LastMessageType::TRACEROUTE:
        return "Traceroute";
    case LastMessageType::NEIGHBOR:
        return "Neighbor";
    case LastMessageType::STORE_FWD:
        return "Store&Fwd";
    case LastMessageType::STORE_FWD_PP:
        return "S&F++";
    case LastMessageType::RANGE_TEST:
        return "RangeTest";
    case LastMessageType::DETECTION:
        return "Detection";
    case LastMessageType::ALERT:
        return "Alert";
    case LastMessageType::PAXCOUNT:
        return "PaxCount";
    case LastMessageType::REMOTE_HW:
        return "RemoteHW";
    case LastMessageType::AUDIO:
        return "Audio";
    case LastMessageType::KEY_VERIFY:
        return "KeyVerify";
    case LastMessageType::REPLY:
        return "Reply";
    case LastMessageType::IP_TUNNEL:
        return "IPTunnel";
    case LastMessageType::NODE_STATUS:
        return "NodeStatus";
    case LastMessageType::SERIAL_PORT:
        return "Serial";
    case LastMessageType::ZPS:
        return "ZPS";
    case LastMessageType::SIMULATOR:
        return "Simulator";
    case LastMessageType::ATAK:
        return "ATAK";
    case LastMessageType::ATAK_FWD:
        return "ATAKFwd";
    case LastMessageType::MAP_REPORT:
        return "MapReport";
    case LastMessageType::POWERSTRESS:
        return "PwrStress";
    case LastMessageType::RETICULUM:
        return "Reticulum";
    case LastMessageType::CAYENNE:
        return "Cayenne";
    case LastMessageType::PRIVATE:
        return "Private";
    case LastMessageType::UNKNOWN:
        return "Unknown";
    case LastMessageType::NONE:
    default:
        return "None";
    }
}

#endif // HAS_SCREEN

#endif // !MESHTASTIC_EXCLUDE_PET
