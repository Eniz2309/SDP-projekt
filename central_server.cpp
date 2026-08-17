// v6_watchdog: obrada CONNECTION_LOST alarma i prekid aktivne misije.
// v7_inspection: centralni server sa podrskom za INSPECTION tacke i INSPECTION_REPORT poruke.
// v9_scheduler: prioriteti se primjenjuju na RED CEKANJA ZADATAKA i raspolozivost dronova.
// v10_formation: formacijski let koristi REGIONALNI SERVER kao VIRTUAL_LEADER/FORMATION_CONTROLLER.
// v11_control_commands: operator moze poslati CHANGE_PARAMS i rucni RETURN_TO_BASE preko servera.
// Svi clanovi formacije koriste isti visinski nivo, a razdvojeni su horizontalnim offsetima.
// Dronovi se registruju kao AVAILABLE, centralni server dodjeljuje zadatke slobodnim dronovima.
// Nema automatskog preemptiona zbog prioriteta; STOP_MISSION ostaje posebna kontrolna komanda.
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
// - prioritetni red zadataka (MONITORING > DELIVERY > INSPECTION > TEST_FLIGHT)
// - dodjela zadataka slobodnim dronovima; rute i kapacitet su odvojena logika
// - FORMATION: 2-5 slobodnih dronova, isti altitude, horizontalni razmak, server kao virtual leader

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
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
#include "pqc_tls_utils.h"

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;
using json = nlohmann::json;
namespace sqlite = sqlite3_wrapper;

std::unique_ptr<sqlite::db> g_db;
std::mutex db_mutex;
std::mutex scheduler_mutex;

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

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS formation_requests (
            mission_id TEXT PRIMARY KEY,
            formation_size INTEGER NOT NULL,
            spacing_m REAL NOT NULL
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS formation_members (
            mission_id TEXT,
            drone_uri TEXT,
            member_index INTEGER,
            offset_north_m REAL,
            offset_east_m REAL,
            status TEXT,
            PRIMARY KEY(mission_id, drone_uri)
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

    // FORMATION je poseban serverski kontrolisan zadatak.
    // Ne mijenja trazeni red 4>3>2>1 za osnovne misije; dijeli nivo sa INSPECTION.
    if (mission_type == "FORMATION")
        return 2;

    return 0;
}


json schedule_region(const std::string& region_id);


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

json handle_ack_stop(const json& msg)
{
    json response;

    std::string mission_id = msg.value("MISSION_ID", "");
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");

    if (mission_id.empty())
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "MISSING_MISSION_ID_IN_ACK_STOP";
        return response;
    }

    std::string mission_type;
    {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto q = g_db->prepare("SELECT mission_type FROM missions WHERE mission_id = ?;");
        q.execute(mission_id);
        q.fetch(mission_type);
    }

    if (mission_type == "FORMATION")
    {
        int remaining = 0;
        {
            std::lock_guard<std::mutex> lock(db_mutex);

            auto member_stmt = g_db->prepare(R"(
                UPDATE formation_members
                SET status = 'STOPPED'
                WHERE mission_id = ? AND drone_uri = ?;
            )");
            member_stmt.execute(mission_id, drone_uri);

            auto drone_stmt = g_db->prepare(R"(
                UPDATE drones
                SET status = 'AVAILABLE', route_id = '', last_seen = CURRENT_TIMESTAMP
                WHERE drone_uri = ?;
            )");
            drone_stmt.execute(drone_uri);

            auto count_stmt = g_db->prepare(R"(
                SELECT COUNT(*) FROM formation_members
                WHERE mission_id = ? AND status != 'STOPPED';
            )");
            count_stmt.execute(mission_id);
            count_stmt.fetch(remaining);

            if (remaining == 0)
            {
                auto mission_stmt = g_db->prepare(R"(
                    UPDATE missions
                    SET status = 'STOPPED', finished_at = CURRENT_TIMESTAMP
                    WHERE mission_id = ? AND status IN ('ACTIVE', 'STOP_REQUESTED');
                )");
                mission_stmt.execute(mission_id);
            }
        }

        response["TYPE"] = "ACK_STOP_SAVED";
        response["MISSION_ID"] = mission_id;
        response["DRONE_URI"] = drone_uri;
        response["REGION_ID"] = region_id;
        response["FORMATION_REMAINING"] = remaining;
        response["FORMATION_STOPPED"] = (remaining == 0);
        response["ASSIGNMENTS"] = (remaining == 0) ? schedule_region(region_id) : json::array();

        std::cout << "[CENTRAL][FORMATION] ACK_STOP " << drone_uri
                  << " | mission=" << mission_id
                  << " | remaining=" << remaining << std::endl;
        return response;
    }

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto mission_stmt = g_db->prepare(R"(
            UPDATE missions
            SET status = 'STOPPED',
                finished_at = CURRENT_TIMESTAMP
            WHERE mission_id = ?
              AND drone_uri = ?
              AND status IN ('ACTIVE', 'STOP_REQUESTED');
        )");
        mission_stmt.execute(mission_id, drone_uri);

        auto drone_stmt = g_db->prepare(R"(
            UPDATE drones
            SET status = 'AVAILABLE',
                route_id = '',
                last_seen = CURRENT_TIMESTAMP
            WHERE drone_uri = ?;
        )");
        drone_stmt.execute(drone_uri);
    }

    response["TYPE"] = "ACK_STOP_SAVED";
    response["MISSION_ID"] = mission_id;
    response["DRONE_URI"] = drone_uri;
    response["REGION_ID"] = region_id;
    response["ASSIGNMENTS"] = schedule_region(region_id);

    std::cout << "[CENTRAL] ACK_STOP potvrden za dron "
              << drone_uri << " | mission=" << mission_id
              << "; dron je ponovo AVAILABLE." << std::endl;

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
    std::string region_id;

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
            SELECT mission_type FROM missions WHERE mission_id = ?;
        )");
        type_stmt.execute(mission_id);

        if (!type_stmt.fetch(mission_type))
        {
            response["TYPE"] = "ERROR";
            response["MESSAGE"] = "MISSION_NOT_FOUND";
            return response;
        }

        auto region_stmt = g_db->prepare(R"(
            SELECT region_id FROM missions WHERE mission_id = ?;
        )");
        region_stmt.execute(mission_id);
        region_stmt.fetch(region_id);

        if (mission_type == "INSPECTION")
        {
            int completed_points = 0;
            auto count_stmt = g_db->prepare(R"(
                SELECT COUNT(*) FROM inspection_reports
                WHERE mission_id = ? AND result = 'OK';
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
            SET status = 'FINISHED', finished_at = CURRENT_TIMESTAMP
            WHERE mission_id = ? AND drone_uri = ?;
        )");
        stmt.execute(mission_id, drone_uri);

        auto drone_stmt = g_db->prepare(R"(
            UPDATE drones
            SET status = 'AVAILABLE', route_id = '', last_seen = CURRENT_TIMESTAMP
            WHERE drone_uri = ?;
        )");
        drone_stmt.execute(drone_uri);
    }

    response["TYPE"] = "ACK_MISSION_FINISHED";
    response["MISSION_ID"] = mission_id;
    response["DRONE_URI"] = drone_uri;
    response["ASSIGNMENTS"] = schedule_region(region_id);

    std::cout << "[CENTRAL] Misija zavrsena: " << mission_id
              << "; dron " << drone_uri << " je AVAILABLE." << std::endl;

    return response;
}

