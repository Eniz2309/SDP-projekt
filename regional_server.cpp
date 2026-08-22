// ============================================================
// AUTONOMNI DRONOVI - VERZIJA 14
// Fajl: regional_server.cpp
// Dodano: prijem, lokalno cuvanje i prosljedjivanje prosirene
//         telemetrije: misija, rezim leta, senzori, brzina i smjer.
// ============================================================

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <array>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <atomic>
#include <cmath>

#include <sqlite3.h>
#include "json/json.h"
#include "sqlite3_wrapper.h"
#include "pqc_tls_utils.h"
#include "udp_aead.h"

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;
using boost::asio::ip::udp;
using json = nlohmann::json;
namespace sqlite = sqlite3_wrapper;

struct ZoneConfig
{
    std::string zone_id;
    double center_lat;
    double center_lon;
    int radius_m;
    int contours;
};

std::string REGION_ID;
double BASE_LAT;
double BASE_LON;
std::vector<ZoneConfig> ZONES;

std::unique_ptr<sqlite::db> g_db;
std::mutex db_mutex;

boost::asio::io_context central_io;
std::unique_ptr<ssl::context> central_tls_context;
std::unique_ptr<ssl::stream<tcp::socket>> central_stream;
std::unique_ptr<ssl::context> drone_tls_context;
std::mutex central_mutex;

// Aktivne TCP konekcije dronova.
// Svaki DRONE_URI se mapira na njegov socket, tako da regionalni server moze
// naknadno poslati kontrolnu komandu (npr. STOP_MISSION) bas tom dronu.
// write_mutex sprjecava da se dvije TCP poruke istovremeno upisuju na isti socket.
struct DroneConnection
{
    std::shared_ptr<ssl::stream<tcp::socket>> stream;
    std::array<unsigned char, 32> udp_key;
    std::mutex write_mutex;

    DroneConnection(const std::shared_ptr<ssl::stream<tcp::socket>>& s,
                    const std::array<unsigned char, 32>& key)
        : stream(s), udp_key(key)
    {
    }
};

std::mutex drone_connections_mutex;
std::unordered_map<std::string, std::shared_ptr<DroneConnection>> drone_connections;

void register_drone_connection(const std::string& drone_uri,
                               const std::shared_ptr<DroneConnection>& connection)
{
    if (drone_uri.empty() || drone_uri == "UNKNOWN_DRONE")
        return;

    std::lock_guard<std::mutex> lock(drone_connections_mutex);
    drone_connections[drone_uri] = connection;

    std::cout << "[REGIONAL] TCP konekcija registrovana za "
              << drone_uri << std::endl;
}

void unregister_drone_connection(const std::string& drone_uri,
                                 const std::shared_ptr<DroneConnection>& connection)
{
    if (drone_uri.empty())
        return;

    std::lock_guard<std::mutex> lock(drone_connections_mutex);
    auto it = drone_connections.find(drone_uri);

    if (it != drone_connections.end() && it->second == connection)
    {
        drone_connections.erase(it);
        std::cout << "[REGIONAL] TCP konekcija uklonjena za "
                  << drone_uri << std::endl;
    }
}

bool send_to_drone(const std::string& drone_uri, const json& msg)
{
    std::shared_ptr<DroneConnection> connection;

    {
        std::lock_guard<std::mutex> lock(drone_connections_mutex);
        auto it = drone_connections.find(drone_uri);

        if (it == drone_connections.end())
            return false;

        connection = it->second;
    }

    try
    {
        std::string out = msg.dump() + "\n";
        std::lock_guard<std::mutex> write_lock(connection->write_mutex);
        boost::asio::write(*connection->stream, boost::asio::buffer(out));
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[REGIONAL] Slanje komande dronu "
                  << drone_uri << " nije uspjelo: "
                  << e.what() << std::endl;
        return false;
    }
}

struct FormationMemberControl
{
    std::string drone_uri;
    double offset_north_m;
    double offset_east_m;
};

struct FormationControl
{
    std::string mission_id;
    std::string route_id;
    double center_lat;
    double center_lon;
    double half_size_m;
    int altitude;
    std::vector<FormationMemberControl> members;
    std::atomic<bool> active;

    FormationControl()
        : center_lat(0.0), center_lon(0.0), half_size_m(250.0),
          altitude(120), active(true) {}
};

std::mutex formations_mutex;
std::unordered_map<std::string, std::shared_ptr<FormationControl>> active_formations;

struct FormationGeoPoint
{
    double lat;
    double lon;
};

FormationGeoPoint formation_offset_to_latlon(double center_lat, double center_lon,
                                              double north_m, double east_m)
{
    const double PI = 3.14159265358979323846;
    const double meters_per_deg_lat = 111320.0;
    const double meters_per_deg_lon = 111320.0 * std::cos(center_lat * PI / 180.0);

    FormationGeoPoint p;
    p.lat = center_lat + north_m / meters_per_deg_lat;
    p.lon = center_lon + east_m / meters_per_deg_lon;
    return p;
}

void stop_formation_controller(const std::string& mission_id)
{
    std::lock_guard<std::mutex> lock(formations_mutex);
    auto it = active_formations.find(mission_id);
    if (it != active_formations.end())
    {
        it->second->active = false;
        active_formations.erase(it);
        std::cout << "[REGIONAL][FORMATION] Virtual leader za "
                  << mission_id << " zaustavljen." << std::endl;
    }
}

