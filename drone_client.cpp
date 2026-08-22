// ============================================================
// AUTONOMNI DRONOVI - VERZIJA 17
// Fajl: drone_client.cpp
// Dodano: kompatibilnost sa formation failure tokom; STOP_MISSION cisti
//         formation stanje, a LOW_BATTERY clan koristi postojeci RTB/charging tok.
// ============================================================

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <string>
#include <deque>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <algorithm>

#include "json/json.h"
#include "pqc_tls_utils.h"
#include "udp_aead.h"

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;
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

int env_int(const char* name, int fallback, int min_value, int max_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value)
        return fallback;

    try
    {
        int parsed = std::stoi(value);
        return std::max(min_value, std::min(max_value, parsed));
    }
    catch (...)
    {
        return fallback;
    }
}

class DroneClient
{
public:
    DroneClient(boost::asio::io_context& io,
                const std::string& host,
                const std::string& tcp_port,
                const std::string& udp_port,
                const std::string& drone_uri,
                const std::string& token,
                int base_altitude,
                const std::string& regional_cert)
        : io_(io),
          tls_context_(ssl::context::tls_client),
          tls_stream_(io, tls_context_),
          resolver_(io),
          udp_socket_(io, udp::v4()),
          udp_resolver_(io),
          keepalive_timer_(io),
          telemetry_timer_(io),
          battery_timer_(io),
          charging_timer_(io),
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
          battery_(env_int("SDP_INITIAL_BATTERY", 100, 1, 100)),
          battery_tick_seconds_(env_int("SDP_BATTERY_TICK_SECONDS", 120, 1, 3600)),
          lat_(43.8563),
          lon_(18.4131),
          altitude_(base_altitude),
          speed_(10),
          direction_("NORTH"),
          status_("INIT"),
          sensor_status_("OK"),
          current_waypoint_(0),
          current_inspection_point_(0),
          delivery_mode_(false),
          reached_exit_point_(false),
          package_delivered_(false),
          mission_finished_sent_(false),
          low_battery_alarm_sent_(false),
          charging_active_(false),
          rtb_reason_(""),
          formation_mode_(false),
          formation_offset_north_m_(0.0),
          formation_offset_east_m_(0.0),
          udp_key_ready_(false)
    {
        sdpsec::configure_pqc_client(tls_context_, regional_cert);
    }

