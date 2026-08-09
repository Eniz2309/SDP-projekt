// regional_server.cpp
// Verzija usklađena sa LV9-10: Boost.Asio + JSON + SQLITE3 prema LV9-10

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <sqlite3.h>
#include "json/json.h"
#include "sqlite3_wrapper.h"

using boost::asio::ip::tcp;
using json = nlohmann::json;
namespace sqlite = sqlite3_wrapper;

std::string REGION_ID;
double BASE_LAT;
double BASE_LON;
std::vector<std::string> ZONES; 
std::unique_ptr<sqlite::db> g_db;
std::mutex db_mutex;

boost::asio::io_context central_io;
std::unique_ptr<tcp::socket> central_socket;
std::mutex central_mutex;

struct DroneConnection
{
    std::shared_ptr<tcp::socket> socket;
    std::mutex write_mutex;
    std::string drone_uri;
};

std::map<std::string, std::shared_ptr<DroneConnection>> active_drones;
std::mutex drones_mutex;

std::map<std::string, std::set<std::string>> formation_groups;
std::mutex formation_mutex;


std::vector<std::string> parse_zones(const std::string& zones_text)
{
    std::vector<std::string> zones;
    std::stringstream ss(zones_text);
    std::string zone;

    while (std::getline(ss, zone, ','))
    {
        if (!zone.empty())
        {
            zones.push_back(zone);
        }
    }

    return zones;
}


bool is_valid_zone(const std::string& zone)
{
    return std::find(ZONES.begin(), ZONES.end(), zone) != ZONES.end();
}

void print_zones()
{
    std::cout << "[REGIONAL] Zone za " << REGION_ID << ": ";

    for (const auto& zone : ZONES)
    {
        std::cout << zone << " ";
    }

    std::cout << std::endl;
}


void exec_sql(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    g_db->execute(sql);
}

void init_database()
{
    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS drones (
            drone_uri TEXT PRIMARY KEY,
            region_id TEXT,
            battery INTEGER,
            status TEXT,
            lat REAL,
            lon REAL,
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )");

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS keepalive_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            drone_uri TEXT,
            battery INTEGER,
            status TEXT,
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

    exec_sql(R"(
        CREATE TABLE IF NOT EXISTS regional_zones (
            region_id TEXT,
            zone_id TEXT,
            PRIMARY KEY(region_id, zone_id)
        );
    )");
}