void formation_controller_loop(const std::shared_ptr<FormationControl>& formation)
{
    // Virtual leader obilazi kvadratnu konturu u lokalnim metrima.
    const double h = formation->half_size_m;
    const double north_points[5] = { h, h, -h, -h, h };
    const double east_points[5]  = { -h, h, h, -h, -h };
    const double STEP_M = 20.0;

    double leader_north = north_points[0];
    double leader_east = east_points[0];
    int target_index = 1;
    unsigned long sequence = 0;

    std::cout << "[REGIONAL][FORMATION] VIRTUAL_LEADER start mission="
              << formation->mission_id
              << " | members=" << formation->members.size()
              << " | altitude=" << formation->altitude << "m" << std::endl;

    while (formation->active)
    {
        double dn = north_points[target_index] - leader_north;
        double de = east_points[target_index] - leader_east;
        double dist = std::sqrt(dn * dn + de * de);

        if (dist <= STEP_M || dist == 0.0)
        {
            leader_north = north_points[target_index];
            leader_east = east_points[target_index];
            target_index++;
            if (target_index >= 5)
                target_index = 1;
        }
        else
        {
            leader_north += STEP_M * dn / dist;
            leader_east += STEP_M * de / dist;
        }

        FormationGeoPoint leader_geo = formation_offset_to_latlon(
            formation->center_lat, formation->center_lon,
            leader_north, leader_east);

        for (const auto& member : formation->members)
        {
            // Offseti su horizontalni; ALTITUDE je isti za sve clanove.
            FormationGeoPoint target_geo = formation_offset_to_latlon(
                formation->center_lat, formation->center_lon,
                leader_north + member.offset_north_m,
                leader_east + member.offset_east_m);

            json update;
            update["TYPE"] = "FORMATION_UPDATE";
            update["MISSION_ID"] = formation->mission_id;
            update["FORMATION_CONTROLLER"] = "REGIONAL_SERVER";
            update["SEQUENCE"] = sequence;
            update["VIRTUAL_LEADER_LAT"] = leader_geo.lat;
            update["VIRTUAL_LEADER_LON"] = leader_geo.lon;
            update["TARGET_LAT"] = target_geo.lat;
            update["TARGET_LON"] = target_geo.lon;
            update["ALTITUDE"] = formation->altitude;
            update["OFFSET_NORTH_M"] = member.offset_north_m;
            update["OFFSET_EAST_M"] = member.offset_east_m;

            if (!send_to_drone(member.drone_uri, update))
            {
                std::cerr << "[REGIONAL][FORMATION] Update nije isporucen "
                          << member.drone_uri << std::endl;
            }
        }

        if ((sequence % 5) == 0)
        {
            std::cout << "[REGIONAL][FORMATION] " << formation->mission_id
                      << " | virtual_leader=" << leader_geo.lat << ","
                      << leader_geo.lon << " | alt=" << formation->altitude
                      << "m" << std::endl;
        }

        ++sequence;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void start_formation_from_commands(const std::string& mission_id,
                                   const std::vector<json>& commands)
{
    if (commands.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(formations_mutex);
        if (active_formations.find(mission_id) != active_formations.end())
            return;
    }

    auto formation = std::make_shared<FormationControl>();
    formation->mission_id = mission_id;
    formation->route_id = commands[0].value("ROUTE_ID", "");
    formation->center_lat = commands[0].value("CENTER_LAT", 0.0);
    formation->center_lon = commands[0].value("CENTER_LON", 0.0);
    formation->half_size_m = commands[0].value("HALF_SIZE_M", 250.0);
    formation->altitude = commands[0].value("ALTITUDE", 120);

    for (const auto& command : commands)
    {
        FormationMemberControl member;
        member.drone_uri = command.value("ASSIGNED_DRONE", "UNKNOWN_DRONE");
        member.offset_north_m = command.value("OFFSET_NORTH_M", 0.0);
        member.offset_east_m = command.value("OFFSET_EAST_M", 0.0);
        formation->members.push_back(member);
    }

    {
        std::lock_guard<std::mutex> lock(formations_mutex);
        active_formations[mission_id] = formation;
    }

    std::thread(formation_controller_loop, formation).detach();
}

void dispatch_assignments(const json& response)
{
    if (!response.contains("ASSIGNMENTS") || !response["ASSIGNMENTS"].is_array())
        return;

    std::unordered_map<std::string, std::vector<json>> formation_commands;

    for (const auto& command : response["ASSIGNMENTS"])
    {
        std::string type = command.value("TYPE", "");
        std::string target = command.value("ASSIGNED_DRONE", "UNKNOWN_DRONE");

        if (type == "START_MISSION")
        {
            if (send_to_drone(target, command))
            {
                std::cout << "[REGIONAL][SCHEDULER] START_MISSION poslan dronu "
                          << target << " | mission="
                          << command.value("MISSION_ID", "UNKNOWN_MISSION") << std::endl;
            }
            else
            {
                std::cerr << "[REGIONAL][SCHEDULER] Nema aktivne TCP konekcije za "
                          << target << "; zadatak nije isporucen." << std::endl;
            }
        }
        else if (type == "START_FORMATION")
        {
            std::string mission_id = command.value("MISSION_ID", "UNKNOWN_MISSION");
            if (send_to_drone(target, command))
            {
                formation_commands[mission_id].push_back(command);
                std::cout << "[REGIONAL][FORMATION] START_FORMATION -> "
                          << target << " | mission=" << mission_id
                          << " | alt=" << command.value("ALTITUDE", 0)
                          << " | offset_east=" << command.value("OFFSET_EAST_M", 0.0)
                          << "m" << std::endl;
            }
            else
            {
                std::cerr << "[REGIONAL][FORMATION] Dron nije povezan: "
                          << target << std::endl;
            }
        }
    }

    for (const auto& entry : formation_commands)
        start_formation_from_commands(entry.first, entry.second);
}

// Watchdog prati kada je posljednji put primljena poruka od svakog drona.
// KEEPALIVE se salje svakih 15 s, pa je timeout namjerno postavljen na 45 s
// (tri keepalive intervala) kako jedan ili dva izgubljena UDP datagrama ne bi
// odmah proglasila dron nedostupnim.
const int WATCHDOG_TIMEOUT_SECONDS = 45;
const int WATCHDOG_CHECK_INTERVAL_SECONDS = 5;

std::mutex watchdog_mutex;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> drone_last_seen;
std::unordered_map<std::string, json> drone_last_status;
std::unordered_set<std::string> connection_lost_reported;

void mark_drone_seen(const json& msg)
{
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    if (drone_uri.empty() || drone_uri == "UNKNOWN_DRONE")
        return;

    std::lock_guard<std::mutex> lock(watchdog_mutex);
    drone_last_seen[drone_uri] = std::chrono::steady_clock::now();
    drone_last_status[drone_uri] = msg;

    // Ako se dron ponovo javi nakon prekida veze, dozvoli novi watchdog alarm
    // samo ako veza ponovo bude izgubljena u buducnosti.
    connection_lost_reported.erase(drone_uri);
}

// Format zona:
// SKENDERIJA:43.8563:18.4131:1000:4,BASCARSIJA:43.8590:18.4310:800:4
std::vector<ZoneConfig> parse_zones_config(const std::string& text)
{
    std::vector<ZoneConfig> zones;

    std::stringstream all(text);
    std::string one_zone;

    while (std::getline(all, one_zone, ','))
    {
        if (one_zone.empty())
            continue;

        std::stringstream ss(one_zone);
        std::vector<std::string> parts;
        std::string part;

        while (std::getline(ss, part, ':'))
        {
            parts.push_back(part);
        }

        if (parts.size() < 4)
        {
            std::cerr << "[REGIONAL] Neispravna zona: " << one_zone << std::endl;
            continue;
        }

        ZoneConfig z;
        z.zone_id = parts[0];
        z.center_lat = std::stod(parts[1]);
        z.center_lon = std::stod(parts[2]);
        z.radius_m = std::stoi(parts[3]);
        z.contours = (parts.size() >= 5) ? std::stoi(parts[4]) : 4;

        zones.push_back(z);
    }

    return zones;
}

bool is_valid_zone(const std::string& zone)
{
    for (const auto& z : ZONES)
    {
        if (z.zone_id == zone)
            return true;
    }

    return false;
}

void print_zones()
{
    std::cout << "[REGIONAL] Zone za " << REGION_ID << ":\n";

    for (const auto& z : ZONES)
    {
        std::cout << "  " << z.zone_id
                  << " | centar: " << z.center_lat << ", " << z.center_lon
                  << " | radius: " << z.radius_m << "m"
                  << " | konture: " << z.contours << std::endl;
    }
}

void exec_sql(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    g_db->execute(sql);
}

void init_database()
{
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
        CREATE TABLE IF NOT EXISTS drones (
            drone_uri TEXT PRIMARY KEY,
            region_id TEXT,
            battery INTEGER,
            status TEXT,
            lat REAL,
            lon REAL,
            altitude INTEGER,
            speed INTEGER DEFAULT 0,
            direction TEXT DEFAULT '',
            route_id TEXT,
            mission_id TEXT DEFAULT '',
            mission_type TEXT DEFAULT '',
            flight_mode TEXT DEFAULT 'UNKNOWN',
            sensor_status TEXT DEFAULT 'UNKNOWN',
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS keepalive_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            drone_uri TEXT,
            battery INTEGER,
            status TEXT,
            lat REAL,
            lon REAL,
            altitude INTEGER,
            speed INTEGER DEFAULT 0,
            direction TEXT DEFAULT '',
            route_id TEXT,
            mission_id TEXT DEFAULT '',
            mission_type TEXT DEFAULT '',
            flight_mode TEXT DEFAULT 'UNKNOWN',
            sensor_status TEXT DEFAULT 'UNKNOWN',
            received_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )");

    // Migracija postojecih baza nastalih prije prosirene telemetrije.
    auto add_column_if_missing = [](const std::string& sql)
    {
        try
        {
            exec_sql(sql);
        }
        catch (const std::exception& e)
        {
            std::string error = e.what();
            if (error.find("duplicate column name") == std::string::npos)
                throw;
        }
    };

    for (const std::string& table : {std::string("drones"), std::string("keepalive_log")})
    {
        add_column_if_missing("ALTER TABLE " + table + " ADD COLUMN speed INTEGER DEFAULT 0;");
        add_column_if_missing("ALTER TABLE " + table + " ADD COLUMN direction TEXT DEFAULT '';" );
        add_column_if_missing("ALTER TABLE " + table + " ADD COLUMN mission_id TEXT DEFAULT '';" );
        add_column_if_missing("ALTER TABLE " + table + " ADD COLUMN mission_type TEXT DEFAULT '';" );
        add_column_if_missing("ALTER TABLE " + table + " ADD COLUMN flight_mode TEXT DEFAULT 'UNKNOWN';" );
        add_column_if_missing("ALTER TABLE " + table + " ADD COLUMN sensor_status TEXT DEFAULT 'UNKNOWN';" );
    }

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

void save_zones()
{
    std::lock_guard<std::mutex> lock(db_mutex);

    auto del = g_db->prepare("DELETE FROM zones WHERE region_id = ?;");
    del.execute(REGION_ID);

    auto stmt = g_db->prepare(R"(
        INSERT OR REPLACE INTO zones
        (region_id, zone_id, center_lat, center_lon, radius_m, contours)
        VALUES (?, ?, ?, ?, ?, ?);
    )");

    for (const auto& z : ZONES)
    {
        stmt.execute(REGION_ID, z.zone_id, z.center_lat, z.center_lon, z.radius_m, z.contours);
    }
}

void save_drone_status(const json& msg)
{
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    int battery = msg.value("BATTERY", -1);
    std::string status = msg.value("STATUS", "UNKNOWN");
    double lat = msg.value("LAT", 0.0);
    double lon = msg.value("LON", 0.0);
    int altitude = msg.value("ALTITUDE", 0);
    int speed = msg.value("SPEED", 0);
    std::string direction = msg.value("DIRECTION", "");
    std::string route_id = msg.value("ROUTE_ID", "");
    std::string mission_id = msg.value("MISSION_ID", "");
    std::string mission_type = msg.value("MISSION_TYPE", "");
    std::string flight_mode = msg.value("FLIGHT_MODE", "UNKNOWN");
    std::string sensor_status = msg.value("SENSOR_STATUS", "UNKNOWN");

    mark_drone_seen(msg);

    std::lock_guard<std::mutex> lock(db_mutex);

    auto upsert_stmt = g_db->prepare(R"(
        INSERT OR REPLACE INTO drones
        (drone_uri, region_id, battery, status, lat, lon, altitude, speed, direction,
         route_id, mission_id, mission_type, flight_mode, sensor_status, last_seen)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
    )");

    upsert_stmt.execute(drone_uri, REGION_ID, battery, status, lat, lon, altitude, speed, direction,
                        route_id, mission_id, mission_type, flight_mode, sensor_status);

    auto log_stmt = g_db->prepare(R"(
        INSERT INTO keepalive_log
        (drone_uri, battery, status, lat, lon, altitude, speed, direction, route_id,
         mission_id, mission_type, flight_mode, sensor_status)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )");

    log_stmt.execute(drone_uri, battery, status, lat, lon, altitude, speed, direction, route_id,
                     mission_id, mission_type, flight_mode, sensor_status);

    std::cout << "[REGIONAL] Status drona: " << drone_uri
              << " | " << status
              << " | mission=" << (mission_id.empty() ? "NONE" : mission_id)
              << " | mode=" << flight_mode
              << " | sensor=" << sensor_status
              << " | battery=" << battery << "%"
              << " | route=" << route_id << std::endl;
}

void save_alarm(const json& msg)
{
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string alarm_type = msg.value("ALARM_TYPE", "UNKNOWN_ALARM");
    std::string message = msg.value("MESSAGE", "");

    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(R"(
        INSERT INTO alarms(drone_uri, alarm_type, message)
        VALUES (?, ?, ?);
    )");

    stmt.execute(drone_uri, alarm_type, message);

    std::cout << "[REGIONAL] Alarm: " << drone_uri
              << " | " << alarm_type << std::endl;
}

void copy_telemetry_context(json& dst, const json& src)
{
    dst["SPEED"] = src.value("SPEED", 0);
    dst["DIRECTION"] = src.value("DIRECTION", "");
    dst["MISSION_ID"] = src.value("MISSION_ID", "");
    dst["MISSION_TYPE"] = src.value("MISSION_TYPE", "");
    dst["FLIGHT_MODE"] = src.value("FLIGHT_MODE", "UNKNOWN");
    dst["SENSOR_STATUS"] = src.value("SENSOR_STATUS", "UNKNOWN");
}

json send_to_central(json msg);

void set_local_connection_lost(const std::string& drone_uri)
{
    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(R"(
        UPDATE drones
        SET status = 'CONNECTION_LOST'
        WHERE drone_uri = ?;
    )");

    stmt.execute(drone_uri);
}

void watchdog_loop()
{
    std::cout << "[REGIONAL][WATCHDOG] Pokrenut. Timeout="
              << WATCHDOG_TIMEOUT_SECONDS << "s, provjera svakih "
              << WATCHDOG_CHECK_INTERVAL_SECONDS << "s." << std::endl;

    for (;;)
    {
        std::this_thread::sleep_for(
            std::chrono::seconds(WATCHDOG_CHECK_INTERVAL_SECONDS));

        std::vector<std::pair<std::string, json>> timed_out;
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(watchdog_mutex);

            for (const auto& entry : drone_last_seen)
            {
                const std::string& drone_uri = entry.first;
                long elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - entry.second).count();

                if (elapsed >= WATCHDOG_TIMEOUT_SECONDS &&
                    connection_lost_reported.find(drone_uri) == connection_lost_reported.end())
                {
                    connection_lost_reported.insert(drone_uri);

                    json last_status;
                    auto status_it = drone_last_status.find(drone_uri);
                    if (status_it != drone_last_status.end())
                        last_status = status_it->second;

                    timed_out.push_back(std::make_pair(drone_uri, last_status));
                }
            }
        }

        for (const auto& lost : timed_out)
        {
            const std::string& drone_uri = lost.first;
            json status_msg = lost.second;

            std::cout << "[REGIONAL][WATCHDOG] CONNECTION_LOST: "
                      << drone_uri << " nije poslao podatke najmanje "
                      << WATCHDOG_TIMEOUT_SECONDS << " sekundi." << std::endl;

            set_local_connection_lost(drone_uri);

            // Centralnom prvo azuriraj stanje drona kako bi globalni registar
            // odmah pokazao CONNECTION_LOST.
            status_msg["TYPE"] = "DRONE_STATUS";
            status_msg["DRONE_URI"] = drone_uri;
            status_msg["STATUS"] = "CONNECTION_LOST";

            try
            {
                send_to_central(status_msg);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[REGIONAL][WATCHDOG] Ne mogu poslati DRONE_STATUS centralnom: "
                          << e.what() << std::endl;
            }

            json alarm;
            alarm["TYPE"] = "ALARM";
            alarm["DRONE_URI"] = drone_uri;
            alarm["ALARM_TYPE"] = "CONNECTION_LOST";
            alarm["MESSAGE"] = "No telemetry or keepalive received for 45 seconds";

            save_alarm(alarm);

            try
            {
                json central_response = send_to_central(alarm);
                std::cout << "[REGIONAL][WATCHDOG] Centralni odgovor: "
                          << central_response.dump() << std::endl;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[REGIONAL][WATCHDOG] Ne mogu poslati alarm centralnom: "
                          << e.what() << std::endl;
            }
        }
    }
}

json send_to_central(json msg)
{
    std::lock_guard<std::mutex> lock(central_mutex);

    msg["REGION_ID"] = REGION_ID;

    std::string request = msg.dump() + "\n";

    boost::asio::write(*central_stream, boost::asio::buffer(request));

    boost::asio::streambuf buffer;
    boost::asio::read_until(*central_stream, buffer, "\n");

    std::istream is(&buffer);
    std::string response;
    std::getline(is, response);

    return json::parse(response);
}

json handle_drone_message(json msg)
{
    std::string type = msg.value("TYPE", "UNKNOWN");
    json response;

    if (type == "REGISTER_REQ")
    {
        const std::string drone_uri = msg.value("DRONE_URI", "");
        const std::string token = msg.value("TOKEN", "");
        const std::string session_drone_uri =
            msg.value("_SESSION_DRONE_URI", "");

        if (!session_drone_uri.empty() && session_drone_uri != drone_uri)
        {
            response["TYPE"] = "REGISTER_ERROR";
            response["DRONE_URI"] = drone_uri;
            response["MESSAGE"] = "TLS session already bound to another drone URI";
            response["REASON"] = "SESSION_ALREADY_BOUND";
            return response;
        }

        json register_msg;
        register_msg["TYPE"] = "DRONE_REGISTER";
        register_msg["DRONE_URI"] = drone_uri;
        register_msg["TOKEN"] = token;

        json central_response = send_to_central(register_msg);

        if (central_response.value("TYPE", "") == "DRONE_REGISTER_OK")
        {
            json status_msg;
            status_msg["TYPE"] = "DRONE_STATUS";
            status_msg["DRONE_URI"] = drone_uri;
            status_msg["BATTERY"] = msg.value("BATTERY", -1);
            status_msg["STATUS"] = "REGISTERED";
            status_msg["LAT"] = msg.value("LAT", 0.0);
            status_msg["LON"] = msg.value("LON", 0.0);
            status_msg["ALTITUDE"] = msg.value("ALTITUDE", 0);
            status_msg["ROUTE_ID"] = "";
            copy_telemetry_context(status_msg, msg);

            save_drone_status(status_msg);
            send_to_central(status_msg);

            response["TYPE"] = "REGISTER_ACK";
            response["DRONE_URI"] = drone_uri;
            response["MESSAGE"] = "Drone URI/token accepted by central registry";
        }
        else
        {
            response["TYPE"] = "REGISTER_ERROR";
            response["DRONE_URI"] = drone_uri;
            response["MESSAGE"] = "Registration rejected by central registry";
            response["REASON"] = central_response.value("REASON", "UNKNOWN_REASON");
        }
    }
    else if (type == "AUTH_REQ")
    {
        const std::string drone_uri = msg.value("DRONE_URI", "");
        const std::string token = msg.value("TOKEN", "");
        const std::string session_drone_uri =
            msg.value("_SESSION_DRONE_URI", "");

        if (session_drone_uri.empty() || session_drone_uri != drone_uri)
        {
            response["TYPE"] = "AUTH_ERROR";
            response["DRONE_URI"] = drone_uri;
            response["MESSAGE"] = "Authentication rejected";
            response["REASON"] = "SESSION_URI_MISMATCH";
        }
        else
        {
            json auth_msg;
            auth_msg["TYPE"] = "DRONE_AUTH";
            auth_msg["DRONE_URI"] = drone_uri;
            auth_msg["TOKEN"] = token;

            json central_response = send_to_central(auth_msg);

            if (central_response.value("TYPE", "") == "DRONE_AUTH_OK")
            {
                response["TYPE"] = "AUTH_ACK";
                response["DRONE_URI"] = drone_uri;
                response["MESSAGE"] = "Authentication successful";
            }
            else
            {
                response["TYPE"] = "AUTH_ERROR";
                response["DRONE_URI"] = drone_uri;
                response["MESSAGE"] = "Authentication rejected by central registry";
                response["REASON"] = central_response.value("REASON", "UNKNOWN_REASON");
            }
        }
    }
    else if (type == "KEEPALIVE" || type == "TELEMETRY")
    {
        msg["TYPE"] = "DRONE_STATUS";

        save_drone_status(msg);

        json central_response = send_to_central(msg);

        response["TYPE"] = "ACK_STATUS";
        response["CENTRAL_RESPONSE"] = central_response;
    }
    else if (type == "ALARM")
    {
        save_alarm(msg);

        json central_response = send_to_central(msg);

        if (central_response.value("TYPE", "") == "RETURN_TO_BASE")
        {
            response = central_response;

            std::cout << "[REGIONAL] Prosljeđujem RETURN_TO_BASE komandu dronu "
                      << msg.value("DRONE_URI", "UNKNOWN_DRONE")
                      << std::endl;
        }
        else
        {
            response["TYPE"] = "ACK_ALARM";
            response["CENTRAL_RESPONSE"] = central_response;
        }
    }
    else if (type == "DRONE_READY")
    {
        std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
        json status_msg;
        status_msg["TYPE"] = "DRONE_STATUS";
        status_msg["DRONE_URI"] = drone_uri;
        status_msg["BATTERY"] = msg.value("BATTERY", -1);
        status_msg["STATUS"] = "AVAILABLE";
        status_msg["LAT"] = msg.value("LAT", 0.0);
        status_msg["LON"] = msg.value("LON", 0.0);
        status_msg["ALTITUDE"] = msg.value("ALTITUDE", 0);
        status_msg["ROUTE_ID"] = "";
        copy_telemetry_context(status_msg, msg);
        save_drone_status(status_msg);

        msg["TYPE"] = "DRONE_READY";
        response = send_to_central(msg);
        dispatch_assignments(response);
    }
    else if (type == "MISSION_SUBMIT" || type == "MISSION_REQUEST")
    {
        std::string zone = msg.value("ZONE", "");
        if (!is_valid_zone(zone))
        {
            response["TYPE"] = "MISSION_REJECTED";
            response["MISSION_ID"] = msg.value("MISSION_ID", "UNKNOWN_MISSION");
            response["REASON"] = "INVALID_ZONE_ON_REGIONAL";
        }
        else
        {
            msg["TYPE"] = "MISSION_SUBMIT";
            response = send_to_central(msg);
            dispatch_assignments(response);
        }
    }
    else if (type == "CONTROL_PARAMS_REQUEST")
    {
        response = send_to_central(msg);

        if (response.value("TYPE", "") == "CHANGE_PARAMS_DISPATCH")
        {
            std::string target = response.value("TARGET_DRONE", "UNKNOWN_DRONE");
            json command;
            if (response.contains("COMMAND"))
                command = response["COMMAND"];

            if (!send_to_drone(target, command))
            {
                response["TYPE"] = "CONTROL_DELIVERY_FAILED";
                response["DRONE_URI"] = target;
                response["REASON"] = "TARGET_DRONE_NOT_CONNECTED";
            }
            else
            {
                response["DELIVERED"] = true;
                std::cout << "[REGIONAL][CONTROL] CHANGE_PARAMS poslan dronu "
                          << target << std::endl;
            }
        }
    }
    else if (type == "MANUAL_RTB_REQUEST")
    {
        response = send_to_central(msg);

        if (response.value("TYPE", "") == "RETURN_TO_BASE_DISPATCH")
        {
            std::string target = response.value("TARGET_DRONE", "UNKNOWN_DRONE");
            json command;
            if (response.contains("COMMAND"))
                command = response["COMMAND"];

            if (!send_to_drone(target, command))
            {
                response["TYPE"] = "RTB_DELIVERY_FAILED";
                response["DRONE_URI"] = target;
                response["REASON"] = "TARGET_DRONE_NOT_CONNECTED";
            }
            else
            {
                response["DELIVERED"] = true;
                std::cout << "[REGIONAL][CONTROL] RETURN_TO_BASE poslan dronu "
                          << target << std::endl;
            }

            // Ako je RTB prekinuo aktivnu misiju, scheduler moze odmah
            // dodijeliti neki QUEUED zadatak drugom slobodnom dronu.
            dispatch_assignments(response);
        }
    }
    else if (type == "STOP_MISSION_REQUEST")
    {
        response = send_to_central(msg);
        if (response.value("TYPE", "") == "STOP_MISSION_DISPATCH")
        {
            std::string target = response.value("TARGET_DRONE", "UNKNOWN_DRONE");
            json command;
            if (response.contains("COMMAND"))
                command = response["COMMAND"];
            if (!send_to_drone(target, command))
            {
                response["TYPE"] = "STOP_MISSION_DELIVERY_FAILED";
                response["REASON"] = "TARGET_DRONE_NOT_CONNECTED";
            }
            else
            {
                std::cout << "[REGIONAL] STOP_MISSION poslan dronu " << target
                          << " na zahtjev operatora." << std::endl;
            }
        }
        else if (response.value("TYPE", "") == "STOP_FORMATION_DISPATCH")
        {
            std::string mission_id = response.value("MISSION_ID", "UNKNOWN_MISSION");
            stop_formation_controller(mission_id);

            int delivered = 0;
            if (response.contains("COMMANDS") && response["COMMANDS"].is_array())
            {
                for (const auto& command : response["COMMANDS"])
                {
                    std::string target = command.value("DRONE_URI", "UNKNOWN_DRONE");
                    if (send_to_drone(target, command))
                        ++delivered;
                }
            }
            response["STOP_COMMANDS_DELIVERED"] = delivered;
            std::cout << "[REGIONAL][FORMATION] STOP poslan za " << mission_id
                      << " | delivered=" << delivered << std::endl;
        }
    }
    else if (type == "INSPECTION_REPORT")
    {
        std::cout << "[REGIONAL] INSPECTION_REPORT "
                  << msg.value("POINT_ID", "UNKNOWN_POINT")
                  << " od " << msg.value("DRONE_URI", "UNKNOWN_DRONE")
                  << std::endl;

        response = send_to_central(msg);
    }
    else if (type == "MISSION_FINISHED")
    {
        json status_msg = msg;
        status_msg["TYPE"] = "DRONE_STATUS";
        status_msg["STATUS"] = "AVAILABLE";
        status_msg["ROUTE_ID"] = "";
        status_msg["MISSION_ID"] = "";
        status_msg["MISSION_TYPE"] = "";
        status_msg["FLIGHT_MODE"] = "STANDBY";
        save_drone_status(status_msg);

        response = send_to_central(msg);
        dispatch_assignments(response);
    }
    else if (type == "ACK_STOP")
    {
        std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
        std::string mission_id = msg.value("MISSION_ID", "UNKNOWN_MISSION");

        std::cout << "[REGIONAL] ACK_STOP od " << drone_uri
                  << " za misiju " << mission_id << std::endl;

        json status_msg;
        status_msg["TYPE"] = "DRONE_STATUS";
        status_msg["DRONE_URI"] = drone_uri;
        status_msg["BATTERY"] = msg.value("BATTERY", -1);
        status_msg["STATUS"] = "IDLE";
        status_msg["LAT"] = msg.value("LAT", 0.0);
        status_msg["LON"] = msg.value("LON", 0.0);
        status_msg["ALTITUDE"] = msg.value("ALTITUDE", 0);
        status_msg["ROUTE_ID"] = "";
        copy_telemetry_context(status_msg, msg);
        status_msg["MISSION_ID"] = "";
        status_msg["MISSION_TYPE"] = "";
        status_msg["FLIGHT_MODE"] = "STANDBY";
        save_drone_status(status_msg);

        response = send_to_central(msg);
        dispatch_assignments(response);
    }
    else if (type == "ACK_PARAMS")
    {
        std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");

        json status_msg;
        status_msg["TYPE"] = "DRONE_STATUS";
        status_msg["DRONE_URI"] = drone_uri;
        status_msg["BATTERY"] = msg.value("BATTERY", -1);
        status_msg["STATUS"] = msg.value("STATUS", "UNKNOWN");
        status_msg["LAT"] = msg.value("LAT", 0.0);
        status_msg["LON"] = msg.value("LON", 0.0);
        status_msg["ALTITUDE"] = msg.value("ALTITUDE", 0);
        status_msg["ROUTE_ID"] = msg.value("ROUTE_ID", "");
        copy_telemetry_context(status_msg, msg);

        save_drone_status(status_msg);
        json central_response = send_to_central(status_msg);

        response["TYPE"] = "ACK_PARAMS_SAVED";
        response["DRONE_URI"] = drone_uri;
        response["ALTITUDE"] = msg.value("ALTITUDE", 0);
        response["SPEED"] = msg.value("SPEED", 0);
        response["DIRECTION"] = msg.value("DIRECTION", "");
        response["CENTRAL_RESPONSE"] = central_response;

        std::cout << "[REGIONAL][CONTROL] ACK_PARAMS od " << drone_uri
                  << " | altitude=" << msg.value("ALTITUDE", 0)
                  << " speed=" << msg.value("SPEED", 0)
                  << " direction=" << msg.value("DIRECTION", "")
                  << std::endl;
    }
    else if (type == "ACK_RTB")
    {
        // Potvrda da je dron primio komandu za povratak u bazu.
        json status_msg;
        status_msg["TYPE"] = "DRONE_STATUS";
        status_msg["DRONE_URI"] = msg.value("DRONE_URI", "UNKNOWN_DRONE");
        status_msg["BATTERY"] = msg.value("BATTERY", -1);
        status_msg["STATUS"] = "RETURN_TO_BASE";
        status_msg["LAT"] = msg.value("BASE_LAT", 0.0);
        status_msg["LON"] = msg.value("BASE_LON", 0.0);
        status_msg["ALTITUDE"] = msg.value("ALTITUDE", 0);
        status_msg["ROUTE_ID"] = "";
        copy_telemetry_context(status_msg, msg);
        status_msg["MISSION_ID"] = "";
        status_msg["MISSION_TYPE"] = "";
        status_msg["FLIGHT_MODE"] = "RTB";

        save_drone_status(status_msg);
        json central_response = send_to_central(status_msg);

        response["TYPE"] = "ACK_RTB_SAVED";
        response["DRONE_URI"] = msg.value("DRONE_URI", "UNKNOWN_DRONE");
        response["STATUS"] = "RETURN_TO_BASE";
        response["CENTRAL_RESPONSE"] = central_response;

        std::cout << "[REGIONAL][CONTROL] ACK_RTB od "
                  << msg.value("DRONE_URI", "UNKNOWN_DRONE")
                  << " | baza=" << msg.value("BASE_LAT", 0.0)
                  << "," << msg.value("BASE_LON", 0.0) << std::endl;
    }
    else
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "Unknown message type on regional server";
    }

    return response;
}

