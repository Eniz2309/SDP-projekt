// mission_client.cpp
// Jednostavan testni/operator CLI za slanje zadataka regionalnom serveru.
// v10: FORMATION <zone> <route> <altitude> [formation_size] [spacing_m]
// Nije dron i ne ucestvuje u telemetriji; koristi se za demonstraciju centralne dodjele zadataka.

#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <cstdlib>
#include "json/json.h"

using boost::asio::ip::tcp;
using json = nlohmann::json;

json transact(const std::string& host, const std::string& port, const json& request)
{
    boost::asio::io_context io;
    tcp::resolver resolver(io);
    tcp::socket socket(io);
    boost::asio::connect(socket, resolver.resolve(host, port));

    std::string out = request.dump() + "\n";
    boost::asio::write(socket, boost::asio::buffer(out));

    boost::asio::streambuf buffer;
    boost::asio::read_until(socket, buffer, "\n");
    std::istream is(&buffer);
    std::string line;
    std::getline(is, line);
    return json::parse(line);
}

int main(int argc, char* argv[])
{
    if (argc >= 5 && std::string(argv[3]) == "STOP")
    {
        json req;
        req["TYPE"] = "STOP_MISSION_REQUEST";
        req["MISSION_ID"] = argv[4];
        json res = transact(argv[1], argv[2], req);
        std::cout << res.dump() << std::endl;
        return 0;
    }

    if (argc < 6 || argc > 11)
    {
        std::cerr << "SUBMIT:\n"
                  << "./mission_client <regional_host> <tcp_port> <mission_id> <mission_type> <zone> "
                  << "[route_id] [base_altitude] [arg8] [arg9]\n"
                  << "  DELIVERY:  arg8=delivery_lat arg9=delivery_lon\n"
                  << "  FORMATION: arg8=formation_size arg9=spacing_m\n\n"
                  << "STOP:\n"
                  << "./mission_client <regional_host> <tcp_port> STOP <mission_id>\n\n"
                  << "Primjeri:\n"
                  << "./mission_client 127.0.0.1 8000 M001 MONITORING SKENDERIJA SKENDERIJA_K2 120\n"
                  << "./mission_client 127.0.0.1 8000 M002 INSPECTION SKENDERIJA SKENDERIJA_K2 120\n"
                  << "./mission_client 127.0.0.1 8000 M003 DELIVERY SKENDERIJA AUTO 120 43.8580 18.4160\n"
                  << "./mission_client 127.0.0.1 8000 M_FORM FORMATION SKENDERIJA SKENDERIJA_K2 120 3 10\n"
                  << "./mission_client 127.0.0.1 8000 STOP M001\n";
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

    try
    {
        json res = transact(argv[1], argv[2], req);
        std::cout << res.dump() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "mission_client error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
