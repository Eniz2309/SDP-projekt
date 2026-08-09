// central_server.cpp
// Verzija usklađena sa LV9-10: Boost.Asio + JSON + SQLITE3 prema LV9-10
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <mutex>
#include <memory>
#include <vector>
#include <cstdlib>
#include <sqlite3.h>
#include "json/json.h"
#include "sqlite3_wrapper.h"

using boost::asio::ip::tcp;
using json = nlohmann::json;
namespace sqlite = sqlite3_wrapper;

std::unique_ptr<sqlite::db> g_db;
std::mutex db_mutex;

const int DEFAULT_MAX_DRONES = 3;
const int DEFAULT_VERTICAL_SEPARATION = 2;

void exec_sql(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    g_db->execute(sql);
}

void init_database()
{
    exec_sql("CREATE TABLE IF NOT EXISTS regional_servers (region_id TEXT PRIMARY KEY, base_lat REAL, base_lon REAL, last_seen DATETIME DEFAULT CURRENT_TIMESTAMP);");
    exec_sql("CREATE TABLE IF NOT EXISTS zones (region_id TEXT, zone_id TEXT, center_lat REAL, center_lon REAL, radius_m INTEGER, contours INTEGER, PRIMARY KEY(region_id, zone_id));");
    exec_sql("CREATE TABLE IF NOT EXISTS zone_routes (region_id TEXT, zone_id TEXT, route_id TEXT, route_type TEXT, contour_level INTEGER, radius_m INTEGER, center_lat REAL, center_lon REAL, max_drones INTEGER, vertical_separation INTEGER, PRIMARY KEY(region_id, zone_id, route_id));");
    exec_sql("CREATE TABLE IF NOT EXISTS drones (drone_uri TEXT PRIMARY KEY, region_id TEXT, battery INTEGER, status TEXT, lat REAL, lon REAL, last_seen DATETIME DEFAULT CURRENT_TIMESTAMP);");
    exec_sql("CREATE TABLE IF NOT EXISTS missions (mission_id TEXT PRIMARY KEY, drone_uri TEXT, region_id TEXT, mission_type TEXT, zone TEXT, route_id TEXT, altitude INTEGER, altitude_slot INTEGER, status TEXT, created_at DATETIME DEFAULT CURRENT_TIMESTAMP, finished_at DATETIME);");
    exec_sql("CREATE TABLE IF NOT EXISTS alarms (id INTEGER PRIMARY KEY AUTOINCREMENT, drone_uri TEXT, alarm_type TEXT, message TEXT, created_at DATETIME DEFAULT CURRENT_TIMESTAMP);");
}

bool count_query(const std::string& sql, const std::string& a, const std::string& b, int& count)
{
    auto stmt = g_db->prepare(sql);
    stmt.execute(a, b);
    return stmt.fetch(count);
}

bool count_query3(const std::string& sql, const std::string& a, const std::string& b, const std::string& c, int& count)
{
    auto stmt = g_db->prepare(sql);
    stmt.execute(a, b, c);
    return stmt.fetch(count);
}

bool zone_exists(const std::string& region_id, const std::string& zone_id)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    int count = 0;
    count_query("SELECT COUNT(*) FROM zones WHERE region_id = ? AND zone_id = ?;", region_id, zone_id, count);
    return count > 0;
}

bool route_exists(const std::string& region_id, const std::string& zone_id, const std::string& route_id)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    int count = 0;
    count_query3("SELECT COUNT(*) FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;", region_id, zone_id, route_id, count);
    return count > 0;
}

int get_route_int(const std::string& column, const std::string& region_id, const std::string& zone_id, const std::string& route_id, int def)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare("SELECT " + column + " FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;");
    stmt.execute(region_id, zone_id, route_id);
    int value = def;
    if (stmt.fetch(value)) return value;
    return def;
}

double get_route_double(const std::string& column, const std::string& region_id, const std::string& zone_id, const std::string& route_id, double def)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare("SELECT " + column + " FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;");
    stmt.execute(region_id, zone_id, route_id);
    double value = def;
    if (stmt.fetch(value)) return value;
    return def;
}

std::string get_route_string(const std::string& column, const std::string& region_id, const std::string& zone_id, const std::string& route_id, const std::string& def)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare("SELECT " + column + " FROM zone_routes WHERE region_id = ? AND zone_id = ? AND route_id = ?;");
    stmt.execute(region_id, zone_id, route_id);
    std::string value = def;
    if (stmt.fetch(value)) return value;
    return def;
}