void drone_session(const std::shared_ptr<DroneConnection>& connection)
{
    std::string registered_drone_uri;
    bool authenticated = false;

    try
    {
        boost::asio::streambuf buffer;

        for (;;)
        {
            boost::system::error_code ec;
            boost::asio::read_until(*connection->stream, buffer, "\n", ec);

            if (ec)
            {
                std::cout << "[REGIONAL] Dron prekinuo TCP konekciju";
                if (!registered_drone_uri.empty())
                    std::cout << ": " << registered_drone_uri;
                std::cout << ".\n";
                break;
            }

            std::istream is(&buffer);
            std::string line;
            std::getline(is, line);

            if (line.empty())
                continue;

            std::cout << "[REGIONAL] Poruka od drona: " << line << std::endl;

            json msg = json::parse(line);

            // Server-side binding: klijent ne moze AUTH_REQ-om promijeniti URI
            // nakon sto je ova TLS sesija uspjesno registrovana.
            msg["_SESSION_DRONE_URI"] = registered_drone_uri;

            const std::string type = msg.value("TYPE", "");
            const bool requires_drone_auth =
                (type == "KEEPALIVE" || type == "TELEMETRY" ||
                 type == "ALARM" || type == "DRONE_READY" ||
                 type == "INSPECTION_REPORT" || type == "MISSION_FINISHED" ||
                 type == "ACK_STOP" || type == "ACK_PARAMS" ||
                 type == "ACK_RTB");

            json response;
            if (requires_drone_auth && !authenticated)
            {
                response["TYPE"] = "AUTH_REQUIRED";
                response["REASON"] = "DRONE_SESSION_NOT_AUTHENTICATED";
            }
            else
            {
                response = handle_drone_message(msg);
            }

            if (msg.value("TYPE", "") == "REGISTER_REQ" &&
                response.value("TYPE", "") == "REGISTER_ACK")
            {
                registered_drone_uri = msg.value("DRONE_URI", "");
            }

            if (msg.value("TYPE", "") == "AUTH_REQ" &&
                response.value("TYPE", "") == "AUTH_ACK")
            {
                authenticated = true;
                register_drone_connection(registered_drone_uri, connection);
            }

            std::string out = response.dump() + "\n";
            {
                std::lock_guard<std::mutex> write_lock(connection->write_mutex);
                boost::asio::write(*connection->stream, boost::asio::buffer(out));
            }
        }
    }
    catch (const sqlite::exception& e)
    {
        std::cerr << "[REGIONAL] SQLite greška u sesiji: "
                  << e.what() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[REGIONAL] Drone session error: "
                  << e.what() << std::endl;
    }

    unregister_drone_connection(registered_drone_uri, connection);
}

