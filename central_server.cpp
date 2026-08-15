// v6_watchdog: obrada CONNECTION_LOST alarma i prekid aktivne misije.
// v7_inspection: centralni server sa podrskom za INSPECTION tacke i INSPECTION_REPORT poruke.
// central_server.cpp
// v5_tcp_udp: centralni server ostaje TCP; kompatibilan je sa regionalnim v5 koji prima UDP telemetriju od dronova.
// Centralni server za autonomne dronove.
// Funkcionalnosti:
// - registracija regionalnih servera sa bazom i zonama
// - generisanje pravougaonih/kockastih kontura kao ruta
// - DELIVERY: izbor najbliže konture i izlazne tačke prema dostavnoj tački
// - maksimalno 3 drona po istoj ruti, visinski slotovi po 2m
// - čuvanje statusa dronova, misija i alarma u SQLite bazi
// - LOW_BATTERY alarm automatski generiše RETURN_TO_BASE komandu
// - prioriteti misija i preuzimanje slota od misije nižeg prioriteta

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>

#include <sqlite3.h>
#include "json/json.h"
#include "sqlite3_wrapper.h"

using boost::asio::ip::tcp;
using json = nlohmann::json;
namespace sqlite = sqlite3_wrapper;

std::unique_ptr<sqlite::db> g_db;
std::mutex db_mutex;

const int DEFAULT_MAX_DRONES_PER_ROUTE = 3;
const int DEFAULT_VERTICAL_SEPARATION_M = 2;

struct LocalPoint
{
    double north_m;
    double east_m;
};

struct GeoPoint
{
    double lat;
    double lon;
};

struct ZoneInfo
{
    bool found;
    double center_lat;
    double center_lon;
    int radius_m;
    int contours;

    ZoneInfo() : found(false), center_lat(0), center_lon(0), radius_m(0), contours(0) {}
};

struct RouteInfo
{
    bool found;
    std::string route_type;
    int contour_level;
    int half_size_m;
    double center_lat;
    double center_lon;
    int max_drones;
    int vertical_separation;

    RouteInfo()
        : found(false), contour_level(0), half_size_m(0),
          center_lat(0), center_lon(0),
          max_drones(DEFAULT_MAX_DRONES_PER_ROUTE),
          vertical_separation(DEFAULT_VERTICAL_SEPARATION_M)
    {
    }
};

struct PreemptedMission
{
    bool found;
    std::string mission_id;
    std::string drone_uri;
    int mission_priority;
    int altitude_slot;
    int altitude;

    PreemptedMission()
        : found(false),
          mission_priority(0),
          altitude_slot(-1),
          altitude(0)
    {
    }
};

void exec_sql(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    g_db->execute(sql);
}

