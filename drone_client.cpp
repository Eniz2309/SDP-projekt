#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <deque>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstdlib>
#include "json/json.h"

using boost::asio::ip::tcp;
using json = nlohmann::json;

struct GeoPoint
{
    double lat;
    double lon;
};

class DroneClient
{
public:
    DroneClient(boost::asio::io_context& io,
                const std::string& host,
                const std::string& port,
                const std::string& drone_uri,
                const std::string& token,
                const std::string& zone,
                const std::string& mission_type,
                const std::string& route_id,
                int base_altitude)
        : io_(io),
          socket_(io),
          resolver_(io),
          keepalive_timer_(io),
          telemetry_timer_(io),
          host_(host),
          port_(port),
          drone_uri_(drone_uri),
          token_(token),
          zone_(zone),
          mission_type_(mission_type),
          route_id_(route_id),
          mission_id_(drone_uri + "_M001"),
          battery_(90),
          lat_(43.8563),
          lon_(18.4131),
          altitude_(base_altitude),
          speed_(10),
          direction_("NORTH"),
          status_("INIT"),
          current_waypoint_(0)
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
                    std::cerr << "[DRONE] Connection error: " << ec.message() << std::endl;
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

    void send_mission_request()
    {
        json msg;
        msg["TYPE"] = "MISSION_REQUEST";
        msg["MISSION_ID"] = mission_id_;
        msg["MISSION_TYPE"] = mission_type_;
        msg["DRONE_URI"] = drone_uri_;
        msg["ZONE"] = zone_;
        msg["ROUTE_ID"] = route_id_;
        msg["ALTITUDE"] = altitude_;
        send_json(msg);

        std::cout << "[DRONE] Mission request sent: " << zone_ << " / " << route_id_ << std::endl;
    }