// UDP listener za periodicke poruke drona.
// Jedan UDP datagram sadrzi tacno jedan JSON objekat (TELEMETRY ili KEEPALIVE).
void start_udp_server(unsigned short port)
{
    try
    {
        boost::asio::io_context io_context;
        udp::socket socket(io_context, udp::endpoint(udp::v4(), port));

        std::cout << "[REGIONAL] UDP telemetry/keepalive server slusa na portu "
                  << port << std::endl;

        for (;;)
        {
            std::array<char, 8192> data{};
            udp::endpoint sender_endpoint;
            boost::system::error_code ec;

            std::size_t length = socket.receive_from(
                boost::asio::buffer(data), sender_endpoint, 0, ec);

            if (ec)
            {
                std::cerr << "[REGIONAL] UDP receive error: "
                          << ec.message() << std::endl;
                continue;
            }

            try
            {
                std::string received(data.data(), length);
                json envelope = json::parse(received);

                if (envelope.value("TYPE", "") != "PQC_UDP")
                {
                    std::cerr << "[REGIONAL][UDP][SECURITY] Odbijen nešifrovan UDP datagram."
                              << std::endl;
                    continue;
                }

                const std::string drone_uri =
                    envelope.value("DRONE_URI", "UNKNOWN_DRONE");

                std::shared_ptr<DroneConnection> connection;
                {
                    std::lock_guard<std::mutex> lock(drone_connections_mutex);
                    auto it = drone_connections.find(drone_uri);
                    if (it != drone_connections.end())
                        connection = it->second;
                }

                if (!connection)
                {
                    std::cerr << "[REGIONAL][UDP][SECURITY] Nema aktivnog PQC TLS ključa za "
                              << drone_uri << std::endl;
                    continue;
                }

                json msg = sdpsec::decrypt_udp_envelope(envelope,
                                                        connection->udp_key);

                if (msg.value("DRONE_URI", "") != drone_uri)
                {
                    std::cerr << "[REGIONAL][UDP][SECURITY] DRONE_URI mismatch."
                              << std::endl;
                    continue;
                }

                std::string original_type = msg.value("TYPE", "UNKNOWN");

                if (original_type != "KEEPALIVE" && original_type != "TELEMETRY")
                {
                    std::cerr << "[REGIONAL][UDP] Ignorisana poruka tipa "
                              << original_type << std::endl;
                    continue;
                }

                std::cout << "[REGIONAL][UDP][AES-256-GCM] " << original_type
                          << " od " << drone_uri
                          << " | " << sender_endpoint.address().to_string()
                          << ":" << sender_endpoint.port() << std::endl;

                msg["TYPE"] = "DRONE_STATUS";
                save_drone_status(msg);
                send_to_central(msg);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[REGIONAL] Invalid UDP message: "
                          << e.what() << std::endl;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[REGIONAL] UDP server error: "
                  << e.what() << std::endl;
    }
}

void start_drone_server(unsigned short port)
{
    boost::asio::io_context io_context;
    tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));

    std::cout << "[REGIONAL] PQC TLS server sluša dronove/operatora na portu "
              << port << std::endl;

    for (;;)
    {
        try
        {
            auto stream =
                std::make_shared<ssl::stream<tcp::socket>>(io_context,
                                                           *drone_tls_context);

            acceptor.accept(stream->next_layer());
            stream->handshake(ssl::stream_base::server);

            sdpsec::print_tls_session(stream->native_handle(),
                                      "[REGIONAL][PQC][CLIENT]");

            const auto udp_key =
                sdpsec::export_udp_key(stream->native_handle());

            auto connection =
                std::make_shared<DroneConnection>(stream, udp_key);
            std::thread(drone_session, connection).detach();
        }
        catch (const std::exception& e)
        {
            std::cerr << "[REGIONAL][PQC] TLS accept/handshake error: "
                      << e.what() << std::endl;
        }
    }
}