void init_database()
{
    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS regional_servers (
            region_id TEXT PRIMARY KEY,
            base_lat REAL,
            base_lon REAL,
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS zones (
            region_id TEXT,
            zone_id TEXT,
            center_lat REAL,
            center_lon REAL,
            radius_m INTEGER,
            contours INTEGER,
            PRIMARY KEY(region_id, zone_id)
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS zone_routes (
            region_id TEXT,
            zone_id TEXT,
            route_id TEXT,
            route_type TEXT,
            contour_level INTEGER,
            half_size_m INTEGER,
            max_drones INTEGER,
            vertical_separation INTEGER,
            PRIMARY KEY(region_id, zone_id, route_id)
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS drones (
            drone_uri TEXT PRIMARY KEY,
            region_id TEXT,
            battery INTEGER,
            status TEXT,
            lat REAL,
            lon REAL,
            altitude INTEGER,
            route_id TEXT,
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS missions (
            mission_id TEXT PRIMARY KEY,
            drone_uri TEXT,
            region_id TEXT,
            mission_type TEXT,
            mission_priority INTEGER,
            zone TEXT,
            route_id TEXT,
            altitude INTEGER,
            altitude_slot INTEGER,
            delivery_lat REAL,
            delivery_lon REAL,
            exit_lat REAL,
            exit_lon REAL,
            status TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            finished_at DATETIME
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS inspection_reports (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            mission_id TEXT,
            drone_uri TEXT,
            point_id TEXT,
            lat REAL,
            lon REAL,
            result TEXT,
            received_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(mission_id, point_id)
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS alarms (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            drone_uri TEXT,
            alarm_type TEXT,
            message TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )");
}

LocalPoint latlon_to_offset(double center_lat, double center_lon, double lat, double lon)
{
    const double PI = 3.14159265358979323846;

    double meters_per_deg_lat = 111320.0;
    double meters_per_deg_lon = 111320.0 * std::cos(center_lat * PI / 180.0);

    LocalPoint p;
    p.north_m = (lat - center_lat) * meters_per_deg_lat;
    p.east_m = (lon - center_lon) * meters_per_deg_lon;

    return p;
}

GeoPoint offset_to_latlon(double center_lat, double center_lon, double north_m, double east_m)
{
    const double PI = 3.14159265358979323846;

    double meters_per_deg_lat = 111320.0;
    double meters_per_deg_lon = 111320.0 * std::cos(center_lat * PI / 180.0);

    GeoPoint p;
    p.lat = center_lat + north_m / meters_per_deg_lat;
    p.lon = center_lon + east_m / meters_per_deg_lon;

    return p;
}

LocalPoint closest_point_on_square_contour(double delivery_north,
                                           double delivery_east,
                                           double half_size_m)
{
    LocalPoint exit_point;

    double abs_north = std::abs(delivery_north);
    double abs_east = std::abs(delivery_east);

    if (abs_east >= abs_north)
    {
        // Najbliža je lijeva/desna ivica konture.
        exit_point.east_m = (delivery_east >= 0) ? half_size_m : -half_size_m;
        exit_point.north_m = std::max(-half_size_m, std::min(half_size_m, delivery_north));
    }
    else
    {
        // Najbliža je gornja/donja ivica konture.
        exit_point.north_m = (delivery_north >= 0) ? half_size_m : -half_size_m;
        exit_point.east_m = std::max(-half_size_m, std::min(half_size_m, delivery_east));
    }

    return exit_point;
}

int choose_delivery_contour(double delivery_north,
                            double delivery_east,
                            int zone_radius_m,
                            int contours)
{
    double distance_square = std::max(std::abs(delivery_north), std::abs(delivery_east));

    if (distance_square > zone_radius_m)
    {
        return -1;
    }

    double step = static_cast<double>(zone_radius_m) / contours;
    int level = static_cast<int>(std::round(distance_square / step));

    if (level < 1)
        level = 1;

    if (level > contours)
        level = contours;

    return level;
}

int get_mission_priority(const std::string& mission_type)
{
    if (mission_type == "MONITORING")
        return 4;

    if (mission_type == "DELIVERY")
        return 3;

    if (mission_type == "INSPECTION")
        return 2;

    if (mission_type == "TEST_FLIGHT")
        return 1;

    return 0;
}

PreemptedMission find_lowest_priority_active_mission(const std::string& region_id,
                                                     const std::string& zone,
                                                     const std::string& route_id)
{
    PreemptedMission result;

    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(R"(
        SELECT mission_id
        FROM missions
        WHERE status = 'ACTIVE'
          AND region_id = ?
          AND zone = ?
          AND route_id = ?
        ORDER BY mission_priority ASC, created_at ASC
        LIMIT 1;
    )");

    stmt.execute(region_id, zone, route_id);

    if (!stmt.fetch(result.mission_id))
    {
        return result;
    }

    auto s_drone = g_db->prepare("SELECT drone_uri FROM missions WHERE mission_id = ?;");
    s_drone.execute(result.mission_id);
    s_drone.fetch(result.drone_uri);

    auto s_priority = g_db->prepare("SELECT mission_priority FROM missions WHERE mission_id = ?;");
    s_priority.execute(result.mission_id);
    s_priority.fetch(result.mission_priority);

    auto s_slot = g_db->prepare("SELECT altitude_slot FROM missions WHERE mission_id = ?;");
    s_slot.execute(result.mission_id);
    s_slot.fetch(result.altitude_slot);

    auto s_alt = g_db->prepare("SELECT altitude FROM missions WHERE mission_id = ?;");
    s_alt.execute(result.mission_id);
    s_alt.fetch(result.altitude);

    result.found = true;
    return result;
}

void mark_mission_preempted(const std::string& old_mission_id,
                            const std::string& new_mission_id)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(R"(
        UPDATE missions
        SET status = 'PREEMPTED_BY_HIGHER_PRIORITY',
            finished_at = CURRENT_TIMESTAMP
        WHERE mission_id = ?;
    )");

    stmt.execute(old_mission_id);

    std::cout << "[CENTRAL] Misija " << old_mission_id
              << " prekinuta zbog misije višeg prioriteta "
              << new_mission_id << std::endl;
}

bool zone_exists(const std::string& region_id, const std::string& zone)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(
        "SELECT COUNT(*) FROM zones WHERE region_id = ? AND zone_id = ?;"
    );

    stmt.execute(region_id, zone);

    int count = 0;
    if (stmt.fetch(count))
    {
        return count > 0;
    }

    return false;
}

ZoneInfo get_zone_info(const std::string& region_id, const std::string& zone)
{
    ZoneInfo result;

    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(
        "SELECT center_lat FROM zones WHERE region_id = ? AND zone_id = ?;"
    );
    stmt.execute(region_id, zone);
    if (!stmt.fetch(result.center_lat))
        return result;

    stmt = g_db->prepare(
        "SELECT center_lon FROM zones WHERE region_id = ? AND zone_id = ?;"
    );
    stmt.execute(region_id, zone);
    stmt.fetch(result.center_lon);

    stmt = g_db->prepare(
        "SELECT radius_m FROM zones WHERE region_id = ? AND zone_id = ?;"
    );
    stmt.execute(region_id, zone);
    stmt.fetch(result.radius_m);

    stmt = g_db->prepare(
        "SELECT contours FROM zones WHERE region_id = ? AND zone_id = ?;"
    );
    stmt.execute(region_id, zone);
    stmt.fetch(result.contours);

    result.found = true;
    return result;
}

RouteInfo get_route_info(const std::string& region_id,
                         const std::string& zone,
                         const std::string& route_id)
{
    RouteInfo result;

    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(
        "SELECT route_type FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;"
    );
    stmt.execute(region_id, zone, route_id);
    if (!stmt.fetch(result.route_type))
        return result;

    stmt = g_db->prepare(
        "SELECT contour_level FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;"
    );
    stmt.execute(region_id, zone, route_id);
    stmt.fetch(result.contour_level);

    stmt = g_db->prepare(
        "SELECT half_size_m FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;"
    );
    stmt.execute(region_id, zone, route_id);
    stmt.fetch(result.half_size_m);

    stmt = g_db->prepare(
        "SELECT max_drones FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;"
    );
    stmt.execute(region_id, zone, route_id);
    stmt.fetch(result.max_drones);

    stmt = g_db->prepare(
        "SELECT vertical_separation FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;"
    );
    stmt.execute(region_id, zone, route_id);
    stmt.fetch(result.vertical_separation);

    result.found = true;
    return result;
}

bool assign_altitude_slot(const std::string& region_id,
                          const std::string& zone,
                          const std::string& route_id,
                          const RouteInfo& route,
                          int base_altitude,
                          int& assigned_altitude,
                          int& assigned_slot)
{
    std::vector<bool> used_slots(route.max_drones, false);

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto stmt = g_db->prepare(R"(
            SELECT altitude_slot
            FROM missions
            WHERE status = 'ACTIVE'
              AND region_id = ?
              AND zone = ?
              AND route_id = ?;
        )");

        stmt.execute(region_id, zone, route_id);

        int slot = 0;
        while (stmt.fetch(slot))
        {
            if (slot >= 0 && slot < route.max_drones)
            {
                used_slots[slot] = true;
            }
        }
    }

    for (int i = 0; i < route.max_drones; i++)
    {
        if (!used_slots[i])
        {
            assigned_slot = i;
            assigned_altitude = base_altitude + i * route.vertical_separation;
            return true;
        }
    }

    return false;
}

void handle_region_register(const json& msg)
{
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    double base_lat = msg.value("BASE_LAT", 0.0);
    double base_lon = msg.value("BASE_LON", 0.0);

    json zones = msg.value("ZONES", json::array());

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto region_stmt = g_db->prepare(R"(
            INSERT OR REPLACE INTO regional_servers(region_id, base_lat, base_lon, last_seen)
            VALUES (?, ?, ?, CURRENT_TIMESTAMP);
        )");
        region_stmt.execute(region_id, base_lat, base_lon);

        auto dz = g_db->prepare("DELETE FROM zones WHERE region_id = ?;");
        dz.execute(region_id);

        auto dr = g_db->prepare("DELETE FROM zone_routes WHERE region_id = ?;");
        dr.execute(region_id);

        auto zone_stmt = g_db->prepare(R"(
            INSERT OR REPLACE INTO zones
            (region_id, zone_id, center_lat, center_lon, radius_m, contours)
            VALUES (?, ?, ?, ?, ?, ?);
        )");

        auto route_stmt = g_db->prepare(R"(
            INSERT OR REPLACE INTO zone_routes
            (region_id, zone_id, route_id, route_type, contour_level, half_size_m, max_drones, vertical_separation)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?);
        )");

        for (const auto& z : zones)
        {
            std::string zone_id = z.value("ZONE_ID", "");
            double center_lat = z.value("CENTER_LAT", 0.0);
            double center_lon = z.value("CENTER_LON", 0.0);
            int radius_m = z.value("RADIUS_M", 1000);
            int contours = z.value("CONTOURS", 4);

            if (zone_id.empty())
                continue;

            zone_stmt.execute(region_id, zone_id, center_lat, center_lon, radius_m, contours);

            for (int level = 1; level <= contours; level++)
            {
                std::string route_id = zone_id + "_K" + std::to_string(level);
                int half_size_m = (radius_m * level) / contours;

                route_stmt.execute(region_id, zone_id, route_id, "CONTOUR",
                                   level, half_size_m,
                                   DEFAULT_MAX_DRONES_PER_ROUTE,
                                   DEFAULT_VERTICAL_SEPARATION_M);
            }

            std::string connector_id = zone_id + "_DIAGONAL";
            route_stmt.execute(region_id, zone_id, connector_id, "CONNECTOR",
                               0, radius_m,
                               1,
                               DEFAULT_VERTICAL_SEPARATION_M);
        }
    }

    std::cout << "[CENTRAL] Registrovan region " << region_id
              << " | baza: " << base_lat << ", " << base_lon
              << " | broj zona: " << zones.size() << std::endl;
}

void handle_drone_status(const json& msg)
{
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    int battery = msg.value("BATTERY", -1);
    std::string status = msg.value("STATUS", "UNKNOWN");
    double lat = msg.value("LAT", 0.0);
    double lon = msg.value("LON", 0.0);
    int altitude = msg.value("ALTITUDE", 0);
    std::string route_id = msg.value("ROUTE_ID", "");

    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(R"(
        INSERT OR REPLACE INTO drones
        (drone_uri, region_id, battery, status, lat, lon, altitude, route_id, last_seen)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
    )");

    stmt.execute(drone_uri, region_id, battery, status, lat, lon, altitude, route_id);

    std::cout << "[CENTRAL] Status drona: " << drone_uri
              << " | " << status
              << " | battery=" << battery << "%" << std::endl;
}

json handle_alarm(const json& msg)
{
    json response;

    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    std::string alarm_type = msg.value("ALARM_TYPE", "UNKNOWN_ALARM");
    std::string message = msg.value("MESSAGE", "");

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto stmt = g_db->prepare(R"(
            INSERT INTO alarms(drone_uri, alarm_type, message)
            VALUES (?, ?, ?);
        )");

        stmt.execute(drone_uri, alarm_type, message);
    }

    std::cout << "[CENTRAL] Alarm za " << drone_uri
              << " | " << alarm_type << std::endl;

    if (alarm_type == "CONNECTION_LOST")
    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto drone_stmt = g_db->prepare(R"(
            UPDATE drones
            SET status = 'CONNECTION_LOST'
            WHERE drone_uri = ?;
        )");
        drone_stmt.execute(drone_uri);

        // Dron koji je izgubio vezu ne smije zadrzati aktivnu rutu/visinski slot.
        auto mission_stmt = g_db->prepare(R"(
            UPDATE missions
            SET status = 'ABORTED_CONNECTION_LOST',
                finished_at = CURRENT_TIMESTAMP
            WHERE drone_uri = ?
              AND status = 'ACTIVE';
        )");
        mission_stmt.execute(drone_uri);

        response["TYPE"] = "ACK_ALARM";
        response["MESSAGE"] = "CONNECTION_LOST_SAVED";

        std::cout << "[CENTRAL] Dron " << drone_uri
                  << " oznacen kao CONNECTION_LOST; aktivna misija je prekinuta."
                  << std::endl;

        return response;
    }

    if (alarm_type == "LOW_BATTERY")
    {
        double base_lat = 0.0;
        double base_lon = 0.0;

        {
            std::lock_guard<std::mutex> lock(db_mutex);

            auto stmt_lat = g_db->prepare(R"(
                SELECT base_lat
                FROM regional_servers
                WHERE region_id = ?;
            )");

            stmt_lat.execute(region_id);

            if (!stmt_lat.fetch(base_lat))
            {
                response["TYPE"] = "ERROR";
                response["MESSAGE"] = "BASE_NOT_FOUND_FOR_REGION";
                return response;
            }

            auto stmt_lon = g_db->prepare(R"(
                SELECT base_lon
                FROM regional_servers
                WHERE region_id = ?;
            )");

            stmt_lon.execute(region_id);
            stmt_lon.fetch(base_lon);

            // Dron prekida aktivnu misiju, pa se ruta i visinski slot oslobađaju.
            auto mission_stmt = g_db->prepare(R"(
                UPDATE missions
                SET status = 'ABORTED_LOW_BATTERY',
                    finished_at = CURRENT_TIMESTAMP
                WHERE drone_uri = ?
                  AND status = 'ACTIVE';
            )");

            mission_stmt.execute(drone_uri);
        }

        response["TYPE"] = "RETURN_TO_BASE";
        response["DRONE_URI"] = drone_uri;
        response["BASE_LAT"] = base_lat;
        response["BASE_LON"] = base_lon;
        response["REASON"] = "LOW_BATTERY";

        std::cout << "[CENTRAL] RETURN_TO_BASE komanda za "
                  << drone_uri
                  << " prema bazi "
                  << base_lat << ", " << base_lon
                  << std::endl;

        return response;
    }

    response["TYPE"] = "ACK_ALARM";
    response["MESSAGE"] = "ALARM_SAVED";

    return response;
}

