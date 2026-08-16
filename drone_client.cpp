// drone_client.cpp
// v9_scheduler: dron vise ne bira vlastitu misiju.
// Nakon registracije/autentifikacije prijavljuje DRONE_READY/AVAILABLE i ceka START_MISSION od servera.
// TCP: kontrolne poruke; UDP: TELEMETRY i KEEPALIVE.
// Podrzani scenariji: TEST_FLIGHT, MONITORING, DELIVERY, INSPECTION, RTB i STOP_MISSION.

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
using boost::asio::ip::udp;
using json = nlohmann::json;

struct GeoPoint
{
    double lat;
    double lon;
};

struct InspectionPoint
{
    std::string point_id;
    GeoPoint position;
};

class DroneClient
{
public:
    DroneClient(boost::asio::io_context& io,
                const std::string& host,
                const std::string& tcp_port,
                const std::string& udp_port,
                const std::string& drone_uri,
                const std::string& token,
                int base_altitude)
        : io_(io),
          socket_(io),
          resolver_(io),
          udp_socket_(io, udp::v4()),
          udp_resolver_(io),
          keepalive_timer_(io),
          telemetry_timer_(io),
          battery_timer_(io),
          host_(host),
          port_(tcp_port),
          udp_port_(udp_port),
          drone_uri_(drone_uri),
          token_(token),
          zone_(""),
          mission_type_(""),
          route_id_("AUTO"),
          active_route_id_(""),
          mission_id_(""),
          delivery_lat_(0.0),
          delivery_lon_(0.0),
          battery_(100),
          lat_(43.8563),
          lon_(18.4131),
          altitude_(base_altitude),
          speed_(10),
          direction_("NORTH"),
          status_("INIT"),
          current_waypoint_(0),
          current_inspection_point_(0),
          delivery_mode_(false),
          reached_exit_point_(false),
          package_delivered_(false),
          mission_finished_sent_(false),
          low_battery_alarm_sent_(false)
    {
    }

    void start()
    {
        // UDP endpoint se razrjesava jednom. UDP ne uspostavlja konekciju/handshake.
        auto udp_endpoints = udp_resolver_.resolve(udp::v4(), host_, udp_port_);
        udp_endpoint_ = *udp_endpoints.begin();

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
        msg["ALTITUDE"] = altitude_;

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
        msg["ALTITUDE"] = altitude_;
        msg["ROUTE_ID"] = active_route_id_;

        send_udp_json(msg);

        std::cout << "[DRONE][UDP] KEEPALIVE sent. Battery="
                  << battery_ << "%\n";
    }