bool assign_altitude_slot(const std::string& region_id, const std::string& zone, const std::string& route_id, int base_altitude, int max_drones, int vertical_sep, int& assigned_altitude, int& assigned_slot)
{
    std::vector<bool> used(max_drones, false);

    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare("SELECT altitude_slot FROM missions WHERE status = 'ACTIVE' AND region_id = ? AND zone = ? AND route_id = ?;");
    stmt.execute(region_id, zone, route_id);

    int slot = 0;
    while (stmt.fetch(slot))
    {
        if (slot >= 0 && slot < max_drones) used[slot] = true;
    }

    for (int i = 0; i < max_drones; i++)
    {
        if (!used[i])
        {
            assigned_slot = i;
            assigned_altitude = base_altitude + i * vertical_sep;
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

    std::lock_guard<std::mutex> lock(db_mutex);

    auto region_stmt = g_db->prepare("INSERT OR REPLACE INTO regional_servers(region_id, base_lat, base_lon, last_seen) VALUES (?, ?, ?, CURRENT_TIMESTAMP);");
    region_stmt.execute(region_id, base_lat, base_lon);

    auto del_zones = g_db->prepare("DELETE FROM zones WHERE region_id = ?;");
    del_zones.execute(region_id);

    auto del_routes = g_db->prepare("DELETE FROM zone_routes WHERE region_id = ?;");
    del_routes.execute(region_id);

    auto zone_stmt = g_db->prepare("INSERT OR REPLACE INTO zones(region_id, zone_id, center_lat, center_lon, radius_m, contours) VALUES (?, ?, ?, ?, ?, ?);");
    auto route_stmt = g_db->prepare("INSERT OR REPLACE INTO zone_routes(region_id, zone_id, route_id, route_type, contour_level, radius_m, center_lat, center_lon, max_drones, vertical_separation) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");

    for (const auto& z : zones)
    {
        std::string zone_id = z.value("ZONE_ID", "");
        double center_lat = z.value("CENTER_LAT", 0.0);
        double center_lon = z.value("CENTER_LON", 0.0);
        int radius_m = z.value("RADIUS_M", 1000);
        int contours = z.value("CONTOURS", 4);

        if (zone_id.empty()) continue;

        zone_stmt.execute(region_id, zone_id, center_lat, center_lon, radius_m, contours);

        for (int level = 1; level <= contours; level++)
        {
            std::string route_id = zone_id + "_K" + std::to_string(level);
            int contour_radius = (radius_m * level) / contours;

            route_stmt.execute(region_id, zone_id, route_id, "CONTOUR", level, contour_radius, center_lat, center_lon, DEFAULT_MAX_DRONES, DEFAULT_VERTICAL_SEPARATION);
        }

        std::string diagonal_id = zone_id + "_DIAGONAL";
        route_stmt.execute(region_id, zone_id, diagonal_id, "CONNECTOR", 0, radius_m, center_lat, center_lon, 1, DEFAULT_VERTICAL_SEPARATION);
    }

    std::cout << "[CENTRAL] Registrovan region " << region_id << " | zona: " << zones.size() << std::endl;
}

void handle_drone_status(const json& msg)
{
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    int battery = msg.value("BATTERY", -1);
    std::string status = msg.value("STATUS", "UNKNOWN");
    double lat = msg.value("LAT", 0.0);
    double lon = msg.value("LON", 0.0);

    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare("INSERT OR REPLACE INTO drones(drone_uri, region_id, battery, status, lat, lon, last_seen) VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);");
    stmt.execute(drone_uri, region_id, battery, status, lat, lon);
}

json handle_mission_finished(const json& msg)
{
    json response;
    std::string mission_id = msg.value("MISSION_ID", "");

    std::lock_guard<std::mutex> lock(db_mutex);
    auto stmt = g_db->prepare("UPDATE missions SET status = 'FINISHED', finished_at = CURRENT_TIMESTAMP WHERE mission_id = ?;");
    stmt.execute(mission_id);

    response["TYPE"] = "ACK_MISSION_FINISHED";
    response["MISSION_ID"] = mission_id;
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
    std::string route_id = msg.value("ROUTE_ID", "AUTO");
    int base_altitude = msg.value("ALTITUDE", 120);

    if (!zone_exists(region_id, zone))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "INVALID_ZONE";
        return response;
    }

    if (route_id.empty() || route_id == "AUTO") route_id = zone + "_K1";

    if (!route_exists(region_id, zone, route_id))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "INVALID_ROUTE";
        return response;
    }

    std::string route_type = get_route_string("route_type", region_id, zone, route_id, "CONTOUR");
    int contour_level = get_route_int("contour_level", region_id, zone, route_id, 1);
    int radius_m = get_route_int("radius_m", region_id, zone, route_id, 100);
    double center_lat = get_route_double("center_lat", region_id, zone, route_id, 0.0);
    double center_lon = get_route_double("center_lon", region_id, zone, route_id, 0.0);
    int max_drones = get_route_int("max_drones", region_id, zone, route_id, DEFAULT_MAX_DRONES);
    int vertical_sep = get_route_int("vertical_separation", region_id, zone, route_id, DEFAULT_VERTICAL_SEPARATION);

    int assigned_altitude = 0;
    int assigned_slot = -1;

    if (!assign_altitude_slot(region_id, zone, route_id, base_altitude, max_drones, vertical_sep, assigned_altitude, assigned_slot))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "ROUTE_CAPACITY_FULL";
        return response;
    }

    {
        std::lock_guard<std::mutex> lock(db_mutex);
        auto stmt = g_db->prepare("INSERT INTO missions(mission_id, drone_uri, region_id, mission_type, zone, route_id, altitude, altitude_slot, status) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'ACTIVE');");
        stmt.execute(mission_id, drone_uri, region_id, mission_type, zone, route_id, assigned_altitude, assigned_slot);
    }

    response["TYPE"] = "MISSION_APPROVED";
    response["MISSION_ID"] = mission_id;
    response["DRONE_URI"] = drone_uri;
    response["MISSION_TYPE"] = mission_type;
    response["ZONE"] = zone;
    response["ROUTE_ID"] = route_id;
    response["ROUTE_TYPE"] = route_type;
    response["CONTOUR_LEVEL"] = contour_level;
    response["CENTER_LAT"] = center_lat;
    response["CENTER_LON"] = center_lon;
    response["RADIUS_M"] = radius_m;
    response["ALTITUDE"] = assigned_altitude;
    response["ALTITUDE_SLOT"] = assigned_slot;
    response["MAX_DRONES_ON_ROUTE"] = max_drones;
    response["VERTICAL_SEPARATION"] = vertical_sep;

    std::cout << "[CENTRAL] Misija odobrena: " << mission_id << " | " << zone << "/" << route_id << " | alt=" << assigned_altitude << " | slot=" << assigned_slot << std::endl;

    return response;
}

