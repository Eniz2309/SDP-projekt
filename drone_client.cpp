#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <deque>
#include <chrono>
#include "json/json.h"

using boost::asio::ip::tcp;
using json = nlohmann::json;

class DroneClient
{
public:
    DroneClient(boost::asio::io_context& io,
                const std::string& host,
                const std::string& port,
                const std::string& drone_uri,
                const std::string& token,
                const std::string& mission_zone,
                const std::string& mission_type)
        : io_(io),
          socket_(io),
          resolver_(io),
          keepalive_timer_(io),
          telemetry_timer_(io),
          host_(host),
          port_(port),
          drone_uri_(drone_uri),
          token_(token),
          mission_zone_(mission_zone),
          mission_type_(mission_type),
          battery_(90),
          lat_(43.8563),
          lon_(18.4131),
          altitude_(100),
          speed_(10),
          direction_("NORTH"),
          status_("INIT")
    {
    }

    void start()
    {
        auto endpoints = resolver_.resolve(host_, port_);

        boost::asio::async_connect(socket_, endpoints,
            [this](boost::system::error_code ec, tcp::endpoint)
            {
                if (!ec)
                {
                    std::cout << "[DRONE] Connected to regional server.\n";

                    start_read();
                    send_register();
                }
                else
                {
                    std::cerr << "[DRONE] Connection error: "
                              << ec.message() << std::endl;
                }
            });
    }

private:
    void send_register()
    {
        json msg;
        msg["TYPE"] = "REGISTER_REQ";
        msg["DRONE_URI"] = drone_uri_;
        msg["TOKEN"] = token_;
        msg["BATTERY"] = battery_;
        msg["LAT"] = lat_;
        msg["LON"] = lon_;

        send_json(msg);
    }

    void send_auth()
    {
        json msg;
        msg["TYPE"] = "AUTH_REQ";
        msg["DRONE_URI"] = drone_uri_;
        msg["TOKEN"] = token_;

        send_json(msg);
    }

    void start_keepalive()
    {
        keepalive_timer_.expires_after(std::chrono::seconds(15));

        keepalive_timer_.async_wait(
            [this](boost::system::error_code ec)
            {
                if (!ec)
                {
                    send_keepalive();
                    start_keepalive();
                }
            });
    }

    void send_keepalive()
    {
        json msg;
        msg["TYPE"] = "KEEPALIVE";
        msg["DRONE_URI"] = drone_uri_;
        msg["BATTERY"] = battery_;
        msg["STATUS"] = status_;
        msg["LAT"] = lat_;
        msg["LON"] = lon_;

        send_json(msg);

        std::cout << "[DRONE] KEEPALIVE sent.\n";
    }

    void start_telemetry()
    {
        telemetry_timer_.expires_after(std::chrono::seconds(15));

        telemetry_timer_.async_wait(
            [this](boost::system::error_code ec)
            {
                if (!ec)
                {
                    send_telemetry();
                    update_simulated_values();
                    start_telemetry();
                }
            });
    }

    void send_telemetry()
    {
        json msg;
        msg["TYPE"] = "TELEMETRY";
        msg["DRONE_URI"] = drone_uri_;
        msg["BATTERY"] = battery_;
        msg["STATUS"] = status_;
        msg["LAT"] = lat_;
        msg["LON"] = lon_;
        msg["ALTITUDE"] = altitude_;
        msg["SPEED"] = speed_;
        msg["DIRECTION"] = direction_;

        send_json(msg);

        std::cout << "[DRONE] TELEMETRY sent.\n";
    }

    void update_simulated_values()
    {
        if (status_ == "ON_MISSION" || status_ == "FORMATION_MODE")
        {
            lat_ += 0.0001;
            lon_ += 0.0001;

            if (battery_ > 0)
                battery_ -= 1;
        }

        if (battery_ <= 20 && status_ != "RETURN_TO_BASE")
        {
            send_low_battery_alarm();
        }
    }

