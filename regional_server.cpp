// regional_server.cpp
// Verzija usklađena sa LV9-10: Boost.Asio + JSON + SQLITE3 prema LV9-10

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sstream>
#include <vector>
#include <cstdlib>
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
double BASE_LAT = 0.0;
double BASE_LON = 0.0;
std::vector<ZoneConfig> ZONES;

std::unique_ptr<sqlite::db> g_db;
std::mutex db_mutex;

boost::asio::io_context central_io;
std::unique_ptr<tcp::socket> central_socket;
std::mutex central_mutex;

void exec_sql(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    g_db->execute(sql);
}

// Format: BASCARSIJA:43.8590:18.4310:800:4,SKENDERIJA:43.8563:18.4131:1000:4
std::vector<ZoneConfig> parse_zones(const std::string& text)
{
    std::vector<ZoneConfig> zones;
    std::stringstream all(text);
    std::string zone_text;

    while (std::getline(all, zone_text, ','))
    {
        std::stringstream ss(zone_text);
        std::vector<std::string> p;
        std::string item;

        while (std::getline(ss, item, ':')) p.push_back(item);

        if (p.size() < 4)
        {
            std::cerr << "[REGIONAL] Preskačem neispravnu zonu: " << zone_text << std::endl;
            continue;
        }

        ZoneConfig z;
        z.zone_id = p[0];
        z.center_lat = std::stod(p[1]);
        z.center_lon = std::stod(p[2]);
        z.radius_m = std::stoi(p[3]);
        z.contours = (p.size() >= 5) ? std::stoi(p[4]) : 4;

        zones.push_back(z);
    }

    return zones;
}

bool valid_zone(const std::string& zone)
{
    for (const auto& z : ZONES)
    {
        if (z.zone_id == zone) return true;
    }
    return false;
}

void init_database()
{
    exec_sql("CREATE TABLE IF NOT EXISTS drones (drone_uri TEXT PRIMARY KEY, region_id TEXT, battery INTEGER, status TEXT, lat REAL, lon REAL, last_seen DATETIME DEFAULT CURRENT_TIMESTAMP);");
    exec_sql("CREATE TABLE IF NOT EXISTS keepalive_log (id INTEGER PRIMARY KEY AUTOINCREMENT, drone_uri TEXT, battery INTEGER, status TEXT, received_at DATETIME DEFAULT CURRENT_TIMESTAMP);");
    exec_sql("CREATE TABLE IF NOT EXISTS zones (region_id TEXT, zone_id TEXT, center_lat REAL, center_lon REAL, radius_m INTEGER, contours INTEGER, PRIMARY KEY(region_id, zone_id));");
    exec_sql("CREATE TABLE IF NOT EXISTS alarms (id INTEGER PRIMARY KEY AUTOINCREMENT, drone_uri TEXT, alarm_type TEXT, message TEXT, created_at DATETIME DEFAULT CURRENT_TIMESTAMP);");
}

void save_zones()
{
    std::lock_guard<std::mutex> lock(db_mutex);

    auto del = g_db->prepare("DELETE FROM zones WHERE region_id = ?;");
    del.execute(REGION_ID);

    auto stmt = g_db->prepare("INSERT OR REPLACE INTO zones(region_id, zone_id, center_lat, center_lon, radius_m, contours) VALUES (?, ?, ?, ?, ?, ?);");

    for (const auto& z : ZONES)
    {
        stmt.execute(REGION_ID, z.zone_id, z.center_lat, z.center_lon, z.radius_m, z.contours);
    }
}

void print_zones()
{
    std::cout << "[REGIONAL] Zone za " << REGION_ID << ":\n";
    for (const auto& z : ZONES)
    {
        std::cout << "  " << z.zone_id << " center=" << z.center_lat << "," << z.center_lon
                  << " radius=" << z.radius_m << "m contours=" << z.contours << std::endl;
    }
}

void save_drone_status(const json& msg)
{
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    int battery = msg.value("BATTERY", -1);
    std::string status = msg.value("STATUS", "UNKNOWN");
    double lat = msg.value("LAT", 0.0);
    double lon = msg.value("LON", 0.0);

    std::lock_guard<std::mutex> lock(db_mutex);

    auto upsert_stmt = g_db->prepare("INSERT OR REPLACE INTO drones(drone_uri, region_id, battery, status, lat, lon, last_seen) VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);");
    upsert_stmt.execute(drone_uri, REGION_ID, battery, status, lat, lon);

    auto log_stmt = g_db->prepare("INSERT INTO keepalive_log(drone_uri, battery, status) VALUES (?, ?, ?);");
    log_stmt.execute(drone_uri, battery, status);
}