json process_message(const json& msg)
{
    json response;
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
    else if (type == "MISSION_REQUEST")
    {
        response = handle_mission_request(msg);
    }
    else if (type == "MISSION_FINISHED")
    {
        response = handle_mission_finished(msg);
    }
    else if (type == "ALARM")
    {
        response["TYPE"] = "ACK";
        response["MESSAGE"] = "ALARM_RECEIVED";
    }
    else
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "UNKNOWN_TYPE";
    }

    return response;
}

void regional_session(tcp::socket socket)
{
    try
    {
        boost::asio::streambuf buffer;

        for (;;)
        {
            boost::system::error_code ec;
            boost::asio::read_until(socket, buffer, "\n", ec);
            if (ec) break;

            std::istream is(&buffer);
            std::string line;
            std::getline(is, line);
            if (line.empty()) continue;

            json response;
            try
            {
                json msg = json::parse(line);
                response = process_message(msg);
            }
            catch (const std::exception& e)
            {
                response["TYPE"] = "ERROR";
                response["MESSAGE"] = e.what();
            }

            std::string out = response.dump() + "\n";
            boost::asio::write(socket, boost::asio::buffer(out));
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[CENTRAL] Session error: " << e.what() << std::endl;
    }
}

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

        boost::asio::io_context io;
        tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), std::atoi(argv[1])));

        std::cout << "[CENTRAL] Listening on port " << argv[1] << std::endl;

        for (;;)
        {
            tcp::socket socket(io);
            acceptor.accept(socket);
            std::cout << "[CENTRAL] Regional connected.\n";
            std::thread(regional_session, std::move(socket)).detach();
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[CENTRAL] Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
