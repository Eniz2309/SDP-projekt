// regional_server.cpp
// Regionalni server za autonomne dronove.
// Registruje zone na centralni server, prima dronove, čuva lokalni status,
// validira zone i prosljeđuje zahtjeve centralnom serveru.
// Ako centralni vrati RETURN_TO_BASE, regionalni komandu prosljeđuje dronu.

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdlib>
#include <sstream>
#include <vector>

#include <sqlite3.h>
#include "json/json.h"
#include "sqlite3_wrapper.h"

using boost::asio::ip::tcp;
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
std::unique_ptr<tcp::socket> central_socket;
std::mutex central_mutex;

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
            route_id TEXT,
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
            route_id TEXT,
            received_at DATETIME DEFAULT CURRENT_TIMESTAMP
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
    std::string route_id = msg.value("ROUTE_ID", "");

    std::lock_guard<std::mutex> lock(db_mutex);

    auto upsert_stmt = g_db->prepare(R"(
        INSERT OR REPLACE INTO drones
        (drone_uri, region_id, battery, status, lat, lon, altitude, route_id, last_seen)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
    )");

    upsert_stmt.execute(drone_uri, REGION_ID, battery, status, lat, lon, altitude, route_id);

    auto log_stmt = g_db->prepare(R"(
        INSERT INTO keepalive_log(drone_uri, battery, status, lat, lon, altitude, route_id)
        VALUES (?, ?, ?, ?, ?, ?, ?);
    )");

    log_stmt.execute(drone_uri, battery, status, lat, lon, altitude, route_id);

    std::cout << "[REGIONAL] Status drona: " << drone_uri
              << " | " << status
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

json send_to_central(json msg)
{
    std::lock_guard<std::mutex> lock(central_mutex);

    msg["REGION_ID"] = REGION_ID;

    std::string request = msg.dump() + "\n";

    boost::asio::write(*central_socket, boost::asio::buffer(request));

    boost::asio::streambuf buffer;
    boost::asio::read_until(*central_socket, buffer, "\n");

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
        std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");

        json status_msg;
        status_msg["TYPE"] = "DRONE_STATUS";
        status_msg["DRONE_URI"] = drone_uri;
        status_msg["BATTERY"] = msg.value("BATTERY", -1);
        status_msg["STATUS"] = "REGISTERED";
        status_msg["LAT"] = msg.value("LAT", 0.0);
        status_msg["LON"] = msg.value("LON", 0.0);
        status_msg["ALTITUDE"] = msg.value("ALTITUDE", 0);
        status_msg["ROUTE_ID"] = "";

        save_drone_status(status_msg);
        send_to_central(status_msg);

        response["TYPE"] = "REGISTER_ACK";
        response["DRONE_URI"] = drone_uri;
        response["MESSAGE"] = "Drone registered on regional server";
    }
    else if (type == "AUTH_REQ")
    {
        std::string token = msg.value("TOKEN", "");

        if (!token.empty())
        {
            response["TYPE"] = "AUTH_ACK";
            response["MESSAGE"] = "Authentication successful";
        }
        else
        {
            response["TYPE"] = "AUTH_ERROR";
            response["MESSAGE"] = "Invalid token";
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
    else if (type == "MISSION_REQUEST")
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
            response = send_to_central(msg);
        }
    }
    else if (type == "MISSION_FINISHED")
    {
        response = send_to_central(msg);
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

        save_drone_status(status_msg);
        json central_response = send_to_central(status_msg);

        response["TYPE"] = "ACK_RTB_SAVED";
        response["CENTRAL_RESPONSE"] = central_response;
    }
    else
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "Unknown message type on regional server";
    }

    return response;
}

void drone_session(tcp::socket socket)
{
    try
    {
        boost::asio::streambuf buffer;

        for (;;)
        {
            boost::system::error_code ec;
            boost::asio::read_until(socket, buffer, "\n", ec);

            if (ec)
            {
                std::cout << "[REGIONAL] Dron prekinuo konekciju.\n";
                break;
            }

            std::istream is(&buffer);
            std::string line;
            std::getline(is, line);

            if (line.empty())
                continue;

            std::cout << "[REGIONAL] Poruka od drona: " << line << std::endl;

            json msg = json::parse(line);
            json response = handle_drone_message(msg);

            std::string out = response.dump() + "\n";
            boost::asio::write(socket, boost::asio::buffer(out));
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
}

void start_drone_server(unsigned short port)
{
    boost::asio::io_context io_context;
    tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));

    std::cout << "[REGIONAL] Regionalni server sluša dronove na portu "
              << port << std::endl;

    for (;;)
    {
        tcp::socket socket(io_context);
        acceptor.accept(socket);

        std::cout << "[REGIONAL] Dron povezan na regionalni server.\n";

        std::thread(drone_session, std::move(socket)).detach();
    }
}

void connect_to_central(const std::string& host, const std::string& port)
{
    tcp::resolver resolver(central_io);
    auto endpoints = resolver.resolve(host, port);

    central_socket.reset(new tcp::socket(central_io));
    boost::asio::connect(*central_socket, endpoints);

    std::cout << "[REGIONAL] Regionalni server povezan na centralni server.\n";

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
    if (argc != 8)
    {
        std::cerr << "Usage: ./regional_server <region_id> <central_host> <central_port> "
                  << "<drone_listen_port> <base_lat> <base_lon> <zones_config>\n\n";

        std::cerr << "Primjer:\n";
        std::cerr << "./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 "
                  << "43.8563 18.4131 "
                  << "SKENDERIJA:43.8563:18.4131:1000:4,BASCARSIJA:43.8590:18.4310:800:4\n";
        return 1;
    }

    REGION_ID = argv[1];
    std::string central_host = argv[2];
    std::string central_port = argv[3];
    unsigned short drone_port = static_cast<unsigned short>(std::atoi(argv[4]));
    BASE_LAT = std::stod(argv[5]);
    BASE_LON = std::stod(argv[6]);
    ZONES = parse_zones_config(argv[7]);

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

        connect_to_central(central_host, central_port);

        start_drone_server(drone_port);
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