json handle_inspection_report(const json& msg)
{
    json response;

    std::string mission_id = msg.value("MISSION_ID", "");
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string point_id = msg.value("POINT_ID", "");
    double lat = msg.value("LAT", 0.0);
    double lon = msg.value("LON", 0.0);
    std::string result = msg.value("RESULT", "UNKNOWN");

    if (mission_id.empty() || point_id.empty())
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "MISSING_INSPECTION_REPORT_FIELDS";
        return response;
    }

    if (point_id != "I1" && point_id != "I2" &&
        point_id != "I3" && point_id != "I4")
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "INVALID_INSPECTION_POINT";
        response["POINT_ID"] = point_id;
        return response;
    }

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        std::string mission_type;
        std::string mission_status;

        auto type_stmt = g_db->prepare(R"(
            SELECT mission_type
            FROM missions
            WHERE mission_id = ?;
        )");
        type_stmt.execute(mission_id);

        if (!type_stmt.fetch(mission_type) || mission_type != "INSPECTION")
        {
            response["TYPE"] = "ERROR";
            response["MESSAGE"] = "MISSION_IS_NOT_INSPECTION";
            return response;
        }

        auto status_stmt = g_db->prepare(R"(
            SELECT status
            FROM missions
            WHERE mission_id = ?;
        )");
        status_stmt.execute(mission_id);
        status_stmt.fetch(mission_status);

        if (mission_status != "ACTIVE")
        {
            response["TYPE"] = "ERROR";
            response["MESSAGE"] = "INSPECTION_MISSION_NOT_ACTIVE";
            return response;
        }

        auto report_stmt = g_db->prepare(R"(
            INSERT OR REPLACE INTO inspection_reports
            (mission_id, drone_uri, point_id, lat, lon, result, received_at)
            VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
        )");

        report_stmt.execute(mission_id, drone_uri, point_id, lat, lon, result);
    }

    response["TYPE"] = "ACK_INSPECTION_REPORT";
    response["MISSION_ID"] = mission_id;
    response["DRONE_URI"] = drone_uri;
    response["POINT_ID"] = point_id;
    response["RESULT"] = result;

    std::cout << "[CENTRAL] INSPECTION_REPORT: "
              << mission_id << " | " << point_id
              << " | result=" << result
              << " | " << lat << "," << lon
              << std::endl;

    return response;
}