    void start_keepalive()
    {
        keepalive_timer_.expires_after(std::chrono::seconds(15));
        keepalive_timer_.async_wait([this](boost::system::error_code ec)
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
    }

    void start_telemetry()
    {
        telemetry_timer_.expires_after(std::chrono::seconds(3));
        telemetry_timer_.async_wait([this](boost::system::error_code ec)
        {
            if (!ec)
            {
                update_simulated_position();
                send_telemetry();
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
        msg["ROUTE_ID"] = active_route_id_;
        send_json(msg);

        std::cout << "[DRONE] TELEMETRY | " << status_
                  << " | lat=" << lat_
                  << " lon=" << lon_
                  << " alt=" << altitude_
                  << " route=" << active_route_id_
                  << std::endl;
    }

    void update_simulated_position()
    {
        if (status_ == "ON_MISSION" && !route_points_.empty())
        {
            GeoPoint p = route_points_[current_waypoint_];
            lat_ = p.lat;
            lon_ = p.lon;

            current_waypoint_++;

            if (current_waypoint_ >= route_points_.size())
            {
                current_waypoint_ = 0;

                if (mission_type_ == "TEST_FLIGHT")
                {
                    finish_mission();
                }
            }

            if (battery_ > 0) battery_ -= 1;
        }

        if (battery_ <= 20 && status_ != "RETURN_TO_BASE")
        {
            send_low_battery_alarm();
        }
    }

    void finish_mission()
    {
        if (status_ != "ON_MISSION") return;

        status_ = "IDLE";

        json msg;
        msg["TYPE"] = "MISSION_FINISHED";
        msg["MISSION_ID"] = mission_id_;
        msg["DRONE_URI"] = drone_uri_;
        send_json(msg);

        std::cout << "[DRONE] Mission finished: " << mission_id_ << std::endl;
    }

    void send_low_battery_alarm()
    {
        json msg;
        msg["TYPE"] = "ALARM";
        msg["DRONE_URI"] = drone_uri_;
        msg["ALARM_TYPE"] = "LOW_BATTERY";
        msg["MESSAGE"] = "Battery below 20 percent";
        send_json(msg);
    }

    GeoPoint offset_from_center(double center_lat, double center_lon, double north_m, double east_m)
    {
        const double PI = 3.14159265358979323846;
        double meters_per_deg_lat = 111320.0;
        double meters_per_deg_lon = 111320.0 * std::cos(center_lat * PI / 180.0);

        GeoPoint p;
        p.lat = center_lat + north_m / meters_per_deg_lat;
        p.lon = center_lon + east_m / meters_per_deg_lon;
        return p;
    }

    std::vector<GeoPoint> generate_contour(double center_lat, double center_lon, double radius_m)
    {
        const double PI = 3.14159265358979323846;
        std::vector<GeoPoint> points;

        for (int angle = 0; angle < 360; angle += 45)
        {
            double rad = angle * PI / 180.0;
            double north_m = radius_m * std::cos(rad);
            double east_m = radius_m * std::sin(rad);
            points.push_back(offset_from_center(center_lat, center_lon, north_m, east_m));
        }

        return points;
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

                    if (!line.empty()) handle_server_message(line);

                    start_read();
                }
                else
                {
                    std::cerr << "[DRONE] Server disconnected: " << ec.message() << std::endl;
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
                start_keepalive();
                start_telemetry();
                send_mission_request();
            }
            else if (type == "MISSION_APPROVED")
            {
                mission_id_ = msg.value("MISSION_ID", mission_id_);
                active_route_id_ = msg.value("ROUTE_ID", route_id_);
                altitude_ = msg.value("ALTITUDE", altitude_);

                double center_lat = msg.value("CENTER_LAT", lat_);
                double center_lon = msg.value("CENTER_LON", lon_);
                int radius_m = msg.value("RADIUS_M", 100);

                route_points_ = generate_contour(center_lat, center_lon, radius_m);
                current_waypoint_ = 0;
                status_ = "ON_MISSION";

                std::cout << "[DRONE] Mission approved | route=" << active_route_id_
                          << " radius=" << radius_m
                          << " alt=" << altitude_
                          << " slot=" << msg.value("ALTITUDE_SLOT", -1)
                          << std::endl;
            }
            else if (type == "MISSION_REJECTED")
            {
                status_ = "IDLE";
                std::cout << "[DRONE] Mission rejected: " << msg.value("REASON", "UNKNOWN") << std::endl;
            }
            else if (type == "ACK_MISSION_FINISHED")
            {
                std::cout << "[DRONE] Central acknowledged mission finish.\n";
            }
            else if (type == "RETURN_TO_BASE")
            {
                lat_ = msg.value("BASE_LAT", lat_);
                lon_ = msg.value("BASE_LON", lon_);
                status_ = "RETURN_TO_BASE";

                json ack;
                ack["TYPE"] = "ACK_RTB";
                ack["DRONE_URI"] = drone_uri_;
                send_json(ack);
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[DRONE] Invalid JSON: " << e.what() << std::endl;
        }
    }

    void send_json(const json& msg)
    {
        std::string data = msg.dump() + "\n";
        bool writing = !write_queue_.empty();
        write_queue_.push_back(data);
        if (!writing) do_write();
    }

    void do_write()
    {
        boost::asio::async_write(socket_, boost::asio::buffer(write_queue_.front()),
            [this](boost::system::error_code ec, std::size_t)
            {
                if (!ec)
                {
                    write_queue_.pop_front();
                    if (!write_queue_.empty()) do_write();
                }
                else
                {
                    std::cerr << "[DRONE] Write error: " << ec.message() << std::endl;
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
    std::string zone_;
    std::string mission_type_;
    std::string route_id_;
    std::string active_route_id_;
    std::string mission_id_;

    int battery_;
    double lat_;
    double lon_;
    int altitude_;
    int speed_;
    std::string direction_;
    std::string status_;

    std::vector<GeoPoint> route_points_;
    std::size_t current_waypoint_;
};

int main(int argc, char* argv[])
{
    if (argc < 7 || argc > 9)
    {
        std::cerr << "Usage: ./drone_client <regional_host> <regional_port> <drone_uri> <token> <zone> <mission_type> [route_id] [base_altitude]\n";
        std::cerr << "Primjer: ./drone_client 127.0.0.1 8000 DRON_001 abc123 SKENDERIJA MONITORING SKENDERIJA_K2 120\n";
        return 1;
    }

    std::string route_id = (argc >= 8) ? argv[7] : "AUTO";
    int base_altitude = (argc >= 9) ? std::atoi(argv[8]) : 120;

    boost::asio::io_context io;

    DroneClient drone(io, argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], route_id, base_altitude);
    drone.start();
    io.run();

    return 0;
}
