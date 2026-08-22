// ============================================================
// AUTONOMNI DRONOVI - VERZIJA 18
// Fajl: mission_client.cpp
// Dodano: bez promjene CLI sintakse; kontrola misija ostaje kompatibilna
//         sa SIGNAL_LOSS, live route nadzorom i ostalim serverskim zastitama.
// ============================================================

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <string>
#include <cstdlib>
#include "json/json.h"
#include "pqc_tls_utils.h"

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;
using json = nlohmann::json;

json transact(const std::string& host, const std::string& port, const json& request)
{
    boost::asio::io_context io;
    ssl::context ctx(ssl::context::tls_client);
    const std::string regional_cert =
        sdpsec::env_or("SDP_REGIONAL_CERT", "regional-cert.pem");
    sdpsec::configure_pqc_client(ctx, regional_cert);

    tcp::resolver resolver(io);
    ssl::stream<tcp::socket> stream(io, ctx);
    boost::asio::connect(stream.next_layer(), resolver.resolve(host, port));
    stream.handshake(ssl::stream_base::client);

    sdpsec::print_tls_session(stream.native_handle(), "[MISSION_CLIENT][PQC]");

    std::string out = request.dump() + "\n";
    boost::asio::write(stream, boost::asio::buffer(out));

    boost::asio::streambuf buffer;
    boost::asio::read_until(stream, buffer, "\n");
    std::istream is(&buffer);
    std::string line;
    std::getline(is, line);
    return json::parse(line);
}

void print_usage()
{
    std::cerr
        << "SUBMIT:\n"
        << "./mission_client <regional_host> <tcp_port> <mission_id> <mission_type> <zone> "
        << "[route_id] [base_altitude] [arg8] [arg9]\n"
        << "  DELIVERY:  arg8=delivery_lat arg9=delivery_lon\n"
        << "  FORMATION: arg8=formation_size arg9=spacing_m\n\n"
        << "STOP:\n"
        << "./mission_client <regional_host> <tcp_port> STOP <mission_id>\n\n"
        << "PARAMS:\n"
        << "./mission_client <regional_host> <tcp_port> PARAMS <drone_uri> <altitude> <speed> <direction>\n\n"
        << "RTB:\n"
        << "./mission_client <regional_host> <tcp_port> RTB <drone_uri>\n\n"
        << "Primjeri:\n"
        << "./mission_client 127.0.0.1 8000 M001 MONITORING SKENDERIJA SKENDERIJA_K2 120\n"
        << "./mission_client 127.0.0.1 8000 M003 DELIVERY SKENDERIJA AUTO 120 43.8580 18.4160\n"
        << "./mission_client 127.0.0.1 8000 M_FORM FORMATION SKENDERIJA SKENDERIJA_K2 120 3 10\n"
        << "./mission_client 127.0.0.1 8000 STOP M001\n"
        << "./mission_client 127.0.0.1 8000 PARAMS DRON_001 150 15 EAST\n"
        << "./mission_client 127.0.0.1 8000 RTB DRON_001\n";
}

int main(int argc, char* argv[])
{
    if (argc < 4)
    {
        print_usage();
        return 1;
    }

    try
    {
        std::string command = argv[3];

        if (command == "STOP")
        {
            if (argc != 5)
            {
                print_usage();
                return 1;
            }

            json req;
            req["TYPE"] = "STOP_MISSION_REQUEST";
            req["MISSION_ID"] = argv[4];
            std::cout << transact(argv[1], argv[2], req).dump() << std::endl;
            return 0;
        }

        if (command == "PARAMS")
        {
            if (argc != 8)
            {
                print_usage();
                return 1;
            }

            json req;
            req["TYPE"] = "CONTROL_PARAMS_REQUEST";
            req["DRONE_URI"] = argv[4];
            req["ALTITUDE"] = std::atoi(argv[5]);
            req["SPEED"] = std::atoi(argv[6]);
            req["DIRECTION"] = argv[7];
            std::cout << transact(argv[1], argv[2], req).dump() << std::endl;
            return 0;
        }

        if (command == "RTB")
        {
            if (argc != 5)
            {
                print_usage();
                return 1;
            }

            json req;
            req["TYPE"] = "MANUAL_RTB_REQUEST";
            req["DRONE_URI"] = argv[4];
            std::cout << transact(argv[1], argv[2], req).dump() << std::endl;
            return 0;
        }

        // U suprotnom se argv[3] tumaci kao mission_id.
        if (argc < 6 || argc > 11)
        {
            print_usage();
            return 1;
        }

        json req;
        req["TYPE"] = "MISSION_SUBMIT";
        req["MISSION_ID"] = argv[3];
        req["MISSION_TYPE"] = argv[4];
        req["ZONE"] = argv[5];
        req["ROUTE_ID"] = (argc >= 7) ? argv[6] : "AUTO";
        req["ALTITUDE"] = (argc >= 8) ? std::atoi(argv[7]) : 120;

        if (std::string(argv[4]) == "DELIVERY")
        {
            if (argc < 10)
            {
                std::cerr << "DELIVERY zahtijeva delivery_lat i delivery_lon.\n";
                return 1;
            }
            req["DELIVERY_LAT"] = std::stod(argv[8]);
            req["DELIVERY_LON"] = std::stod(argv[9]);
        }

        if (std::string(argv[4]) == "FORMATION")
        {
            int formation_size = (argc >= 9) ? std::atoi(argv[8]) : 3;
            double spacing_m = (argc >= 10) ? std::stod(argv[9]) : 10.0;
            req["FORMATION_SIZE"] = formation_size;
            req["FORMATION_SPACING_M"] = spacing_m;
        }

        std::cout << transact(argv[1], argv[2], req).dump() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "mission_client error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