bool mission_id_exists(const std::string& mission_id)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare("SELECT COUNT(*) FROM missions WHERE mission_id = ?;");
    stmt.execute(mission_id);
    int count = 0;
    stmt.fetch(count);
    return count > 0;
}

bool get_available_drone(const std::string& region_id, std::string& drone_uri)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare(R"(
        SELECT drone_uri
        FROM drones
        WHERE region_id = ?
          AND status = 'AVAILABLE'
          AND battery > 20
        ORDER BY last_seen ASC
        LIMIT 1;
    )");
    stmt.execute(region_id);
    return stmt.fetch(drone_uri);
}


std::vector<std::string> get_available_drones(const std::string& region_id, int count)
{
    std::vector<std::string> drones;
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare(R"(
        SELECT drone_uri
        FROM drones
        WHERE region_id = ?
          AND status = 'AVAILABLE'
          AND battery > 20
        ORDER BY last_seen ASC;
    )");
    stmt.execute(region_id);
    std::string drone_uri;
    while (stmt.fetch(drone_uri))
    {
        drones.push_back(drone_uri);
        if (static_cast<int>(drones.size()) >= count)
            break;
    }
    return drones;
}

std::string get_mission_type_by_id(const std::string& mission_id)
{
    std::string type;
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare("SELECT mission_type FROM missions WHERE mission_id = ?;");
    stmt.execute(mission_id);
    stmt.fetch(type);
    return type;
}

bool get_formation_request(const std::string& mission_id, int& formation_size, double& spacing_m)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare(
        "SELECT formation_size FROM formation_requests WHERE mission_id = ?;"
    );
    stmt.execute(mission_id);
    if (!stmt.fetch(formation_size))
        return false;

    stmt = g_db->prepare(
        "SELECT spacing_m FROM formation_requests WHERE mission_id = ?;"
    );
    stmt.execute(mission_id);
    stmt.fetch(spacing_m);
    return true;
}

json get_formation_member_uris(const std::string& mission_id)
{
    json result = json::array();
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare(R"(
        SELECT drone_uri
        FROM formation_members
        WHERE mission_id = ?
        ORDER BY member_index ASC;
    )");
    stmt.execute(mission_id);
    std::string drone_uri;
    while (stmt.fetch(drone_uri))
        result.push_back(drone_uri);
    return result;
}