void connect_to_central(const std::string& host, const std::string& port)
{
    tcp::resolver resolver(central_io);
    auto endpoints = resolver.resolve(host, port);

    central_stream.reset(
        new ssl::stream<tcp::socket>(central_io, *central_tls_context));

    boost::asio::connect(central_stream->next_layer(), endpoints);
    central_stream->handshake(ssl::stream_base::client);

    sdpsec::print_tls_session(central_stream->native_handle(),
                              "[REGIONAL][PQC][CENTRAL]");
    std::cout << "[REGIONAL] Regionalni server povezan na centralni server preko PQC TLS-a.\n";

    json register_msg;
    register_msg["TYPE"] = "REGION_REGISTER";
    register_msg["REGION_ID"] = REGION_ID;
    register_msg["BASE_LAT"] = BASE_LAT;
    register_msg["BASE_LON"] = BASE_LON;
    register_msg["ZONES"] = json::array();

    for (const auto& z : ZONES)
    {
        json zone;
        zone["ZONE_ID"] = z.zone_id;
        zone["CENTER_LAT"] = z.center_lat;
        zone["CENTER_LON"] = z.center_lon;
        zone["RADIUS_M"] = z.radius_m;
        zone["CONTOURS"] = z.contours;

        register_msg["ZONES"].push_back(zone);
    }

    json response = send_to_central(register_msg);

    std::cout << "[REGIONAL] Odgovor centralnog servera: "
              << response.dump() << std::endl;
}