void save_zones()
{
    std::lock_guard<std::mutex> lock(db_mutex);

    auto delete_stmt = g_db->prepare(
        "DELETE FROM regional_zones WHERE region_id = ?;"
    );

    delete_stmt.execute(REGION_ID);

    auto insert_stmt = g_db->prepare(
        "INSERT OR REPLACE INTO regional_zones(region_id, zone_id) "
        "VALUES (?, ?);"
    );

    for (const auto& zone : ZONES)
    {
        insert_stmt.execute(REGION_ID, zone);
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

    auto upsert_stmt = g_db->prepare(R"(
        INSERT OR REPLACE INTO drones
        (drone_uri, region_id, battery, status, lat, lon, last_seen)
        VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
    )");

    upsert_stmt.execute(drone_uri, REGION_ID, battery, status, lat, lon);

    auto log_stmt = g_db->prepare(R"(
        INSERT INTO keepalive_log(drone_uri, battery, status)
        VALUES (?, ?, ?);
    )");

    log_stmt.execute(drone_uri, battery, status);

    std::cout << "[REGIONAL] Lokalno sačuvan status drona: "
              << drone_uri << std::endl;
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

    std::cout << "[REGIONAL] Lokalno sačuvan alarm za dron: "
              << drone_uri << std::endl;
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

bool send_to_drone(const std::string& drone_uri, const json& msg)
{
    std::shared_ptr<DroneConnection> conn;

    {
        std::lock_guard<std::mutex> lock(drones_mutex);

        auto it = active_drones.find(drone_uri);
        if (it == active_drones.end())
        {
            std::cout << "[REGIONAL] Dron nije aktivan: "
                      << drone_uri << std::endl;
            return false;
        }

        conn = it->second;
    }

    try
    {
        std::lock_guard<std::mutex> write_lock(conn->write_mutex);

        std::string out = msg.dump() + "\n";
        boost::asio::write(*(conn->socket), boost::asio::buffer(out));

        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[REGIONAL] Slanje dronu nije uspjelo: "
                  << drone_uri << " | " << e.what() << std::endl;
        return false;
    }
}

void multicast_to_formation(const std::string& formation_id, const json& msg)
{
    std::set<std::string> drones;

    {
        std::lock_guard<std::mutex> lock(formation_mutex);

        auto it = formation_groups.find(formation_id);
        if (it == formation_groups.end())
        {
            std::cout << "[REGIONAL] Formacija ne postoji: "
                      << formation_id << std::endl;
            return;
        }

        drones = it->second;
    }

    int sent_count = 0;

    for (const auto& drone_uri : drones)
    {
        bool ok = send_to_drone(drone_uri, msg);

        if (ok)
        {
            sent_count++;
            std::cout << "[REGIONAL] FORMATION multicast poslan dronu: "
                      << drone_uri << std::endl;
        }
    }

    std::cout << "[REGIONAL] Multicast formaciji " << formation_id
              << " zavrsen. Poslano dronova: " << sent_count
              << "/" << drones.size() << std::endl;
}

void remove_active_drone(std::shared_ptr<DroneConnection> conn)
{
    if (!conn || conn->drone_uri.empty())
        return;

    std::lock_guard<std::mutex> lock(drones_mutex);
    active_drones.erase(conn->drone_uri);

    std::cout << "[REGIONAL] Dron uklonjen iz aktivnih konekcija: "
              << conn->drone_uri << std::endl;
}

void operator_console()
{
    std::string line;

    std::cout << "[OPERATOR] Komande: list_drones | formation_create F1 DRON_001 DRON_002 | formation_start F1 DRON_001 120 15 EAST 30 | formation_stop F1\n";

    while (true)
    {
        std::cout << "[OPERATOR] ";
        std::getline(std::cin, line);

        if (line.empty())
            continue;

        std::stringstream ss(line);
        std::string command;
        ss >> command;

        if (command == "list_drones")
        {
            std::lock_guard<std::mutex> lock(drones_mutex);

            std::cout << "[REGIONAL] Aktivni dronovi: ";
            for (const auto& item : active_drones)
            {
                std::cout << item.first << " ";
            }
            std::cout << std::endl;
        }
        else if (command == "formation_create")
        {
            std::string formation_id;
            ss >> formation_id;

            if (formation_id.empty())
            {
                std::cout << "Format: formation_create FORMATION_1 DRON_001 DRON_002\n";
                continue;
            }

            std::set<std::string> drones;
            std::string drone_uri;

            while (ss >> drone_uri)
            {
                drones.insert(drone_uri);
            }

            if (drones.empty())
            {
                std::cout << "[REGIONAL] Moras navesti barem jednog drona.\n";
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(formation_mutex);
                formation_groups[formation_id] = drones;
            }

            std::cout << "[REGIONAL] Kreirana formacija "
                      << formation_id
                      << " sa "
                      << drones.size()
                      << " dronova.\n";
        }
        else if (command == "formation_start")
        {
            std::string formation_id;
            std::string leader;
            int altitude = 0;
            int speed = 0;
            std::string direction;
            int spacing = 0;

            ss >> formation_id >> leader >> altitude >> speed >> direction >> spacing;

            if (formation_id.empty() || leader.empty() || direction.empty())
            {
                std::cout << "Format: formation_start FORMATION_1 DRON_001 120 15 EAST 30\n";
                continue;
            }

            json msg;
            msg["TYPE"] = "FORMATION_START";
            msg["FORMATION_ID"] = formation_id;
            msg["LEADER"] = leader;
            msg["ALTITUDE"] = altitude;
            msg["SPEED"] = speed;
            msg["DIRECTION"] = direction;
            msg["SPACING"] = spacing;

            multicast_to_formation(formation_id, msg);

            std::cout << "[REGIONAL] Formacijski let pokrenut: "
                      << formation_id << std::endl;
        }
        else if (command == "formation_stop")
        {
            std::string formation_id;
            ss >> formation_id;

            if (formation_id.empty())
            {
                std::cout << "Format: formation_stop FORMATION_1\n";
                continue;
            }

            json msg;
            msg["TYPE"] = "FORMATION_STOP";
            msg["FORMATION_ID"] = formation_id;

            multicast_to_formation(formation_id, msg);

            std::cout << "[REGIONAL] Formacijski let zaustavljen: "
                      << formation_id << std::endl;
        }
        else
        {
            std::cout << "Nepoznata komanda. Dostupno:\n";
            std::cout << "  list_drones\n";
            std::cout << "  formation_create FORMATION_1 DRON_001 DRON_002\n";
            std::cout << "  formation_start FORMATION_1 DRON_001 120 15 EAST 30\n";
            std::cout << "  formation_stop FORMATION_1\n";
        }
    }
}

json handle_drone_message(json msg, std::shared_ptr<DroneConnection> conn)
{
    std::string type = msg.value("TYPE", "UNKNOWN");
    json response;

    if (type == "REGISTER_REQ")
    {
        std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");

        conn->drone_uri = drone_uri;

        {
            std::lock_guard<std::mutex> lock(drones_mutex);
            active_drones[drone_uri] = conn;
        }

        std::cout << "[REGIONAL] Dron dodat u aktivne konekcije: "
                  << drone_uri << std::endl;

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

        json central_response = send_to_central(msg);

        response["TYPE"] = "ACK_STATUS";
        response["CENTRAL_RESPONSE"] = central_response;
    }
    else if (type == "ALARM")
    {
        save_alarm(msg);

        json central_response = send_to_central(msg);

        response["TYPE"] = "ACK_ALARM";
        response["CENTRAL_RESPONSE"] = central_response;
    }
    else if (type == "MISSION_REQUEST")
    {
        std::string zone = msg.value("ZONE", "");

        if (!is_valid_zone(zone))
        {
            response["TYPE"] = "MISSION_REJECTED";
            response["MISSION_ID"] = msg.value("MISSION_ID", "UNKNOWN_MISSION");
            response["REASON"] = "INVALID_ZONE";

            std::cout << "[REGIONAL] Misija odbijena, zona nije u nadleznosti regiona: "
                      << zone << std::endl;
        }
        else
        {
            response = send_to_central(msg);
        }
    }
    else if (type == "ACK_FORMATION_START" || type == "ACK_FORMATION_STOP")
    {
        response["TYPE"] = "ACK";
        response["MESSAGE"] = "FORMATION_ACK_RECEIVED";

        std::cout << "[REGIONAL] ACK formacije od drona: "
                  << msg.value("DRONE_URI", "UNKNOWN_DRONE") << std::endl;
    }
    else
    {
        response["TYPE"] = "ERROR";
        response["MESSAGE"] = "Unknown message type";
    }

    return response;
}

void drone_session(tcp::socket socket)
{
    auto conn = std::make_shared<DroneConnection>();
    conn->socket = std::make_shared<tcp::socket>(std::move(socket));

    try
    {
        boost::asio::streambuf buffer;

        for (;;)
        {
            boost::system::error_code ec;
            boost::asio::read_until(*(conn->socket), buffer, "\n", ec);

            if (ec)
            {
                std::cout << "[REGIONAL] Dron prekinuo konekciju.\n";
                break;
            }

            std::istream is(&buffer);
            std::string line;
            std::getline(is, line);

            if (line.empty())
            {
                continue;
            }

            std::cout << "[REGIONAL] Poruka od drona: "
                      << line << std::endl;

            json msg = json::parse(line);
            json response = handle_drone_message(msg, conn);

            std::string out = response.dump() + "\n";

            {
                std::lock_guard<std::mutex> write_lock(conn->write_mutex);
                boost::asio::write(*(conn->socket), boost::asio::buffer(out));
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

    remove_active_drone(conn);
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
    register_msg["ZONES"] = ZONES;

    json response = send_to_central(register_msg);

    std::cout << "[REGIONAL] Odgovor centralnog servera: "
              << response.dump() << std::endl;
}

int main(int argc, char* argv[])
{
    if (argc != 8)
    {
        std::cerr << "Usage: ./regional_server <region_id> <central_host> <central_port> <drone_listen_port> <base_lat> <base_lon> <zones>\n";
        std::cerr << "Primjer: ./regional_server REGION_SARAJEVO 127.0.0.1 9000 8000 43.8563 18.4131 BASCARSIJA,SKENDERIJA,KOSEVSKO_BRDO,POFALICI,OTOKA\n";
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
        std::cerr << "[REGIONAL] Moras definisati barem jednu zonu.\n";
        return 1;
    }

    try
    {
        g_db.reset(new sqlite::db("regional_server.db"));
        init_database();
        save_zones();
        print_zones();

        connect_to_central(central_host, central_port);

        std::thread(operator_console).detach();

        start_drone_server(drone_port);
    }
    catch (const sqlite::exception& e)
    {
        std::cerr << "[REGIONAL] SQLite greška: "
                  << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[REGIONAL] Greška: "
                  << e.what() << std::endl;
        return 1;
    }

    return 0;
}