std::vector<std::string> get_queued_mission_ids(const std::string& region_id)
{
    std::vector<std::string> ids;
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare(R"(
        SELECT mission_id
        FROM missions
        WHERE region_id = ? AND status = 'QUEUED'
        ORDER BY mission_priority DESC, created_at ASC;
    )");
    stmt.execute(region_id);
    std::string mission_id;
    while (stmt.fetch(mission_id))
        ids.push_back(mission_id);
    return ids;
}

bool build_assignment(const std::string& mission_id,
                      const std::string& drone_uri,
                      json& assignment)
{
    std::string region_id, mission_type, zone, route_id;
    int mission_priority = 0;
    int requested_altitude = 120;
    double delivery_lat = 0.0, delivery_lon = 0.0;

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto q = g_db->prepare("SELECT region_id FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); if (!q.fetch(region_id)) return false;
        q = g_db->prepare("SELECT mission_type FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(mission_type);
        q = g_db->prepare("SELECT mission_priority FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(mission_priority);
        q = g_db->prepare("SELECT zone FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(zone);
        q = g_db->prepare("SELECT route_id FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(route_id);
        q = g_db->prepare("SELECT altitude FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(requested_altitude);
        q = g_db->prepare("SELECT delivery_lat FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(delivery_lat);
        q = g_db->prepare("SELECT delivery_lon FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(delivery_lon);
    }

    ZoneInfo zone_info = get_zone_info(region_id, zone);
    if (!zone_info.found) return false;

    GeoPoint exit_geo{0.0, 0.0};

    if (mission_type == "DELIVERY")
    {
        LocalPoint delivery_offset = latlon_to_offset(
            zone_info.center_lat, zone_info.center_lon, delivery_lat, delivery_lon);
        int level = choose_delivery_contour(delivery_offset.north_m, delivery_offset.east_m,
                                            zone_info.radius_m, zone_info.contours);
        if (level < 0) return false;
        route_id = zone + "_K" + std::to_string(level);
        int half_size_m = (zone_info.radius_m * level) / zone_info.contours;
        LocalPoint exit_local = closest_point_on_square_contour(
            delivery_offset.north_m, delivery_offset.east_m, half_size_m);
        exit_geo = offset_to_latlon(zone_info.center_lat, zone_info.center_lon,
                                    exit_local.north_m, exit_local.east_m);
    }
    else if (route_id.empty() || route_id == "AUTO")
    {
        route_id = zone + "_K1";
    }

    RouteInfo route = get_route_info(region_id, zone, route_id);
    if (!route.found) return false;

    int assigned_altitude = 0;
    int assigned_slot = -1;
    if (!assign_altitude_slot(region_id, zone, route_id, route,
                              requested_altitude, assigned_altitude, assigned_slot))
    {
        // Vazno: ruta puna NE izaziva preemption. Zadatak ostaje QUEUED.
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto mission_stmt = g_db->prepare(R"(
            UPDATE missions
            SET drone_uri = ?, route_id = ?, altitude = ?, altitude_slot = ?,
                exit_lat = ?, exit_lon = ?, status = 'ACTIVE'
            WHERE mission_id = ? AND status = 'QUEUED';
        )");
        mission_stmt.execute(drone_uri, route_id, assigned_altitude, assigned_slot,
                             exit_geo.lat, exit_geo.lon, mission_id);

        auto drone_stmt = g_db->prepare(R"(
            UPDATE drones
            SET status = 'BUSY', route_id = ?, altitude = ?, last_seen = CURRENT_TIMESTAMP
            WHERE drone_uri = ? AND status = 'AVAILABLE';
        )");
        drone_stmt.execute(route_id, assigned_altitude, drone_uri);
    }

    assignment["TYPE"] = "START_MISSION";
    assignment["MISSION_ID"] = mission_id;
    assignment["ASSIGNED_DRONE"] = drone_uri;
    assignment["DRONE_URI"] = drone_uri;
    assignment["MISSION_TYPE"] = mission_type;
    assignment["MISSION_PRIORITY"] = mission_priority;
    assignment["ZONE"] = zone;
    assignment["ROUTE_ID"] = route_id;
    assignment["ROUTE_TYPE"] = route.route_type;
    assignment["CONTOUR_LEVEL"] = route.contour_level;
    assignment["CENTER_LAT"] = zone_info.center_lat;
    assignment["CENTER_LON"] = zone_info.center_lon;
    assignment["HALF_SIZE_M"] = route.half_size_m;
    assignment["ALTITUDE"] = assigned_altitude;
    assignment["ALTITUDE_SLOT"] = assigned_slot;
    assignment["MAX_DRONES_ON_ROUTE"] = route.max_drones;
    assignment["VERTICAL_SEPARATION_M"] = route.vertical_separation;

    if (mission_type == "DELIVERY")
    {
        assignment["DELIVERY_LAT"] = delivery_lat;
        assignment["DELIVERY_LON"] = delivery_lon;
        assignment["EXIT_LAT"] = exit_geo.lat;
        assignment["EXIT_LON"] = exit_geo.lon;
    }
    else if (mission_type == "INSPECTION")
    {
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
        assignment["INSPECTION_POINTS"] = points;
        assignment["REQUIRED_INSPECTION_POINTS"] = 4;
    }

    std::cout << "[CENTRAL][SCHEDULER] " << mission_id
              << " (priority=" << mission_priority << ") -> " << drone_uri
              << " | route=" << route_id << " | altitude=" << assigned_altitude
              << std::endl;
    return true;
}


bool build_formation_assignment(const std::string& mission_id, json& commands)
{
    std::string region_id, zone, route_id;
    int mission_priority = 0;
    int requested_altitude = 120;
    int formation_size = 3;
    double spacing_m = 10.0;

    {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto q = g_db->prepare("SELECT region_id FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); if (!q.fetch(region_id)) return false;
        q = g_db->prepare("SELECT mission_priority FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(mission_priority);
        q = g_db->prepare("SELECT zone FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(zone);
        q = g_db->prepare("SELECT route_id FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(route_id);
        q = g_db->prepare("SELECT altitude FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(requested_altitude);
    }

    if (!get_formation_request(mission_id, formation_size, spacing_m))
        return false;

    std::vector<std::string> members = get_available_drones(region_id, formation_size);
    if (static_cast<int>(members.size()) < formation_size)
        return false;

    ZoneInfo zone_info = get_zone_info(region_id, zone);
    if (!zone_info.found)
        return false;

    if (route_id.empty() || route_id == "AUTO")
        route_id = zone + "_K1";

    RouteInfo route = get_route_info(region_id, zone, route_id);
    if (!route.found)
        return false;

    // Cijela formacija zauzima JEDAN logicki visinski slot na ruti.
    // Svi clanovi dobijaju potpuno isti altitude.
    int formation_altitude = 0;
    int formation_slot = -1;
    if (!assign_altitude_slot(region_id, zone, route_id, route,
                              requested_altitude, formation_altitude, formation_slot))
        return false;

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto mission_stmt = g_db->prepare(R"(
            UPDATE missions
            SET drone_uri = ?, route_id = ?, altitude = ?, altitude_slot = ?,
                status = 'ACTIVE'
            WHERE mission_id = ? AND status = 'QUEUED';
        )");
        // drone_uri je samo reprezentativni clan radi kompatibilnosti sa starijom semom.
        mission_stmt.execute(members.front(), route_id, formation_altitude,
                             formation_slot, mission_id);

        auto del = g_db->prepare("DELETE FROM formation_members WHERE mission_id = ?;");
        del.execute(mission_id);

        auto member_stmt = g_db->prepare(R"(
            INSERT OR REPLACE INTO formation_members
            (mission_id, drone_uri, member_index, offset_north_m, offset_east_m, status)
            VALUES (?, ?, ?, ?, ?, 'ACTIVE');
        )");

        auto drone_stmt = g_db->prepare(R"(
            UPDATE drones
            SET status = 'BUSY', route_id = ?, altitude = ?, last_seen = CURRENT_TIMESTAMP
            WHERE drone_uri = ? AND status = 'AVAILABLE';
        )");

        const double center_index = (formation_size - 1) / 2.0;
        for (int i = 0; i < formation_size; ++i)
        {
            // Line-abreast: svi su na istom nivou, razmaknuti lijevo/desno.
            double offset_north_m = 0.0;
            double offset_east_m = (i - center_index) * spacing_m;

            member_stmt.execute(mission_id, members[i], i,
                                offset_north_m, offset_east_m);
            drone_stmt.execute(route_id, formation_altitude, members[i]);
        }
    }

    commands = json::array();
    const double center_index = (formation_size - 1) / 2.0;
    for (int i = 0; i < formation_size; ++i)
    {
        json command;
        command["TYPE"] = "START_FORMATION";
        command["MISSION_ID"] = mission_id;
        command["MISSION_TYPE"] = "FORMATION";
        command["MISSION_PRIORITY"] = mission_priority;
        command["ASSIGNED_DRONE"] = members[i];
        command["DRONE_URI"] = members[i];
        command["ZONE"] = zone;
        command["ROUTE_ID"] = route_id;
        command["ROUTE_TYPE"] = route.route_type;
        command["CONTOUR_LEVEL"] = route.contour_level;
        command["CENTER_LAT"] = zone_info.center_lat;
        command["CENTER_LON"] = zone_info.center_lon;
        command["HALF_SIZE_M"] = route.half_size_m;
        command["ALTITUDE"] = formation_altitude;
        command["ALTITUDE_SLOT"] = formation_slot;
        command["FORMATION_SIZE"] = formation_size;
        command["FORMATION_MEMBER_INDEX"] = i;
        command["OFFSET_NORTH_M"] = 0.0;
        command["OFFSET_EAST_M"] = (i - center_index) * spacing_m;
        command["FORMATION_SPACING_M"] = spacing_m;
        command["FORMATION_CONTROLLER"] = "REGIONAL_SERVER";
        command["VIRTUAL_LEADER"] = true;
        command["SAME_ALTITUDE"] = true;
        commands.push_back(command);
    }

    std::cout << "[CENTRAL][FORMATION] " << mission_id
              << " -> " << formation_size << " dronova"
              << " | route=" << route_id
              << " | zajednicka visina=" << formation_altitude
              << "m | spacing=" << spacing_m << "m" << std::endl;

    return true;
}

json schedule_region(const std::string& region_id)
{
    // Scheduler je serijalizovan da dvije paralelne sesije ne dodijele isti AVAILABLE dron.
    std::lock_guard<std::mutex> scheduler_lock(scheduler_mutex);
    json assignments = json::array();

    for (;;)
    {
        std::vector<std::string> queued = get_queued_mission_ids(region_id);
        if (queued.empty())
            break;

        bool assigned = false;

        // Prioritet je vec DESC. Ako FORMATION nema dovoljno slobodnih dronova,
        // scheduler moze probati sljedecu misiju umjesto da blokira cijeli red.
        for (const auto& mission_id : queued)
        {
            std::string mission_type = get_mission_type_by_id(mission_id);

            if (mission_type == "FORMATION")
            {
                json formation_commands;
                if (build_formation_assignment(mission_id, formation_commands))
                {
                    for (const auto& command : formation_commands)
                        assignments.push_back(command);
                    assigned = true;
                    break;
                }
            }
            else
            {
                std::string drone_uri;
                if (!get_available_drone(region_id, drone_uri))
                    continue;

                json command;
                if (build_assignment(mission_id, drone_uri, command))
                {
                    assignments.push_back(command);
                    assigned = true;
                    break;
                }
            }
        }

        if (!assigned)
            break;
    }

    return assignments;
}

json handle_drone_ready(const json& msg)
{
    json response;
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");

    {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto stmt = g_db->prepare(R"(
            UPDATE drones
            SET status = 'AVAILABLE', route_id = '', last_seen = CURRENT_TIMESTAMP
            WHERE drone_uri = ?;
        )");
        stmt.execute(drone_uri);
    }

    response["TYPE"] = "ACK_DRONE_READY";
    response["DRONE_URI"] = drone_uri;
    response["STATUS"] = "AVAILABLE";
    response["ASSIGNMENTS"] = schedule_region(region_id);

    std::cout << "[CENTRAL] Dron " << drone_uri << " je AVAILABLE." << std::endl;
    return response;
}

json handle_change_params_request(const json& msg)
{
    json response;

    std::string drone_uri = msg.value("DRONE_URI", "");
    std::string request_region = msg.value("REGION_ID", "UNKNOWN_REGION");
    int altitude = msg.value("ALTITUDE", -1);
    int speed = msg.value("SPEED", -1);
    std::string direction = msg.value("DIRECTION", "");

    if (drone_uri.empty())
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "MISSING_DRONE_URI";
        return response;
    }

    if (altitude < 20 || altitude > 500 || speed < 0 || speed > 50 || direction.empty())
    {
        response["TYPE"] = "CONTROL_REJECTED";
        response["DRONE_URI"] = drone_uri;
        response["REASON"] = "INVALID_FLIGHT_PARAMETERS";
        return response;
    }

    std::string drone_region;
    std::string drone_status;
    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto q = g_db->prepare("SELECT region_id FROM drones WHERE drone_uri = ?;");
        q.execute(drone_uri);
        if (!q.fetch(drone_region))
        {
            response["TYPE"] = "CONTROL_REJECTED";
            response["DRONE_URI"] = drone_uri;
            response["REASON"] = "DRONE_NOT_FOUND";
            return response;
        }

        q = g_db->prepare("SELECT status FROM drones WHERE drone_uri = ?;");
        q.execute(drone_uri);
        q.fetch(drone_status);
    }

    if (drone_region != request_region)
    {
        response["TYPE"] = "CONTROL_REJECTED";
        response["DRONE_URI"] = drone_uri;
        response["REASON"] = "DRONE_NOT_IN_THIS_REGION";
        return response;
    }

    if (drone_status == "CONNECTION_LOST")
    {
        response["TYPE"] = "CONTROL_REJECTED";
        response["DRONE_URI"] = drone_uri;
        response["REASON"] = "DRONE_CONNECTION_LOST";
        return response;
    }

    if (drone_status == "RETURN_TO_BASE")
    {
        response["TYPE"] = "CONTROL_REJECTED";
        response["DRONE_URI"] = drone_uri;
        response["REASON"] = "DRONE_RETURNING_TO_BASE";
        return response;
    }

    json command;
    command["TYPE"] = "CHANGE_PARAMS";
    command["DRONE_URI"] = drone_uri;
    command["ALTITUDE"] = altitude;
    command["SPEED"] = speed;
    command["DIRECTION"] = direction;
    command["REASON"] = "OPERATOR_REQUEST";

    response["TYPE"] = "CHANGE_PARAMS_DISPATCH";
    response["TARGET_DRONE"] = drone_uri;
    response["COMMAND"] = command;
    response["REGION_ID"] = drone_region;

    std::cout << "[CENTRAL][CONTROL] CHANGE_PARAMS za " << drone_uri
              << " | altitude=" << altitude
              << " speed=" << speed
              << " direction=" << direction << std::endl;

    return response;
}

json handle_manual_rtb_request(const json& msg)
{
    json response;

    std::string drone_uri = msg.value("DRONE_URI", "");
    std::string request_region = msg.value("REGION_ID", "UNKNOWN_REGION");

    if (drone_uri.empty())
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "MISSING_DRONE_URI";
        return response;
    }

    std::string drone_region;
    std::string drone_status;
    double base_lat = 0.0;
    double base_lon = 0.0;
    std::string formation_mission;
    std::string active_mission;

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        auto q = g_db->prepare("SELECT region_id FROM drones WHERE drone_uri = ?;");
        q.execute(drone_uri);
        if (!q.fetch(drone_region))
        {
            response["TYPE"] = "RTB_REJECTED";
            response["DRONE_URI"] = drone_uri;
            response["REASON"] = "DRONE_NOT_FOUND";
            return response;
        }

        q = g_db->prepare("SELECT status FROM drones WHERE drone_uri = ?;");
        q.execute(drone_uri);
        q.fetch(drone_status);

        q = g_db->prepare("SELECT base_lat FROM regional_servers WHERE region_id = ?;");
        q.execute(drone_region);
        if (!q.fetch(base_lat))
        {
            response["TYPE"] = "RTB_REJECTED";
            response["DRONE_URI"] = drone_uri;
            response["REASON"] = "BASE_NOT_FOUND_FOR_REGION";
            return response;
        }

        q = g_db->prepare("SELECT base_lon FROM regional_servers WHERE region_id = ?;");
        q.execute(drone_region);
        q.fetch(base_lon);

        // Ako je dron clan aktivne formacije, prvo treba zaustaviti cijelu formaciju.
        q = g_db->prepare(R"(
            SELECT fm.mission_id
            FROM formation_members fm
            JOIN missions m ON m.mission_id = fm.mission_id
            WHERE fm.drone_uri = ?
              AND fm.status = 'ACTIVE'
              AND m.status IN ('ACTIVE', 'STOP_REQUESTED')
            LIMIT 1;
        )");
        q.execute(drone_uri);
        q.fetch(formation_mission);

        q = g_db->prepare(R"(
            SELECT mission_id
            FROM missions
            WHERE drone_uri = ? AND status = 'ACTIVE'
            LIMIT 1;
        )");
        q.execute(drone_uri);
        q.fetch(active_mission);
    }

    if (drone_region != request_region)
    {
        response["TYPE"] = "RTB_REJECTED";
        response["DRONE_URI"] = drone_uri;
        response["REASON"] = "DRONE_NOT_IN_THIS_REGION";
        return response;
    }

    if (drone_status == "CONNECTION_LOST")
    {
        response["TYPE"] = "RTB_REJECTED";
        response["DRONE_URI"] = drone_uri;
        response["REASON"] = "DRONE_CONNECTION_LOST";
        return response;
    }

    if (!formation_mission.empty())
    {
        response["TYPE"] = "RTB_REJECTED";
        response["DRONE_URI"] = drone_uri;
        response["REASON"] = "DRONE_IN_ACTIVE_FORMATION_STOP_FORMATION_FIRST";
        response["FORMATION_MISSION_ID"] = formation_mission;
        return response;
    }

    {
        std::lock_guard<std::mutex> lock(db_mutex);

        if (!active_mission.empty())
        {
            auto m = g_db->prepare(R"(
                UPDATE missions
                SET status = 'ABORTED_MANUAL_RTB', finished_at = CURRENT_TIMESTAMP
                WHERE mission_id = ? AND status = 'ACTIVE';
            )");
            m.execute(active_mission);
        }

        auto d = g_db->prepare(R"(
            UPDATE drones
            SET status = 'RETURN_TO_BASE', route_id = '', last_seen = CURRENT_TIMESTAMP
            WHERE drone_uri = ?;
        )");
        d.execute(drone_uri);
    }

    json command;
    command["TYPE"] = "RETURN_TO_BASE";
    command["DRONE_URI"] = drone_uri;
    command["BASE_LAT"] = base_lat;
    command["BASE_LON"] = base_lon;
    command["REASON"] = "MANUAL_OPERATOR_REQUEST";

    response["TYPE"] = "RETURN_TO_BASE_DISPATCH";
    response["TARGET_DRONE"] = drone_uri;
    response["COMMAND"] = command;
    response["REGION_ID"] = drone_region;
    response["ABORTED_MISSION_ID"] = active_mission;
    response["ASSIGNMENTS"] = schedule_region(drone_region);

    std::cout << "[CENTRAL][CONTROL] MANUAL RTB za " << drone_uri
              << " -> " << base_lat << ", " << base_lon;
    if (!active_mission.empty())
        std::cout << " | aborted mission=" << active_mission;
    std::cout << std::endl;

    return response;
}

json handle_mission_submit(const json& msg)
{
    json response;
    std::string mission_id = msg.value("MISSION_ID", "");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    std::string mission_type = msg.value("MISSION_TYPE", "");
    std::string zone = msg.value("ZONE", "");
    std::string route_id = msg.value("ROUTE_ID", "AUTO");
    int requested_altitude = msg.value("ALTITUDE", 120);
    double delivery_lat = msg.value("DELIVERY_LAT", 0.0);
    double delivery_lon = msg.value("DELIVERY_LON", 0.0);
    int formation_size = msg.value("FORMATION_SIZE", 3);
    double formation_spacing_m = msg.value("FORMATION_SPACING_M", 10.0);
    int priority = get_mission_priority(mission_type);

    if (mission_id.empty() || priority == 0)
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["REASON"] = "INVALID_MISSION_ID_OR_TYPE";
        return response;
    }
    if (mission_id_exists(mission_id))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "DUPLICATE_MISSION_ID";
        return response;
    }
    if (!zone_exists(region_id, zone))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "INVALID_ZONE";
        return response;
    }
    if (mission_type == "DELIVERY" && delivery_lat == 0.0 && delivery_lon == 0.0)
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "MISSING_DELIVERY_POINT";
        return response;
    }
    if (mission_type == "FORMATION" &&
        (formation_size < 2 || formation_size > 5 ||
         formation_spacing_m < 2.0 || formation_spacing_m > 100.0))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "INVALID_FORMATION_PARAMETERS";
        return response;
    }

    {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto stmt = g_db->prepare(R"(
            INSERT INTO missions
            (mission_id, drone_uri, region_id, mission_type, mission_priority, zone, route_id,
             altitude, altitude_slot, delivery_lat, delivery_lon, exit_lat, exit_lon, status)
            VALUES (?, '', ?, ?, ?, ?, ?, ?, -1, ?, ?, 0, 0, 'QUEUED');
        )");
        stmt.execute(mission_id, region_id, mission_type, priority, zone, route_id,
                     requested_altitude, delivery_lat, delivery_lon);

        if (mission_type == "FORMATION")
        {
            auto f = g_db->prepare(R"(
                INSERT OR REPLACE INTO formation_requests
                (mission_id, formation_size, spacing_m)
                VALUES (?, ?, ?);
            )");
            f.execute(mission_id, formation_size, formation_spacing_m);
        }
    }

    json assignments = schedule_region(region_id);
    std::string current_status = "QUEUED";
    std::string assigned_drone;
    {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto s = g_db->prepare("SELECT status FROM missions WHERE mission_id = ?;");
        s.execute(mission_id); s.fetch(current_status);
        s = g_db->prepare("SELECT drone_uri FROM missions WHERE mission_id = ?;");
        s.execute(mission_id); s.fetch(assigned_drone);
    }

    response["TYPE"] = "MISSION_SUBMITTED";
    response["MISSION_ID"] = mission_id;
    response["MISSION_TYPE"] = mission_type;
    response["MISSION_PRIORITY"] = priority;
    response["STATUS"] = current_status;
    response["ASSIGNED_DRONE"] = (mission_type == "FORMATION") ? "" : assigned_drone;
    response["ASSIGNMENTS"] = assignments;

    if (mission_type == "FORMATION")
    {
        response["ASSIGNED_DRONES"] = get_formation_member_uris(mission_id);
        response["FORMATION_SIZE"] = formation_size;
        response["FORMATION_SPACING_M"] = formation_spacing_m;
        response["FORMATION_CONTROLLER"] = "REGIONAL_SERVER";
        response["SAME_ALTITUDE"] = true;
    }

    std::cout << "[CENTRAL] Zadatak primljen: " << mission_id
              << " | type=" << mission_type << " | priority=" << priority
              << " | status=" << current_status << std::endl;
    return response;
}

json handle_stop_mission_request(const json& msg)
{
    json response;
    std::string mission_id = msg.value("MISSION_ID", "");
    if (mission_id.empty())
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "MISSING_MISSION_ID";
        return response;
    }

    std::string drone_uri, region_id, status, mission_type;
    {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto q = g_db->prepare("SELECT drone_uri FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); if (!q.fetch(drone_uri))
        {
            response["TYPE"] = "ERROR";
            response["MESSAGE"] = "MISSION_NOT_FOUND";
            return response;
        }
        q = g_db->prepare("SELECT region_id FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(region_id);
        q = g_db->prepare("SELECT status FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(status);
        q = g_db->prepare("SELECT mission_type FROM missions WHERE mission_id = ?;");
        q.execute(mission_id); q.fetch(mission_type);

        if (status == "QUEUED")
        {
            auto cancel = g_db->prepare(R"(
                UPDATE missions SET status = 'CANCELLED', finished_at = CURRENT_TIMESTAMP
                WHERE mission_id = ?;
            )");
            cancel.execute(mission_id);
            response["TYPE"] = "MISSION_CANCELLED";
            response["MISSION_ID"] = mission_id;
            return response;
        }
        if (status != "ACTIVE")
        {
            response["TYPE"] = "ERROR";
            response["MESSAGE"] = "MISSION_NOT_ACTIVE";
            response["STATUS"] = status;
            return response;
        }

        auto u = g_db->prepare("UPDATE missions SET status = 'STOP_REQUESTED' WHERE mission_id = ?;");
        u.execute(mission_id);
    }

    if (mission_type == "FORMATION")
    {
        json commands = json::array();
        {
            std::lock_guard<std::mutex> lock(db_mutex);
            auto q = g_db->prepare(R"(
                SELECT drone_uri
                FROM formation_members
                WHERE mission_id = ? AND status = 'ACTIVE'
                ORDER BY member_index ASC;
            )");
            q.execute(mission_id);
            std::string member;
            while (q.fetch(member))
            {
                json command;
                command["TYPE"] = "STOP_MISSION";
                command["MISSION_ID"] = mission_id;
                command["DRONE_URI"] = member;
                command["REASON"] = "OPERATOR_REQUEST";
                command["FORMATION"] = true;
                commands.push_back(command);
            }
        }

        response["TYPE"] = "STOP_FORMATION_DISPATCH";
        response["MISSION_ID"] = mission_id;
        response["COMMANDS"] = commands;
        response["REGION_ID"] = region_id;
        return response;
    }

    json command;
    command["TYPE"] = "STOP_MISSION";
    command["MISSION_ID"] = mission_id;
    command["DRONE_URI"] = drone_uri;
    command["REASON"] = "OPERATOR_REQUEST";

    response["TYPE"] = "STOP_MISSION_DISPATCH";
    response["MISSION_ID"] = mission_id;
    response["TARGET_DRONE"] = drone_uri;
    response["COMMAND"] = command;
    response["REGION_ID"] = region_id;
    return response;
}

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(tcp::socket socket, ssl::context& context)
        : stream_(std::move(socket), context)
    {
    }

    void start()
    {
        auto self = shared_from_this();
        stream_.async_handshake(ssl::stream_base::server,
            [this, self](boost::system::error_code ec)
            {
                if (!ec)
                {
                    sdpsec::print_tls_session(stream_.native_handle(),
                                              "[CENTRAL][PQC]");
                    read_message();
                }
                else
                {
                    std::cerr << "[CENTRAL][PQC] TLS handshake error: "
                              << ec.message() << std::endl;
                }
            });
    }

private:
    void read_message()
    {
        auto self = shared_from_this();

        boost::asio::async_read_until(stream_, buffer_, "\n",
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
            else if (type == "DRONE_READY")
            {
                response = handle_drone_ready(msg);
            }
            else if (type == "ALARM")
            {
                response = handle_alarm(msg);
            }
            else if (type == "MISSION_SUBMIT" || type == "MISSION_REQUEST")
            {
                response = handle_mission_submit(msg);
            }
            else if (type == "CONTROL_PARAMS_REQUEST")
            {
                response = handle_change_params_request(msg);
            }
            else if (type == "MANUAL_RTB_REQUEST")
            {
                response = handle_manual_rtb_request(msg);
            }
            else if (type == "STOP_MISSION_REQUEST")
            {
                response = handle_stop_mission_request(msg);
            }
            else if (type == "INSPECTION_REPORT")
            {
                response = handle_inspection_report(msg);
            }
            else if (type == "ACK_STOP")
            {
                response = handle_ack_stop(msg);
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

        boost::asio::async_write(stream_, boost::asio::buffer(*out),
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
    ssl::stream<tcp::socket> stream_;
    boost::asio::streambuf buffer_;
};

class CentralServer
{
public:
    CentralServer(boost::asio::io_context& io_context,
                  short port,
                  const std::string& cert_file,
                  const std::string& key_file)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
          context_(ssl::context::tls_server)
    {
        sdpsec::configure_pqc_server(context_, cert_file, key_file);
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
                    std::make_shared<Session>(std::move(socket), context_)->start();
                }

                accept_connection();
            });
    }

private:
    tcp::acceptor acceptor_;
    ssl::context context_;
};

int main(int argc, char* argv[])
{
    if (argc != 2 && argc != 4)
    {
        std::cerr << "Usage: ./central_server <port> [pqc_cert.pem pqc_key.pem]\n";
        return 1;
    }

    try
    {
        g_db.reset(new sqlite::db("central_server.db"));
        init_database();

        const std::string cert_file =
            (argc == 4) ? argv[2] : sdpsec::env_or("SDP_CENTRAL_CERT", "central-cert.pem");
        const std::string key_file =
            (argc == 4) ? argv[3] : sdpsec::env_or("SDP_CENTRAL_KEY", "central-key.pem");

        boost::asio::io_context io_context;
        CentralServer server(io_context, std::atoi(argv[1]), cert_file, key_file);

        std::cout << "[CENTRAL] Centralni server pokrenut na portu "
                  << argv[1] << " | TLS1.3 + X25519MLKEM768 + ML-DSA-44" << std::endl;

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