json handle_mission_finished(const json& msg)
{
    json response;

    std::string mission_id = msg.value("MISSION_ID", "");
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");

    if (mission_id.empty())
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "MISSING_MISSION_ID";
        return response;
    }

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        std::string mission_type;
        auto type_stmt = g_db->prepare(R"(
            SELECT mission_type
            FROM missions
            WHERE mission_id = ?;
        )");
        type_stmt.execute(mission_id);

        if (!type_stmt.fetch(mission_type))
        {
            response["TYPE"] = "ERROR";
            response["MESSAGE"] = "MISSION_NOT_FOUND";
            return response;
        }

        if (mission_type == "INSPECTION")
        {
            int completed_points = 0;
            auto count_stmt = g_db->prepare(R"(
                SELECT COUNT(*)
                FROM inspection_reports
                WHERE mission_id = ?
                  AND result = 'OK';
            )");
            count_stmt.execute(mission_id);
            count_stmt.fetch(completed_points);

            if (completed_points < 4)
            {
                response["TYPE"] = "MISSION_FINISH_REJECTED";
                response["MISSION_ID"] = mission_id;
                response["REASON"] = "INSPECTION_POINTS_INCOMPLETE";
                response["COMPLETED_POINTS"] = completed_points;
                response["REQUIRED_POINTS"] = 4;
                return response;
            }
        }

        auto stmt = g_db->prepare(R"(
            UPDATE missions
            SET status = 'FINISHED',
                finished_at = CURRENT_TIMESTAMP
            WHERE mission_id = ?;
        )");

        stmt.execute(mission_id);
    }

    response["TYPE"] = "ACK_MISSION_FINISHED";
    response["MISSION_ID"] = mission_id;
    response["DRONE_URI"] = drone_uri;

    std::cout << "[CENTRAL] Misija zavrsena: " << mission_id << std::endl;

    return response;
}

