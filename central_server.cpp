// central_server.cpp
// Verzija usklađena sa LV9-10: Boost.Asio + JSON + SQLITE3 prema LV9-10

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <cstdlib>
#include <vector>

#include <sqlite3.h>
#include "json/json.h"
#include "sqlite3_wrapper.h"

using boost::asio::ip::tcp;
using json = nlohmann::json;
namespace sqlite = sqlite3_wrapper;

std::unique_ptr<sqlite::db> g_db;
std::mutex db_mutex;

void exec_sql(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    g_db->execute(sql);
}


// Kreiranje baze ukoliko već ne postoji
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
        CREATE TABLE IF NOT EXISTS regional_zones (
            region_id TEXT,
            zone_id TEXT,
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
            last_seen DATETIME DEFAULT CURRENT_TIMESTAMP
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
        CREATE TABLE IF NOT EXISTS missions (
            mission_id TEXT PRIMARY KEY,
            drone_uri TEXT,
            region_id TEXT,
            mission_type TEXT,
            zone TEXT,
            altitude INTEGER,
            time_slot TEXT,
            status TEXT
        );
    )");
}


void handle_region_register(const json& msg)
{
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    double base_lat = msg.value("BASE_LAT", 0.0);
    double base_lon = msg.value("BASE_LON", 0.0);

    std::vector<std::string> zones =
        msg.value("ZONES", std::vector<std::string>{});

    auto region_stmt = g_db->prepare(
        "INSERT OR REPLACE INTO regional_servers(region_id, base_lat, base_lon, last_seen) "
        "VALUES (?, ?, ?, CURRENT_TIMESTAMP);"
    );

    region_stmt.execute(region_id, base_lat, base_lon);

    auto delete_stmt = g_db->prepare(
        "DELETE FROM regional_zones WHERE region_id = ?;"
    );

    delete_stmt.execute(region_id);

    auto zone_stmt = g_db->prepare(
        "INSERT OR REPLACE INTO regional_zones(region_id, zone_id) "
        "VALUES (?, ?);"
    );

    for (const auto& zone : zones)
    {
        zone_stmt.execute(region_id, zone);
    }

    std::cout << "[CENTRAL] Registrovan regionalni server: "
              << region_id
              << " | baza: "
              << base_lat << ", " << base_lon
              << " | broj zona: "
              << zones.size()
              << std::endl;
}

/*
void handle_region_register(const json& msg)
{
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");

    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(R"(
        INSERT OR REPLACE INTO regional_servers(region_id, last_seen)
        VALUES (?, CURRENT_TIMESTAMP);
    )");

    stmt.execute(region_id);

    std::cout << "[CENTRAL] Registrovan regionalni server: "
              << region_id << std::endl;
}
*/
void handle_drone_status(const json& msg)
{
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    int battery = msg.value("BATTERY", -1);
    std::string status = msg.value("STATUS", "UNKNOWN");
    double lat = msg.value("LAT", 0.0);
    double lon = msg.value("LON", 0.0);

    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(R"(
        INSERT OR REPLACE INTO drones
        (drone_uri, region_id, battery, status, lat, lon, last_seen)
        VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
    )");

    stmt.execute(drone_uri, region_id, battery, status, lat, lon);

    std::cout << "[CENTRAL] Ažuriran status drona: "
              << drone_uri << std::endl;
}

void handle_alarm(const json& msg)
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

    std::cout << "[CENTRAL] Alarm za dron " << drone_uri
              << ": " << alarm_type << std::endl;
}

bool zone_exists(const std::string& region_id, const std::string& zone)
{
    auto stmt = g_db->prepare(
        "SELECT COUNT(*) "
        "FROM regional_zones "
        "WHERE region_id = ? AND zone_id = ?;"
    );

    stmt.execute(region_id, zone);

    int count = 0;
    if (stmt.fetch(count))
    {
        return count > 0;
    }

    return false;
}

bool route_conflict_exists(const json& msg)
{
    std::string region_id = msg.value("REGION_ID", "");
    std::string zone = msg.value("ZONE", "");
    int altitude = msg.value("ALTITUDE", 0);
    std::string time_slot = msg.value("TIME_SLOT", "");

    std::lock_guard<std::mutex> lock(db_mutex);

    auto stmt = g_db->prepare(R"(
        SELECT COUNT(*)
        FROM missions
        WHERE region_id = ?
          AND zone = ?
          AND altitude = ?
          AND time_slot = ?
          AND status = 'ACTIVE';
    )");

    stmt.execute(region_id, zone, altitude, time_slot);

    int count = 0;
    if (stmt.fetch(count))
    {
        return count > 0;
    }

    return false;
}

json handle_mission_request(const json& msg)
{
    json response;

    std::string mission_id = msg.value("MISSION_ID", "UNKNOWN_MISSION");
    std::string drone_uri = msg.value("DRONE_URI", "UNKNOWN_DRONE");
    std::string region_id = msg.value("REGION_ID", "UNKNOWN_REGION");
    std::string mission_type = msg.value("MISSION_TYPE", "TEST_FLIGHT");
    std::string zone = msg.value("ZONE", "");
    int altitude = msg.value("ALTITUDE", 0);
    std::string time_slot = msg.value("TIME_SLOT", "");

    if (!zone_exists(region_id, zone))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "INVALID_ZONE";

        std::cout << "[CENTRAL] Misija odbijena jer zona nije registrovana: "
                  << region_id << " / " << zone << std::endl;

        return response;
    }

    if (route_conflict_exists(msg))
    {
        response["TYPE"] = "MISSION_REJECTED";
        response["MISSION_ID"] = mission_id;
        response["REASON"] = "ROUTE_CONFLICT";

        std::cout << "[CENTRAL] Misija odbijena zbog konflikta rute: "
                  << mission_id << std::endl;
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(db_mutex);

            auto stmt = g_db->prepare(R"(
                INSERT INTO missions
                (mission_id, drone_uri, region_id, mission_type, zone, altitude, time_slot, status)
                VALUES (?, ?, ?, ?, ?, ?, ?, 'ACTIVE');
            )");

            stmt.execute(mission_id, drone_uri, region_id, mission_type, zone, altitude, time_slot);
        }

        response["TYPE"] = "MISSION_APPROVED";
        response["MISSION_ID"] = mission_id;
        response["DRONE_URI"] = drone_uri;
        response["COMMAND"] = "START_MISSION";

        std::cout << "[CENTRAL] Misija odobrena: "
                  << mission_id << std::endl;
    }

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
                handle_alarm(msg);
                response["TYPE"] = "ACK";
                response["MESSAGE"] = "ALARM_SAVED";
            }
            else if (type == "MISSION_REQUEST")
            {
                response = handle_mission_request(msg);
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
