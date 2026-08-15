// regional_server.cpp
// Regionalni server za autonomne dronove.
// Registruje zone na centralni server, prima TCP kontrolne poruke i UDP telemetriju, čuva lokalni status,
// validira zone i prosljeđuje zahtjeve centralnom serveru.
// Ako centralni vrati RETURN_TO_BASE, regionalni komandu prosljeđuje dronu.
// Ako centralni odobri misiju višeg prioriteta, regionalni ispiše koju je misiju preuzeo.
// Watchdog detektuje gubitak telemetrije/keepalive-a nakon 45 s i prijavljuje CONNECTION_LOST.

#include <boost/asio.hpp>
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

#include <sqlite3.h>
#include "json/json.h"
#include "sqlite3_wrapper.h"

using boost::asio::ip::tcp;
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
std::unique_ptr<tcp::socket> central_socket;
std::mutex central_mutex;

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

    mark_drone_seen(msg);

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

            if (!response.value("PREEMPTED_DRONE", "").empty())
            {
                std::cout << "[REGIONAL] Centralni server je zbog prioriteta prekinuo misiju drona "
                          << response.value("PREEMPTED_DRONE", "UNKNOWN_DRONE")
                          << " i dodijelio slot novoj misiji."
                          << std::endl;
            }
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
                json msg = json::parse(received);
                std::string original_type = msg.value("TYPE", "UNKNOWN");

                if (original_type != "KEEPALIVE" && original_type != "TELEMETRY")
                {
                    std::cerr << "[REGIONAL][UDP] Ignorisana poruka tipa "
                              << original_type << std::endl;
                    continue;
                }

                std::cout << "[REGIONAL][UDP] " << original_type
                          << " od " << msg.value("DRONE_URI", "UNKNOWN_DRONE")
                          << " | " << sender_endpoint.address().to_string()
                          << ":" << sender_endpoint.port() << std::endl;

                // Centralni vec ocekuje DRONE_STATUS format.
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