json handle_mission_request(const json& msg)
{
    json response;

    std::string mission_id = msg.value("MISSION_ID", "UNKNOWN_MISSION");
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    std::string mission_type = msg.value("MISSION_TYPE", "TEST_FLIGHT");
    std::string zone = msg.value("ZONE", "");
    std::string route_id = msg.value("ROUTE_ID", "");
    int requested_altitude = msg.value("ALTITUDE", 120);
    int mission_priority = get_mission_priority(mission_type);

    if (!zone_exists(region_id, zone))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "INVALID_ZONE";
        return response;
    }

    ZoneInfo zone_info = get_zone_info(region_id, zone);

    if (!zone_info.found)
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "ZONE_INFO_NOT_FOUND";
        return response;
    }

    double delivery_lat = msg.value("DELIVERY_LAT", 0.0);
    double delivery_lon = msg.value("DELIVERY_LON", 0.0);
    GeoPoint exit_geo;
    exit_geo.lat = 0.0;
    exit_geo.lon = 0.0;

    if (mission_type == "DELIVERY")
    {
        if (delivery_lat == 0.0 && delivery_lon == 0.0)
        {
            response["TYPE"] = "MISSION_REJECTED";
            response["MISSION_ID"] = mission_id;
            response["REASON"] = "MISSING_DELIVERY_POINT";
            return response;
        }

        LocalPoint delivery_offset = latlon_to_offset(
            zone_info.center_lat,
            zone_info.center_lon,
            delivery_lat,
            delivery_lon
        );

        int level = choose_delivery_contour(
            delivery_offset.north_m,
            delivery_offset.east_m,
            zone_info.radius_m,
            zone_info.contours
        );

        if (level < 0)
        {
            response["TYPE"] = "MISSION_REJECTED";
            response["MISSION_ID"] = mission_id;
            response["REASON"] = "DELIVERY_POINT_OUTSIDE_ZONE";
            return response;
        }

        route_id = zone + "_K" + std::to_string(level);

        int half_size_m = (zone_info.radius_m * level) / zone_info.contours;

        LocalPoint exit_local = closest_point_on_square_contour(
            delivery_offset.north_m,
            delivery_offset.east_m,
            half_size_m
        );

        exit_geo = offset_to_latlon(
            zone_info.center_lat,
            zone_info.center_lon,
            exit_local.north_m,
            exit_local.east_m
        );
    }
    else
    {
        if (route_id.empty() || route_id == "AUTO")
        {
            // Testni let i default misije idu na prvu konturu.
            route_id = zone + "_K1";
        }
    }

    RouteInfo route = get_route_info(region_id, zone, route_id);

    if (!route.found)
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "INVALID_ROUTE";
        return response;
    }

    int assigned_altitude = 0;
    int assigned_slot = -1;

    bool preempted = false;
    PreemptedMission preempted_mission;

    if (!assign_altitude_slot(region_id, zone, route_id, route,
                              requested_altitude,
                              assigned_altitude,
                              assigned_slot))
    {
        // Ruta je puna. Provjerava se da li nova misija ima veći prioritet
        // od neke aktivne misije na istoj ruti.
        preempted_mission = find_lowest_priority_active_mission(region_id, zone, route_id);

        if (preempted_mission.found &&
            mission_priority > preempted_mission.mission_priority)
        {
            mark_mission_preempted(preempted_mission.mission_id, mission_id);

            assigned_slot = preempted_mission.altitude_slot;
            assigned_altitude = preempted_mission.altitude;
            preempted = true;
        }
        else
        {
            response["TYPE"] = "MISSION_REJECTED";
            response["MISSION_ID"] = mission_id;
            response["REASON"] = "ROUTE_CAPACITY_FULL_OR_LOW_PRIORITY";
            response["NEW_MISSION_PRIORITY"] = mission_priority;

            if (preempted_mission.found)
            {
                response["LOWEST_ACTIVE_PRIORITY"] = preempted_mission.mission_priority;
            }

            return response;
        }
    }

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto stmt = g_db->prepare(R"(
            INSERT INTO missions
            (mission_id, drone_uri, region_id, mission_type, mission_priority, zone, route_id,
             altitude, altitude_slot, delivery_lat, delivery_lon, exit_lat, exit_lon, status)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'ACTIVE');
        )");

        stmt.execute(mission_id, drone_uri, region_id, mission_type, mission_priority, zone, route_id,
                     assigned_altitude, assigned_slot,
                     delivery_lat, delivery_lon, exit_geo.lat, exit_geo.lon);
    }

    response["TYPE"] = "MISSION_APPROVED";
    response["MISSION_ID"] = mission_id;
    response["DRONE_URI"] = drone_uri;
    response["COMMAND"] = "START_MISSION";
    response["MISSION_TYPE"] = mission_type;
    response["ZONE"] = zone;
    response["ROUTE_ID"] = route_id;
    response["ROUTE_TYPE"] = route.route_type;
    response["CONTOUR_LEVEL"] = route.contour_level;
    response["CENTER_LAT"] = zone_info.center_lat;
    response["CENTER_LON"] = zone_info.center_lon;
    response["HALF_SIZE_M"] = route.half_size_m;
    response["ALTITUDE"] = assigned_altitude;
    response["ALTITUDE_SLOT"] = assigned_slot;
    response["MISSION_PRIORITY"] = mission_priority;
    response["MAX_DRONES_ON_ROUTE"] = route.max_drones;
    response["VERTICAL_SEPARATION_M"] = route.vertical_separation;

    if (preempted)
    {
        response["PREEMPTED_DRONE"] = preempted_mission.drone_uri;
        response["PREEMPTED_MISSION_ID"] = preempted_mission.mission_id;
        response["PREEMPTED_PRIORITY"] = preempted_mission.mission_priority;
        response["PREEMPTION_REASON"] = "HIGHER_PRIORITY_MISSION";
    }

    if (mission_type == "DELIVERY")
    {
        response["DELIVERY_LAT"] = delivery_lat;
        response["DELIVERY_LON"] = delivery_lon;
        response["EXIT_LAT"] = exit_geo.lat;
        response["EXIT_LON"] = exit_geo.lon;
    }
    else if (mission_type == "INSPECTION")
    {
        // Cetiri inspection tacke su uglovi dodijeljene kvadratne konture.
        // Redoslijed je I1 -> I2 -> I3 -> I4; dron nakon svake tacke salje INSPECTION_REPORT.
        json points = json::array();

        const double h = static_cast<double>(route.half_size_m);
        const double north_offsets[4] = { h, h, -h, -h };
        const double east_offsets[4]  = { -h, h, h, -h };

        for (int i = 0; i < 4; ++i)
        {
            GeoPoint p = offset_to_latlon(zone_info.center_lat, zone_info.center_lon,
                                          north_offsets[i], east_offsets[i]);

            json point;
            point["POINT_ID"] = "I" + std::to_string(i + 1);
            point["LAT"] = p.lat;
            point["LON"] = p.lon;
            points.push_back(point);
        }

        response["INSPECTION_POINTS"] = points;
        response["REQUIRED_INSPECTION_POINTS"] = 4;
    }

    std::cout << "[CENTRAL] Misija odobrena: " << mission_id
              << " | type=" << mission_type
              << " | " << zone << "/" << route_id
              << " | priority=" << mission_priority
              << " | slot=" << assigned_slot
              << " | altitude=" << assigned_altitude << "m";

    if (mission_type == "DELIVERY")
    {
        std::cout << " | exit=" << exit_geo.lat << "," << exit_geo.lon;
    }

    if (preempted)
    {
        std::cout << " | preempted=" << preempted_mission.drone_uri
                  << "/" << preempted_mission.mission_id;
    }

    std::cout << std::endl;

    return response;
}

