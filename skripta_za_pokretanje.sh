#!/bin/bash

set -e

git clone https://github.com/Eniz2309/SDP-projekt
cd SDP-projekt

g++ -std=c++11 -I . central_server.cpp -o central_server -lboost_system -lsqlite3 -pthread

g++ -std=c++11 -I . regional_server.cpp -o regional_server -lboost_system -lsqlite3 -pthread

g++ -std=c++11 -I . drone_client.cpp -o drone_client -lboost_system -pthread

g++ -std=c++11 -I . mission_client.cpp -o mission_client -lboost_system -pthread

echo "Build zavrsen."