    void start()
    {
        auto udp_endpoints = udp_resolver_.resolve(udp::v4(), host_, udp_port_);
        udp_endpoint_ = *udp_endpoints.begin();

        auto endpoints = resolver_.resolve(host_, port_);

        boost::asio::async_connect(tls_stream_.next_layer(), endpoints,
            [this](boost::system::error_code ec, tcp::endpoint)
            {
                if (ec)
                {
                    std::cerr << "[DRONE][PQC] TCP connection error: "
                              << ec.message() << std::endl;
                    return;
                }

                tls_stream_.async_handshake(ssl::stream_base::client,
                    [this](boost::system::error_code hs_ec)
                    {
                        if (hs_ec)
                        {
                            std::cerr << "[DRONE][PQC] TLS handshake error: "
                                      << hs_ec.message() << std::endl;
                            return;
                        }

                        sdpsec::print_tls_session(tls_stream_.native_handle(),
                                                  "[DRONE][PQC]");
                        udp_key_ =
                            sdpsec::export_udp_key(tls_stream_.native_handle());
                        udp_key_ready_ = true;

                        std::cout << "[DRONE] Secure connection to regional server established.\n";

                        start_read();
                        send_register();
                    });
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
        add_telemetry_context(msg);

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

    std::string current_flight_mode() const
    {
        if (status_ == "RETURN_TO_BASE")
            return "RTB";

        if (status_ == "AT_BASE" || status_ == "AT_BASE_LOW_BATTERY" ||
            status_ == "CHARGING")
            return "GROUND";

        if (status_ == "FORMATION" || formation_mode_)
            return "FORMATION";

        if (status_ == "ON_MISSION" || status_ == "DELIVERY_APPROACH" ||
            status_ == "DELIVERING")
            return "AUTONOMOUS";

        if (status_ == "AVAILABLE")
            return "STANDBY";

        if (status_ == "INIT" || status_ == "REGISTERED" || status_ == "AUTH_FAILED")
            return "GROUND";

        return "UNKNOWN";
    }

    void add_telemetry_context(json& msg) const
    {
        msg["MISSION_ID"] = mission_id_;
        msg["MISSION_TYPE"] = mission_type_;
        msg["FLIGHT_MODE"] = current_flight_mode();
        msg["SENSOR_STATUS"] = sensor_status_;
        msg["SPEED"] = speed_;
        msg["DIRECTION"] = direction_;
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
        add_telemetry_context(msg);
        if (formation_mode_)
        {
            msg["FORMATION_ID"] = formation_id_;
            msg["FORMATION_CONTROLLER"] = "REGIONAL_SERVER";
            msg["OFFSET_NORTH_M"] = formation_offset_north_m_;
            msg["OFFSET_EAST_M"] = formation_offset_east_m_;
        }

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
        msg["ROUTE_ID"] = active_route_id_;
        add_telemetry_context(msg);
        if (formation_mode_)
        {
            msg["FORMATION_ID"] = formation_id_;
            msg["FORMATION_CONTROLLER"] = "REGIONAL_SERVER";
            msg["OFFSET_NORTH_M"] = formation_offset_north_m_;
            msg["OFFSET_EAST_M"] = formation_offset_east_m_;
        }

        send_udp_json(msg);

        std::cout << "[DRONE][UDP] TELEMETRY | "
                  << status_
                  << " | lat=" << lat_
                  << " lon=" << lon_
                  << " alt=" << altitude_
                  << " route=" << active_route_id_
                  << " mission=" << (mission_id_.empty() ? "NONE" : mission_id_)
                  << " type=" << (mission_type_.empty() ? "NONE" : mission_type_)
                  << " mode=" << current_flight_mode()
                  << " sensor=" << sensor_status_
                  << " battery=" << battery_ << "%"
                  << std::endl;
    }

    void start_battery_timer()
    {
        battery_timer_.expires_after(std::chrono::seconds(battery_tick_seconds_));

        battery_timer_.async_wait(
            [this](boost::system::error_code ec)
            {
                if (!ec)
                {
                    const bool flying =
                        (status_ == "ON_MISSION" || status_ == "DELIVERY_APPROACH" ||
                         status_ == "DELIVERING" || status_ == "FORMATION" ||
                         status_ == "RETURN_TO_BASE");

                    if (flying && battery_ > 0)
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
        msg["BATTERY"] = battery_;
        msg["STATUS"] = status_;
        msg["LAT"] = lat_;
        msg["LON"] = lon_;
        msg["ALTITUDE"] = altitude_;
        msg["ROUTE_ID"] = active_route_id_;
        add_telemetry_context(msg);

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
        add_telemetry_context(msg);
        msg["MISSION_ID"] = "";
        msg["MISSION_TYPE"] = "";
        msg["FLIGHT_MODE"] = "STANDBY";
        send_json(msg);

        std::cout << "[DRONE] Ready for assignment. STATUS=AVAILABLE" << std::endl;
    }

    void start_charging()
    {
        if (charging_active_)
            return;

        charging_active_ = true;
        status_ = "CHARGING";
        active_route_id_.clear();
        mission_id_.clear();
        mission_type_.clear();

        std::cout << "[DRONE][RTB] CHARGING started at " << battery_
                  << "%. Ready threshold=80%." << std::endl;
        schedule_charge_step();
    }

    void schedule_charge_step()
    {
        charging_timer_.expires_after(std::chrono::seconds(2));
        charging_timer_.async_wait(
            [this](boost::system::error_code ec)
            {
                if (ec || !charging_active_)
                    return;

                battery_ = std::min(100, battery_ + 10);
                std::cout << "[DRONE][RTB] CHARGING " << battery_ << "%" << std::endl;

                if (battery_ >= 80)
                {
                    charging_active_ = false;
                    low_battery_alarm_sent_ = false;
                    status_ = "AT_BASE";
                    rtb_reason_.clear();

                    std::cout << "[DRONE][RTB] Battery ready. Requesting AVAILABLE state."
                              << std::endl;
                    send_drone_ready();
                    return;
                }

                schedule_charge_step();
            });
    }

    void finish_mission()
    {
        if (mission_finished_sent_)
            return;

        mission_finished_sent_ = true;
        status_ = "AVAILABLE";
        active_route_id_.clear();

        const std::string finished_mission_id = mission_id_;

        json msg;
        msg["TYPE"] = "MISSION_FINISHED";
        msg["MISSION_ID"] = finished_mission_id;
        msg["MISSION_TYPE"] = mission_type_;
        msg["DRONE_URI"] = drone_uri_;
        msg["BATTERY"] = battery_;
        msg["STATUS"] = "AVAILABLE";
        msg["LAT"] = lat_;
        msg["LON"] = lon_;
        msg["ALTITUDE"] = altitude_;
        msg["SPEED"] = speed_;
        msg["DIRECTION"] = direction_;
        msg["ROUTE_ID"] = "";
        msg["FLIGHT_MODE"] = "STANDBY";
        msg["SENSOR_STATUS"] = sensor_status_;

        send_json(msg);

        mission_id_.clear();
        mission_type_.clear();

        std::cout << "[DRONE] Mission finished: "
                  << finished_mission_id << std::endl;
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

        if (status_ == "FORMATION" && formation_mode_)
        {
            // Poziciju ne racuna drugi dron. Server salje target formacijskog clana.
            // Malo veci korak omogucava da dron prati virtual leader koji se osvjezava svake 2 s.
            const double FORMATION_MOVE_STEP = 0.00025;
            move_towards(formation_target_, FORMATION_MOVE_STEP);
            return;
        }

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
        boost::asio::async_read_until(tls_stream_, read_buffer_, "\n",
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
            else if (type == "REGISTER_ERROR")
            {
                status_ = "AUTH_FAILED";
                std::cerr << "[DRONE][AUTH] Registration rejected: "
                          << msg.value("REASON", "UNKNOWN_REASON") << std::endl;
            }
            else if (type == "AUTH_ERROR")
            {
                status_ = "AUTH_FAILED";
                std::cerr << "[DRONE][AUTH] Authentication rejected: "
                          << msg.value("REASON", "UNKNOWN_REASON") << std::endl;
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
            else if (type == "DRONE_READY_REJECTED")
            {
                status_ = msg.value("STATUS", "CHARGING");
                std::cerr << "[DRONE][RTB] AVAILABLE rejected: "
                          << msg.value("REASON", "UNKNOWN_REASON")
                          << " | min_battery=" << msg.value("MIN_BATTERY", 80)
                          << "%" << std::endl;
                if (status_ == "CHARGING")
                    start_charging();
            }
            else if (type == "START_FORMATION")
            {
                mission_id_ = msg.value("MISSION_ID", mission_id_);
                mission_type_ = "FORMATION";
                formation_id_ = mission_id_;
                active_route_id_ = msg.value("ROUTE_ID", "");
                altitude_ = msg.value("ALTITUDE", altitude_);
                formation_offset_north_m_ = msg.value("OFFSET_NORTH_M", 0.0);
                formation_offset_east_m_ = msg.value("OFFSET_EAST_M", 0.0);
                formation_mode_ = true;
                mission_finished_sent_ = false;
                route_points_.clear();
                inspection_points_.clear();
                delivery_mode_ = false;
                status_ = "FORMATION";

                formation_target_.lat = lat_;
                formation_target_.lon = lon_;

                std::cout << "[DRONE] FORMATION started. mission=" << mission_id_
                          << " controller=" << msg.value("FORMATION_CONTROLLER", "REGIONAL_SERVER")
                          << " same_altitude=" << msg.value("SAME_ALTITUDE", true)
                          << " alt=" << altitude_
                          << "m offset=(N:" << formation_offset_north_m_
                          << "m,E:" << formation_offset_east_m_ << "m)" << std::endl;
            }
            else if (type == "FORMATION_UPDATE")
            {
                std::string incoming_id = msg.value("MISSION_ID", "");
                if (formation_mode_ && incoming_id == formation_id_)
                {
                    formation_target_.lat = msg.value("TARGET_LAT", lat_);
                    formation_target_.lon = msg.value("TARGET_LON", lon_);
                    altitude_ = msg.value("ALTITUDE", altitude_);
                    formation_offset_north_m_ = msg.value("OFFSET_NORTH_M", formation_offset_north_m_);
                    formation_offset_east_m_ = msg.value("OFFSET_EAST_M", formation_offset_east_m_);

                    std::cout << "[DRONE][FORMATION] target="
                              << formation_target_.lat << "," << formation_target_.lon
                              << " alt=" << altitude_
                              << " virtual_leader="
                              << msg.value("VIRTUAL_LEADER_LAT", 0.0) << ","
                              << msg.value("VIRTUAL_LEADER_LON", 0.0)
                              << std::endl;
                }
            }
            else if (type == "START_MISSION" || type == "MISSION_APPROVED")
            {
                mission_id_ = msg.value("MISSION_ID", mission_id_);
                mission_type_ = msg.value("MISSION_TYPE", mission_type_);
                formation_mode_ = false;
                formation_id_.clear();
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
            else if (type == "ACK_PARAMS_SAVED")
            {
                std::cout << "[DRONE] Regional/central confirmed new flight parameters.\n";
            }
            else if (type == "ACK_RTB_SAVED")
            {
                std::string saved_status = msg.value("STATUS", "AT_BASE");
                status_ = saved_status;

                std::cout << "[DRONE][RTB] Regional/central confirmed base arrival. "
                          << "STATUS=" << saved_status << std::endl;

                if (saved_status == "AT_BASE_LOW_BATTERY")
                {
                    start_charging();
                }
                else
                {
                    // Manualni RTB je zavrsen. Dron je na bazi i moze ponovo
                    // zatraziti ulazak u AVAILABLE pool.
                    rtb_reason_.clear();
                    send_drone_ready();
                }
            }
            else if (type == "CHANGE_PARAMS")
            {
                handle_change_params(msg);
            }
            else if (type == "RETURN_TO_BASE")
            {
                double base_lat = msg.value("BASE_LAT", lat_);
                double base_lon = msg.value("BASE_LON", lon_);
                rtb_reason_ = msg.value("REASON", "UNKNOWN_REASON");

                charging_active_ = false;
                status_ = "RETURN_TO_BASE";
                active_route_id_.clear();
                route_points_.clear();
                current_waypoint_ = 0;
                inspection_points_.clear();
                current_inspection_point_ = 0;
                delivery_mode_ = false;
                reached_exit_point_ = false;
                package_delivered_ = false;
                formation_mode_ = false;
                formation_id_.clear();
                formation_offset_north_m_ = 0.0;
                formation_offset_east_m_ = 0.0;
                mission_id_.clear();
                mission_type_.clear();

                // Demo: dron se odmah vrati na bazne koordinate.
                // Kasnije se može napraviti postepeni povratak pomoću move_towards().
                lat_ = base_lat;
                lon_ = base_lon;
                altitude_ = 0;

                std::cout << "[DRONE] Returning to base because of "
                          << msg.value("REASON", "UNKNOWN_REASON")
                          << ": " << base_lat << ", " << base_lon << std::endl;

                // Simulator trenutno modelira instantni dolazak u bazu.
                // Zato ACK_RTB oznacava zavrsen povratak, ne samo prijem komande.
                status_ = "AT_BASE";

                json ack;
                ack["TYPE"] = "ACK_RTB";
                ack["DRONE_URI"] = drone_uri_;
                ack["BASE_LAT"] = base_lat;
                ack["BASE_LON"] = base_lon;
                ack["BATTERY"] = battery_;
                ack["ALTITUDE"] = altitude_;
                ack["STATUS"] = status_;
                ack["ROUTE_ID"] = active_route_id_;
                add_telemetry_context(ack);
                ack["REASON"] = rtb_reason_;
                ack["ARRIVAL_CONFIRMED"] = true;
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

                formation_mode_ = false;
                formation_id_.clear();
                formation_offset_north_m_ = 0.0;
                formation_offset_east_m_ = 0.0;

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
                add_telemetry_context(ack);
                send_json(ack);

                mission_id_.clear();
                mission_type_.clear();
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
        ack["BATTERY"] = battery_;
        ack["STATUS"] = status_;
        ack["LAT"] = lat_;
        ack["LON"] = lon_;
        ack["ROUTE_ID"] = active_route_id_;
        add_telemetry_context(ack);

        send_json(ack);

        std::cout << "[DRONE] New parameters applied. altitude=" << altitude_
                  << " speed=" << speed_
                  << " direction=" << direction_ << std::endl;
    }

    // UDP payload se štiti AES-256-GCM ključem izvedenim iz PQC TLS sesije.
    void send_udp_json(const json& msg)
    {
        if (!udp_key_ready_)
        {
            std::cerr << "[DRONE][UDP][SECURITY] PQC TLS exporter key nije spreman.\n";
            return;
        }

        try
        {
            json envelope =
                sdpsec::encrypt_udp_envelope(drone_uri_, msg, udp_key_);
            std::string data = envelope.dump();

            boost::system::error_code ec;
            udp_socket_.send_to(boost::asio::buffer(data), udp_endpoint_, 0, ec);

            if (ec)
            {
                std::cerr << "[DRONE] UDP send error: "
                          << ec.message() << std::endl;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[DRONE][UDP][SECURITY] Encryption error: "
                      << e.what() << std::endl;
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
        boost::asio::async_write(tls_stream_,
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
    ssl::context tls_context_;
    ssl::stream<tcp::socket> tls_stream_;
    tcp::resolver resolver_;
    boost::asio::streambuf read_buffer_;

    udp::socket udp_socket_;
    udp::resolver udp_resolver_;
    udp::endpoint udp_endpoint_;
    std::array<unsigned char, 32> udp_key_;
    bool udp_key_ready_;

    boost::asio::steady_timer keepalive_timer_;
    boost::asio::steady_timer telemetry_timer_;
    boost::asio::steady_timer battery_timer_;
    boost::asio::steady_timer charging_timer_;

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
    int battery_tick_seconds_;
    double lat_;
    double lon_;
    int altitude_;
    int speed_;
    std::string direction_;
    std::string status_;
    std::string sensor_status_;

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
    bool charging_active_;
    std::string rtb_reason_;

    bool formation_mode_;
    std::string formation_id_;
    double formation_offset_north_m_;
    double formation_offset_east_m_;
    GeoPoint formation_target_;
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
    const std::string regional_cert =
        sdpsec::env_or("SDP_REGIONAL_CERT", "regional-cert.pem");

    boost::asio::io_context io;
    DroneClient drone(io, argv[1], argv[2], argv[3], argv[4], argv[5],
                      base_altitude, regional_cert);
    drone.start();
    io.run();
    return 0;
}