class Session : public std::enable_shared_from_this<Session>
{
public:
    explicit Session(tcp::socket socket)
        : socket_(std::move(socket))
    {
    }

    void start()
    {
        read_message();
    }

private:
    void read_message()
    {
        auto self = shared_from_this();

        boost::asio::async_read_until(socket_, buffer_, "\n",
            [this, self](boost::system::error_code ec, std::size_t)
            {
                if (!ec)
                {
                    std::istream is(&buffer_);
                    std::string message;
                    std::getline(is, message);

                    if (!message.empty())
                    {
                        process_message(message);
                    }

                    read_message();
                }
                else
                {
                    std::cout << "[CENTRAL] Regionalni server prekinuo konekciju.\n";
                }
            });
    }

    void process_message(const std::string& message)
    {
        json response;

        try
        {
            json msg = json::parse(message);
            std::string type = msg.value("TYPE", "UNKNOWN");

            if (type == "REGION_REGISTER")
            {
                handle_region_register(msg);
                response["TYPE"] = "ACK";
                response["MESSAGE"] = "REGION_REGISTERED";
            }
            else if (type == "DRONE_STATUS")
            {
                handle_drone_status(msg);
                response["TYPE"] = "ACK";
                response["MESSAGE"] = "DRONE_STATUS_SAVED";
            }
            else if (type == "ALARM")
            {
                response = handle_alarm(msg);
            }
            else if (type == "MISSION_REQUEST")
            {
                response = handle_mission_request(msg);
            }
            else if (type == "INSPECTION_REPORT")
            {
                response = handle_inspection_report(msg);
            }
            else if (type == "MISSION_FINISHED")
            {
                response = handle_mission_finished(msg);
            }
            else
            {
                response["TYPE"] = "ERROR";
                response["MESSAGE"] = "UNKNOWN_MESSAGE_TYPE";
            }
        }
        catch (const std::exception& e)
        {
            response["TYPE"] = "ERROR";
            response["MESSAGE"] = e.what();
        }

        send_message(response.dump() + "\n");
    }