json send_to_central(json msg)
{
    std::lock_guard<std::mutex> lock(central_mutex);

    msg["REGION_ID"] = REGION_ID;

    std::string req = msg.dump() + "\n";
    boost::asio::write(*central_socket, boost::asio::buffer(req));

    boost::asio::streambuf buffer;
    boost::asio::read_until(*central_socket, buffer, "\n");

    std::istream is(&buffer);
    std::string line;
    std::getline(is, line);

    return json::parse(line);
}

void connect_to_central(const std::string& host, const std::string& port)
{
    tcp::resolver resolver(central_io);
    auto endpoints = resolver.resolve(host, port);

    central_socket.reset(new tcp::socket(central_io));
    boost::asio::connect(*central_socket, endpoints);

    json reg;
    reg["TYPE"] = "REGION_REGISTER";
    reg["REGION_ID"] = REGION_ID;
    reg["BASE_LAT"] = BASE_LAT;
    reg["BASE_LON"] = BASE_LON;
    reg["ZONES"] = json::array();

    for (const auto& z : ZONES)
    {
        json zj;
        zj["ZONE_ID"] = z.zone_id;
        zj["CENTER_LAT"] = z.center_lat;
        zj["CENTER_LON"] = z.center_lon;
        zj["RADIUS_M"] = z.radius_m;
        zj["CONTOURS"] = z.contours;
        reg["ZONES"].push_back(zj);
    }

    json response = send_to_central(reg);

    std::cout << "[REGIONAL] Registrovan na centralni: " << response.dump() << std::endl;
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
        response["TYPE"] = "ACK_STATUS";
        response["CENTRAL_RESPONSE"] = send_to_central(msg);
    }
    else if (type == "MISSION_REQUEST")
    {
        std::string zone = msg.value("ZONE", "");
        if (!valid_zone(zone))
        {
            response["TYPE"] = "MISSION_REJECTED";
            response["MISSION_ID"] = msg.value("MISSION_ID", "UNKNOWN_MISSION");
            response["REASON"] = "INVALID_ZONE_ON_REGIONAL";
        }
        else
        {
            std::string route_id = msg.value("ROUTE_ID", "AUTO");
            if (route_id.empty() || route_id == "AUTO")
            {
                msg["ROUTE_ID"] = zone + "_K1";
            }
            response = send_to_central(msg);
        }
    }
    else if (type == "MISSION_FINISHED")
    {
        response = send_to_central(msg);
    }
    else if (type == "ALARM")
    {
        response = send_to_central(msg);
    }
    else
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "UNKNOWN_MESSAGE_TYPE";
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
            if (ec) break;

            std::istream is(&buffer);
            std::string line;
            std::getline(is, line);
            if (line.empty()) continue;

            std::cout << "[REGIONAL] Od drona: " << line << std::endl;

            json response;
            try
            {
                json msg = json::parse(line);
                response = handle_drone_message(msg);
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
        std::cerr << "[REGIONAL] Drone session error: " << e.what() << std::endl;
    }
}

void start_drone_server(unsigned short port)
{
    boost::asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), port));

    std::cout << "[REGIONAL] Slušam dronove na portu " << port << std::endl;

    for (;;)
    {
        tcp::socket socket(io);
        acceptor.accept(socket);
        std::cout << "[REGIONAL] Dron povezan.\n";
        std::thread(drone_session, std::move(socket)).detach();
    }
}

int main(int argc, char* argv[])
{
    if (argc != 8)
    {
        std::cerr << "Usage: ./regional_server <region_id> <central_host> <central_port> <drone_port> <base_lat> <base_lon> <zones_config>\n";
        std::cerr << "Primjer:\n";
        std::cerr << "./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 43.8563 18.4131 BASCARSIJA:43.8590:18.4310:800:4,SKENDERIJA:43.8563:18.4131:1000:4\n";
        return 1;
    }

    REGION_ID = argv[1];
    std::string central_host = argv[2];
    std::string central_port = argv[3];
    unsigned short drone_port = static_cast<unsigned short>(std::atoi(argv[4]));
    BASE_LAT = std::stod(argv[5]);
    BASE_LON = std::stod(argv[6]);
    ZONES = parse_zones(argv[7]);

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
    catch (const std::exception& e)
    {
        std::cerr << "[REGIONAL] Greška: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