    void send_low_battery_alarm()
    {
        json msg;
        msg["TYPE"] = "ALARM";
        msg["DRONE_URI"] = drone_uri_;
        msg["ALARM_TYPE"] = "LOW_BATTERY";
        msg["MESSAGE"] = "Battery below 20 percent";

        send_json(msg);

        std::cout << "[DRONE] LOW_BATTERY alarm sent.\n";
    }

    void send_mission_request()
    {
        json msg;
        msg["TYPE"] = "MISSION_REQUEST";
        msg["MISSION_ID"] = drone_uri_ + "_M001";
        msg["MISSION_TYPE"] = mission_type_;
        msg["DRONE_URI"] = drone_uri_;
        msg["ZONE"] = mission_zone_;
        msg["ALTITUDE"] = altitude_;
        msg["TIME_SLOT"] = "10:00-10:15";

        send_json(msg);

        std::cout << "[DRONE] Mission request sent.\n";
    }

    void start_read()
    {
        boost::asio::async_read_until(socket_, read_buffer_, "\n",
            [this](boost::system::error_code ec, std::size_t)
            {
                if (!ec)
                {
                    std::istream is(&read_buffer_);
                    std::string line;
                    std::getline(is, line);

                    if (!line.empty())
                        handle_server_message(line);

                    start_read();
                }
                else
                {
                    std::cerr << "[DRONE] Server disconnected: "
                              << ec.message() << std::endl;
                }
            });
    }

    void handle_server_message(const std::string& line)
    {
        try
        {
            json msg = json::parse(line);

            std::string type = msg.value("TYPE", "UNKNOWN");

            std::cout << "[DRONE] Received: " << msg.dump() << std::endl;

            if (type == "REGISTER_ACK")
            {
                status_ = "REGISTERED";
                send_auth();
            }
            else if (type == "AUTH_ACK")
            {
                status_ = "IDLE";

                std::cout << "[DRONE] Authenticated. Starting timers.\n";

                start_keepalive();
                start_telemetry();

                // Demo: nakon autentifikacije tražimo misiju.
                send_mission_request();
            }
            else if (type == "MISSION_APPROVED")
            {
                status_ = "ON_MISSION";
                std::cout << "[DRONE] Mission approved. Drone is ON_MISSION.\n";
            }
            else if (type == "MISSION_REJECTED")
            {
                status_ = "IDLE";
                std::cout << "[DRONE] Mission rejected: "
                          << msg.value("REASON", "UNKNOWN") << std::endl;
            }
            else if (type == "CHANGE_PARAMS")
            {
                handle_change_params(msg);
            }
            else if (type == "RETURN_TO_BASE")
            {
                double base_lat = msg.value("BASE_LAT", lat_);
                double base_lon = msg.value("BASE_LON", lon_);

                status_ = "RETURN_TO_BASE";
                lat_ = base_lat;
                lon_ = base_lon;

                std::cout << "[DRONE] Returning to base: "
                          << base_lat << ", " << base_lon << std::endl;

                json ack;
                ack["TYPE"] = "ACK_RTB";
                ack["DRONE_URI"] = drone_uri_;
                ack["BASE_LAT"] = base_lat;
                ack["BASE_LON"] = base_lon;
                send_json(ack);
            }
            else if (type == "FORMATION_START")
            {
                std::string formation_id = msg.value("FORMATION_ID", "UNKNOWN_FORMATION");
                std::string leader = msg.value("LEADER", "");
                int spacing = msg.value("SPACING", 0);

                altitude_ = msg.value("ALTITUDE", altitude_);
                speed_ = msg.value("SPEED", speed_);
                direction_ = msg.value("DIRECTION", direction_);
                status_ = "FORMATION_MODE";

                std::cout << "[DRONE] Entering formation "
                          << formation_id
                          << " | leader: " << leader
                          << " | altitude: " << altitude_
                          << " | speed: " << speed_
                          << " | direction: " << direction_
                          << " | spacing: " << spacing
                          << std::endl;

                json ack;
                ack["TYPE"] = "ACK_FORMATION_START";
                ack["DRONE_URI"] = drone_uri_;
                ack["FORMATION_ID"] = formation_id;
                send_json(ack);
            }
            else if (type == "FORMATION_STOP")
            {
                std::string formation_id = msg.value("FORMATION_ID", "UNKNOWN_FORMATION");

                status_ = "IDLE";

                std::cout << "[DRONE] Leaving formation "
                          << formation_id << std::endl;

                json ack;
                ack["TYPE"] = "ACK_FORMATION_STOP";
                ack["DRONE_URI"] = drone_uri_;
                ack["FORMATION_ID"] = formation_id;
                send_json(ack);
            }
            else if (type == "STOP_MISSION")
            {
                status_ = "IDLE";
                std::cout << "[DRONE] Mission stopped.\n";

                json ack;
                ack["TYPE"] = "ACK_STOP";
                ack["DRONE_URI"] = drone_uri_;
                send_json(ack);
            }
        }
        catch (std::exception& e)
        {
            std::cerr << "[DRONE] Invalid JSON from server: "
                      << e.what() << std::endl;
        }
    }