    void start_telemetry()
    {
        telemetry_timer_.expires_after(std::chrono::seconds(3));

        telemetry_timer_.async_wait(
            [this](boost::system::error_code ec)
            {
                if (!ec)
                {
                    update_simulated_values();
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

        send_udp_json(msg);

        std::cout << "[DRONE][UDP] TELEMETRY | "
                  << status_
                  << " | lat=" << lat_
                  << " lon=" << lon_
                  << " alt=" << altitude_
                  << " route=" << active_route_id_
                  << " battery=" << battery_ << "%"
                  << std::endl;
    }

    void start_battery_timer()
    {
        battery_timer_.expires_after(std::chrono::seconds(120));

        battery_timer_.async_wait(
            [this](boost::system::error_code ec)
            {
                if (!ec)
                {
                    if (status_ != "AVAILABLE" && status_ != "IDLE" &&
                        status_ != "REGISTERED" && battery_ > 0)
                    {
                        battery_ -= 1;

                        std::cout << "[DRONE] Battery decreased to "
                                  << battery_ << "%\n";

                        if (battery_ <= 20 &&
                            status_ != "RETURN_TO_BASE" &&
                            !low_battery_alarm_sent_)
                        {
                            send_low_battery_alarm();
                            low_battery_alarm_sent_ = true;
                        }
                    }

                    start_battery_timer();
                }
            });
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

    void send_drone_ready()
    {
        json msg;
        msg["TYPE"] = "DRONE_READY";
        msg["DRONE_URI"] = drone_uri_;
        msg["BATTERY"] = battery_;
        msg["STATUS"] = "AVAILABLE";
        msg["LAT"] = lat_;
        msg["LON"] = lon_;
        msg["ALTITUDE"] = altitude_;
        msg["ROUTE_ID"] = "";
        send_json(msg);

        std::cout << "[DRONE] Ready for assignment. STATUS=AVAILABLE" << std::endl;
    }

    void finish_mission()
    {
        if (mission_finished_sent_)
            return;

        mission_finished_sent_ = true;
        status_ = "AVAILABLE";
        active_route_id_.clear();

        json msg;
        msg["TYPE"] = "MISSION_FINISHED";
        msg["MISSION_ID"] = mission_id_;
        msg["DRONE_URI"] = drone_uri_;

        send_json(msg);

        std::cout << "[DRONE] Mission finished: "
                  << mission_id_ << std::endl;
    }

    void send_inspection_report(const InspectionPoint& point)
    {
        json msg;
        msg["TYPE"] = "INSPECTION_REPORT";
        msg["MISSION_ID"] = mission_id_;
        msg["DRONE_URI"] = drone_uri_;
        msg["POINT_ID"] = point.point_id;
        msg["LAT"] = point.position.lat;
        msg["LON"] = point.position.lon;
        msg["RESULT"] = "OK";

        // INSPECTION_REPORT je kontrolna/izvjestajna poruka i ide pouzdano preko TCP-a.
        send_json(msg);

        std::cout << "[DRONE] Inspection " << point.point_id
                  << " completed. RESULT=OK" << std::endl;
    }

    GeoPoint offset_from_center(double center_lat, double center_lon,
                                double north_m, double east_m)
    {
        const double PI = 3.14159265358979323846;

        double meters_per_deg_lat = 111320.0;
        double meters_per_deg_lon = 111320.0 * std::cos(center_lat * PI / 180.0);

        GeoPoint p;
        p.lat = center_lat + north_m / meters_per_deg_lat;
        p.lon = center_lon + east_m / meters_per_deg_lon;

        return p;
    }

    std::vector<GeoPoint> generate_square_contour(double center_lat,
                                                  double center_lon,
                                                  double half_size_m)
    {
        std::vector<GeoPoint> points;

        // Kontura je kvadrat/pravougaonik oko centra.
        // Tačke idu redom: gore-lijevo, gore-desno, dolje-desno, dolje-lijevo, nazad gore-lijevo.
        points.push_back(offset_from_center(center_lat, center_lon,  half_size_m, -half_size_m));
        points.push_back(offset_from_center(center_lat, center_lon,  half_size_m,  half_size_m));
        points.push_back(offset_from_center(center_lat, center_lon, -half_size_m,  half_size_m));
        points.push_back(offset_from_center(center_lat, center_lon, -half_size_m, -half_size_m));
        points.push_back(offset_from_center(center_lat, center_lon,  half_size_m, -half_size_m));

        return points;
    }

    double distance_to_point(const GeoPoint& p)
    {
        double dlat = lat_ - p.lat;
        double dlon = lon_ - p.lon;
        return std::sqrt(dlat * dlat + dlon * dlon);
    }

    void move_towards(const GeoPoint& target, double step)
    {
        double dlat = target.lat - lat_;
        double dlon = target.lon - lon_;

        double dist = std::sqrt(dlat * dlat + dlon * dlon);

        if (dist <= step || dist == 0.0)
        {
            lat_ = target.lat;
            lon_ = target.lon;
            return;
        }

        lat_ += step * dlat / dist;
        lon_ += step * dlon / dist;
    }

    void update_simulated_values()
    {
        const double MOVE_STEP = 0.00008;

        if (status_ == "ON_MISSION" && delivery_mode_)
        {
            // 1) Dron ide do izlazne tačke na konturi.
            if (!reached_exit_point_)
            {
                move_towards(exit_point_, MOVE_STEP);

                if (distance_to_point(exit_point_) < MOVE_STEP)
                {
                    reached_exit_point_ = true;
                    status_ = "DELIVERY_APPROACH";

                    std::cout << "[DRONE] Reached nearest point on contour. "
                              << "Leaving contour toward delivery point.\n";
                }

                return;
            }
        }

        if (status_ == "DELIVERY_APPROACH")
        {
            // 2) Dron ide od konture do dostavne tačke.
            move_towards(delivery_point_, MOVE_STEP);

            if (distance_to_point(delivery_point_) < MOVE_STEP)
            {
                status_ = "DELIVERING";
                std::cout << "[DRONE] Reached delivery point.\n";
            }

            return;
        }

        if (status_ == "DELIVERING")
        {
            // 3) Simulacija predaje paketa.
            package_delivered_ = true;

            std::cout << "[DRONE] Package delivered.\n";

            finish_mission();
            return;
        }

        if (status_ == "ON_MISSION" && mission_type_ == "INSPECTION" && !inspection_points_.empty())
        {
            const InspectionPoint& inspection = inspection_points_[current_inspection_point_];

            move_towards(inspection.position, MOVE_STEP);

            if (distance_to_point(inspection.position) < MOVE_STEP)
            {
                // Fiksiraj koordinatu na inspection tacku radi jasnog izvjestaja.
                lat_ = inspection.position.lat;
                lon_ = inspection.position.lon;

                send_inspection_report(inspection);
                current_inspection_point_++;

                if (current_inspection_point_ >= inspection_points_.size())
                {
                    std::cout << "[DRONE] All inspection points completed." << std::endl;
                    finish_mission();
                }
                else
                {
                    std::cout << "[DRONE] Flying to inspection point "
                              << inspection_points_[current_inspection_point_].point_id
                              << "..." << std::endl;
                }
            }

            return;
        }

        if (status_ == "ON_MISSION" && !route_points_.empty())
        {
            // Obicno kretanje po kockastoj konturi.
            GeoPoint target = route_points_[current_waypoint_];

            move_towards(target, MOVE_STEP);

            if (distance_to_point(target) < MOVE_STEP)
            {
                current_waypoint_++;

                if (current_waypoint_ >= route_points_.size())
                {
                    current_waypoint_ = 0;

                    if (mission_type_ == "TEST_FLIGHT")
                    {
                        finish_mission();
                    }
                }
            }
        }
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
                status_ = "AVAILABLE";

                std::cout << "[DRONE] Authenticated. Starting timers and waiting for assignment.\n";

                start_keepalive();
                start_telemetry();
                start_battery_timer();

                send_drone_ready();
            }
            else if (type == "ACK_DRONE_READY")
            {
                status_ = "AVAILABLE";
                std::cout << "[DRONE] Central confirms AVAILABLE state.\n";
            }
            else if (type == "START_MISSION" || type == "MISSION_APPROVED")
            {
                mission_id_ = msg.value("MISSION_ID", mission_id_);
                mission_type_ = msg.value("MISSION_TYPE", mission_type_);
                active_route_id_ = msg.value("ROUTE_ID", route_id_);
                altitude_ = msg.value("ALTITUDE", altitude_);

                double center_lat = msg.value("CENTER_LAT", lat_);
                double center_lon = msg.value("CENTER_LON", lon_);
                int half_size_m = msg.value("HALF_SIZE_M", 250);

                route_points_ = generate_square_contour(center_lat, center_lon, half_size_m);
                current_waypoint_ = 0;
                current_inspection_point_ = 0;
                inspection_points_.clear();
                mission_finished_sent_ = false;

                delivery_mode_ = false;
                reached_exit_point_ = false;
                package_delivered_ = false;

                if (mission_type_ == "DELIVERY")
                {
                    delivery_mode_ = true;

                    exit_point_.lat = msg.value("EXIT_LAT", lat_);
                    exit_point_.lon = msg.value("EXIT_LON", lon_);

                    delivery_point_.lat = msg.value("DELIVERY_LAT", lat_);
                    delivery_point_.lon = msg.value("DELIVERY_LON", lon_);
                }
                else if (mission_type_ == "INSPECTION")
                {
                    if (msg["INSPECTION_POINTS"].is_array())
                    {
                        for (const auto& item : msg["INSPECTION_POINTS"])
                        {
                            InspectionPoint point;
                            point.point_id = item.value("POINT_ID", "UNKNOWN_POINT");
                            point.position.lat = item.value("LAT", center_lat);
                            point.position.lon = item.value("LON", center_lon);
                            inspection_points_.push_back(point);
                        }
                    }

                    // Sigurnosni fallback: ako server iz nekog razloga nije poslao listu,
                    // koristi prva cetiri ugla vec generisane konture.
                    if (inspection_points_.empty() && route_points_.size() >= 4)
                    {
                        for (std::size_t i = 0; i < 4; ++i)
                        {
                            InspectionPoint point;
                            point.point_id = "I" + std::to_string(i + 1);
                            point.position = route_points_[i];
                            inspection_points_.push_back(point);
                        }
                    }
                }

                status_ = "ON_MISSION";

                std::cout << "[DRONE] Mission assigned by central server. "
                          << "type=" << mission_type_
                          << " route=" << active_route_id_
                          << " priority=" << msg.value("MISSION_PRIORITY", 0)
                          << " half_size=" << half_size_m << "m"
                          << " altitude=" << altitude_
                          << " slot=" << msg.value("ALTITUDE_SLOT", -1)
                          << std::endl;

                if (mission_type_ == "DELIVERY")
                {
                    std::cout << "[DRONE] Delivery path: contour -> exit point ("
                              << exit_point_.lat << "," << exit_point_.lon
                              << ") -> delivery point ("
                              << delivery_point_.lat << "," << delivery_point_.lon
                              << ")\n";
                }
                else if (mission_type_ == "INSPECTION")
                {
                    std::cout << "[DRONE] INSPECTION mission started. Points="
                              << inspection_points_.size() << std::endl;

                    for (const auto& point : inspection_points_)
                    {
                        std::cout << "  " << point.point_id << " -> "
                                  << point.position.lat << "," << point.position.lon
                                  << std::endl;
                    }

                    if (!inspection_points_.empty())
                    {
                        std::cout << "[DRONE] Flying to inspection point "
                                  << inspection_points_[0].point_id << "..." << std::endl;
                    }
                }
            }
            else if (type == "MISSION_REJECTED")
            {
                status_ = "AVAILABLE";
                std::cout << "[DRONE] Mission rejected: "
                          << msg.value("REASON", "UNKNOWN") << std::endl;
            }
            else if (type == "ACK_INSPECTION_REPORT")
            {
                std::cout << "[DRONE] Central acknowledged inspection point "
                          << msg.value("POINT_ID", "UNKNOWN_POINT") << ".\n";
            }
            else if (type == "ACK_MISSION_FINISHED")
            {
                std::cout << "[DRONE] Central acknowledged mission finish.\n";
            }
            else if (type == "MISSION_FINISH_REJECTED")
            {
                std::cerr << "[DRONE] Mission finish rejected: "
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
                active_route_id_ = "";
                route_points_.clear();
                inspection_points_.clear();
                current_inspection_point_ = 0;

                // Demo verzija: dron se odmah vrati na bazne koordinate.
                // Kasnije se može napraviti postepeni povratak pomoću move_towards().
                lat_ = base_lat;
                lon_ = base_lon;

                std::cout << "[DRONE] Returning to base because of "
                          << msg.value("REASON", "UNKNOWN_REASON")
                          << ": " << base_lat << ", " << base_lon << std::endl;

                json ack;
                ack["TYPE"] = "ACK_RTB";
                ack["DRONE_URI"] = drone_uri_;
                ack["BASE_LAT"] = base_lat;
                ack["BASE_LON"] = base_lon;
                ack["BATTERY"] = battery_;
                ack["ALTITUDE"] = altitude_;
                send_json(ack);
            }
            else if (type == "STOP_MISSION")
            {
                std::string stopped_mission_id =
                    msg.value("MISSION_ID", mission_id_);
                std::string reason =
                    msg.value("REASON", "UNKNOWN_REASON");

                status_ = "AVAILABLE";
                active_route_id_.clear();
                route_points_.clear();
                current_waypoint_ = 0;

                inspection_points_.clear();
                current_inspection_point_ = 0;

                delivery_mode_ = false;
                reached_exit_point_ = false;
                package_delivered_ = false;

                mission_finished_sent_ = true;

                std::cout << "[DRONE] STOP_MISSION primljen. mission="
                          << stopped_mission_id
                          << " reason=" << reason << std::endl;

                json ack;
                ack["TYPE"] = "ACK_STOP";
                ack["DRONE_URI"] = drone_uri_;
                ack["MISSION_ID"] = stopped_mission_id;
                ack["REASON"] = reason;
                ack["STATUS"] = "AVAILABLE";
                ack["BATTERY"] = battery_;
                ack["LAT"] = lat_;
                ack["LON"] = lon_;
                ack["ALTITUDE"] = altitude_;
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

    // UDP: jedan JSON objekat = jedan datagram. Nema \n delimiter-a niti ACK-a.
    void send_udp_json(const json& msg)
    {
        std::string data = msg.dump();

        boost::system::error_code ec;
        udp_socket_.send_to(boost::asio::buffer(data), udp_endpoint_, 0, ec);

        if (ec)
        {
            std::cerr << "[DRONE] UDP send error: "
                      << ec.message() << std::endl;
        }
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

    udp::socket udp_socket_;
    udp::resolver udp_resolver_;
    udp::endpoint udp_endpoint_;

    boost::asio::steady_timer keepalive_timer_;
    boost::asio::steady_timer telemetry_timer_;
    boost::asio::steady_timer battery_timer_;

    std::deque<std::string> write_queue_;

    std::string host_;
    std::string port_;       // TCP port regionalnog servera
    std::string udp_port_;   // UDP port za TELEMETRY/KEEPALIVE
    std::string drone_uri_;
    std::string token_;

    std::string zone_;
    std::string mission_type_;
    std::string route_id_;
    std::string active_route_id_;
    std::string mission_id_;

    double delivery_lat_;
    double delivery_lon_;

    int battery_;
    double lat_;
    double lon_;
    int altitude_;
    int speed_;
    std::string direction_;
    std::string status_;

    std::vector<GeoPoint> route_points_;
    std::size_t current_waypoint_;

    std::vector<InspectionPoint> inspection_points_;
    std::size_t current_inspection_point_;

    bool delivery_mode_;
    GeoPoint exit_point_;
    GeoPoint delivery_point_;
    bool reached_exit_point_;
    bool package_delivered_;
    bool mission_finished_sent_;
    bool low_battery_alarm_sent_;
};

int main(int argc, char* argv[])
{
    if (argc < 6 || argc > 7)
    {
        std::cerr << "Usage: ./drone_client <regional_host> <tcp_port> <udp_port> "
                  << "<drone_uri> <token> [base_altitude]\n\n";
        std::cerr << "Primjer:\n";
        std::cerr << "./drone_client 127.0.0.1 8000 8001 DRON_001 abc123 120\n";
        return 1;
    }

    int base_altitude = (argc >= 7) ? std::atoi(argv[6]) : 120;

    boost::asio::io_context io;
    DroneClient drone(io, argv[1], argv[2], argv[3], argv[4], argv[5], base_altitude);
    drone.start();
    io.run();
    return 0;
}