int main(int argc, char* argv[])
{
    if (argc != 9)
    {
        std::cerr << "Usage: ./regional_server <region_id> <central_host> <central_port> "
                  << "<drone_tcp_port> <drone_udp_port> <base_lat> <base_lon> <zones_config>\n\n";

        std::cerr << "Primjer:\n";
        std::cerr << "./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 8001 "
                  << "43.8563 18.4131 "
                  << "SKENDERIJA:43.8563:18.4131:1000:4,BASCARSIJA:43.8590:18.4310:800:4\n";
        return 1;
    }

    REGION_ID = argv[1];
    std::string central_host = argv[2];
    std::string central_port = argv[3];
    unsigned short drone_tcp_port = static_cast<unsigned short>(std::atoi(argv[4]));
    unsigned short drone_udp_port = static_cast<unsigned short>(std::atoi(argv[5]));
    BASE_LAT = std::stod(argv[6]);
    BASE_LON = std::stod(argv[7]);
    ZONES = parse_zones_config(argv[8]);

    if (ZONES.empty())
    {
        std::cerr << "[REGIONAL] Nema validnih zona.\n";
        return 1;
    }

    try
    {
        std::string db_name = REGION_ID + "_regional_server.db";

        g_db.reset(new sqlite::db(db_name));
        init_database();
        save_zones();
        print_zones();

        const std::string central_cert =
            sdpsec::env_or("SDP_CENTRAL_CERT", "central-cert.pem");
        const std::string regional_cert =
            sdpsec::env_or("SDP_REGIONAL_CERT", "regional-cert.pem");
        const std::string regional_key =
            sdpsec::env_or("SDP_REGIONAL_KEY", "regional-key.pem");

        central_tls_context.reset(new ssl::context(ssl::context::tls_client));
        sdpsec::configure_pqc_client(*central_tls_context, central_cert);

        drone_tls_context.reset(new ssl::context(ssl::context::tls_server));
        sdpsec::configure_pqc_server(*drone_tls_context,
                                     regional_cert,
                                     regional_key);

        connect_to_central(central_host, central_port);

        // UDP radi paralelno sa postojecim TCP listenerom.
        std::thread udp_thread(start_udp_server, drone_udp_port);
        udp_thread.detach();

        std::thread watchdog_thread(watchdog_loop);
        watchdog_thread.detach();

        start_drone_server(drone_tcp_port);
    }
    catch (const sqlite::exception& e)
    {
        std::cerr << "[REGIONAL] SQLite greška: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[REGIONAL] Greška: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