    void handle_change_params(const json& msg)
    {
        int new_altitude = msg.value("ALTITUDE", altitude_);
        int new_speed = msg.value("SPEED", speed_);
        std::string new_direction = msg.value("DIRECTION", direction_);

        if (new_altitude < 20 || new_altitude > 500 || new_speed < 0 || new_speed > 50)
        {
            json err;
            err["TYPE"] = "ERROR_PARAMS";
            err["DRONE_URI"] = drone_uri_;
            err["MESSAGE"] = "Invalid altitude or speed";

            send_json(err);

            std::cout << "[DRONE] Invalid flight parameters.\n";
            return;
        }

        altitude_ = new_altitude;
        speed_ = new_speed;
        direction_ = new_direction;

        json ack;
        ack["TYPE"] = "ACK_PARAMS";
        ack["DRONE_URI"] = drone_uri_;
        ack["ALTITUDE"] = altitude_;
        ack["SPEED"] = speed_;
        ack["DIRECTION"] = direction_;

        send_json(ack);

        std::cout << "[DRONE] New parameters applied.\n";
    }

    void send_json(const json& msg)
    {
        std::string data = msg.dump() + "\n";

        bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(data);

        if (!write_in_progress)
        {
            do_write();
        }
    }

    void do_write()
    {
        boost::asio::async_write(socket_,
            boost::asio::buffer(write_queue_.front()),
            [this](boost::system::error_code ec, std::size_t)
            {
                if (!ec)
                {
                    write_queue_.pop_front();

                    if (!write_queue_.empty())
                    {
                        do_write();
                    }
                }
                else
                {
                    std::cerr << "[DRONE] Write error: "
                              << ec.message() << std::endl;
                }
            });
    }

private:
    boost::asio::io_context& io_;
    tcp::socket socket_;
    tcp::resolver resolver_;
    boost::asio::streambuf read_buffer_;

    boost::asio::steady_timer keepalive_timer_;
    boost::asio::steady_timer telemetry_timer_;

    std::deque<std::string> write_queue_;

    std::string host_;
    std::string port_;
    std::string drone_uri_;
    std::string token_;
    std::string mission_zone_;
    std::string mission_type_;

    int battery_;
    double lat_;
    double lon_;
    int altitude_;
    int speed_;
    std::string direction_;
    std::string status_;
};

int main(int argc, char* argv[])
{
    if (argc < 5 || argc > 7)
    {
        std::cerr << "Usage: ./drone_client <regional_host> <regional_port> <drone_uri> <token> [mission_zone] [mission_type]\n";
        std::cerr << "Primjer: ./drone_client 127.0.0.1 8000 DRON_001 abc123 BASCARSIJA MONITORING\n";
        return 1;
    }

    std::string mission_zone = (argc >= 6) ? argv[5] : "BASCARSIJA";
    std::string mission_type = (argc >= 7) ? argv[6] : "MONITORING";

    boost::asio::io_context io;

    DroneClient drone(io, argv[1], argv[2], argv[3], argv[4], mission_zone, mission_type);

    drone.start();

    io.run();

    return 0;
}