    void send_message(const std::string& message)
    {
        auto self = shared_from_this();
        auto out = std::make_shared<std::string>(message);

        boost::asio::async_write(socket_, boost::asio::buffer(*out),
            [this, self, out](boost::system::error_code ec, std::size_t)
            {
                if (ec)
                {
                    std::cerr << "[CENTRAL] Greška pri slanju odgovora: "
                              << ec.message() << std::endl;
                }
            });
    }

private:
    tcp::socket socket_;
    boost::asio::streambuf buffer_;
};

class CentralServer
{
public:
    CentralServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
    {
        accept_connection();
    }

private:
    void accept_connection()
    {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    std::cout << "[CENTRAL] Regionalni server povezan.\n";
                    std::make_shared<Session>(std::move(socket))->start();
                }

                accept_connection();
            });
    }

private:
    tcp::acceptor acceptor_;
};

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: ./central_server <port>\n";
        return 1;
    }

    try
    {
        g_db.reset(new sqlite::db("central_server.db"));
        init_database();

        boost::asio::io_context io_context;
        CentralServer server(io_context, std::atoi(argv[1]));

        std::cout << "[CENTRAL] Centralni server pokrenut na portu "
                  << argv[1] << std::endl;

        io_context.run();
    }
    catch (const sqlite::exception& e)
    {
        std::cerr << "[CENTRAL] SQLite greška: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[CENTRAL] Greška: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
