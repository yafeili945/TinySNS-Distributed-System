#include <algorithm>
#include <cstdio>
#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>

// ✅ glog logging
#include <glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity);

#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

using google::protobuf::Timestamp;
using google::protobuf::Duration;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using csce438::CoordService;
using csce438::ServerInfo;
using csce438::Confirmation;
using csce438::ID;
using csce438::ServerList;
using csce438::SynchService;

struct zNode{
    int serverID;
    std::string hostname;
    std::string port;
    std::string type;
    std::time_t last_heartbeat;
    bool missed_heartbeat;
    bool isActive();

};

//potentially thread safe 
std::mutex v_mutex;
std::vector<zNode*> cluster1;
std::vector<zNode*> cluster2;
std::vector<zNode*> cluster3;

// creating a vector of vectors containing znodes
std::vector<std::vector<zNode*>> clusters = {cluster1, cluster2, cluster3};


//func declarations
int findServer(std::vector<zNode*> v, int id); 
std::time_t getTimeNow();
void checkHeartbeat();


bool zNode::isActive(){
    bool status = false;
    if(!missed_heartbeat){
        status = true;
    }else if(difftime(getTimeNow(),last_heartbeat) < 10){
        status = true;
    }
    return status;
}


class CoordServiceImpl final : public CoordService::Service {

    Status Heartbeat(ServerContext* context, const ServerInfo* serverinfo, Confirmation* confirmation) override {
        std::lock_guard<std::mutex> lock(v_mutex);

        int cluster_id = serverinfo->serverid();  // Server's cluster ID
        std::string host = serverinfo->hostname();
        std::string port = serverinfo->port();

        // Make sure cluster_id is valid
        if (cluster_id < 1 || cluster_id > 3) {
            std::cerr << "Invalid cluster ID: " << cluster_id << std::endl;
            log(ERROR, "Invalid cluster ID received: " + std::to_string(cluster_id));
            confirmation->set_status(false);
            return Status::CANCELLED;
        }

        // Search for existing server in the cluster
        int pos = findServer(clusters[cluster_id - 1], cluster_id);

        if (pos == -1) {
            // Register new server
            zNode* node = new zNode();
            node->serverID = cluster_id;
            node->hostname = host;
            node->port = port;
            node->type = "SERVER";
            node->last_heartbeat = getTimeNow();
            node->missed_heartbeat = false;

            clusters[cluster_id - 1].push_back(node);
            std::cout << "✅ Registered new server (Cluster " << cluster_id
                    << ") at " << host << ":" << port << std::endl;
            log(INFO, "Registered new server (Cluster " + std::to_string(cluster_id) + 
                      ") at " + host + ":" + port);
        } else {
            // Update existing server's heartbeat
            clusters[cluster_id - 1][pos]->last_heartbeat = getTimeNow();
            clusters[cluster_id - 1][pos]->missed_heartbeat = false;
            std::cout << "💓 Heartbeat updated from Server " << cluster_id
                    << " (" << host << ":" << port << ")" << std::endl;
            log(INFO, "Heartbeat updated from Server " + std::to_string(cluster_id) + 
                      " (" + host + ":" + port + ")");
        }

        confirmation->set_status(true);
        return Status::OK;
    }

    //function returns the server information for requested client id
    //this function assumes there are always 3 clusters and has math
    //hardcoded to represent this.
    Status GetServer(ServerContext* context, const ID* id, ServerInfo* serverinfo) override {
        std::lock_guard<std::mutex> lock(v_mutex);

        int client_id = id->id();
        int cluster_id = ((client_id - 1) % 3) + 1;
        std::cout << "Client " << client_id << " requesting connection → Cluster " << cluster_id << std::endl;
        log(INFO, "Client " + std::to_string(client_id) + " requesting connection → Cluster " + std::to_string(cluster_id));

        auto& cluster = clusters[cluster_id - 1];

        // Check if the cluster has any servers
        if (cluster.empty()) {
            std::cerr << "❌ No server found in cluster " << cluster_id << std::endl;
            log(ERROR, "No active server found in cluster " + std::to_string(cluster_id));
            return Status(grpc::StatusCode::UNAVAILABLE, "No server in this cluster");
        }

        // Find the first active server in the cluster
        for (auto& node : cluster) {
            if (node->isActive()) {
                serverinfo->set_serverid(node->serverID);
                serverinfo->set_hostname(node->hostname);
                serverinfo->set_port(node->port);
                serverinfo->set_type(node->type);
                std::cout << "✅ Assigned Client " << client_id
                        << " → Server " << node->hostname << ":" << node->port << std::endl;
                log(INFO, "Assigned Client " + std::to_string(client_id) + 
                          " to Server " + node->hostname + ":" + node->port);
                return Status::OK;
            }
        }

        std::cerr << "❌ All servers in cluster " << cluster_id << " are inactive" << std::endl;
        log(ERROR, "All servers in cluster " + std::to_string(cluster_id) + " are inactive");
        return Status(grpc::StatusCode::UNAVAILABLE, "All servers in cluster inactive");
    }
    
    int findServer(std::vector<zNode*> v, int id) {
        for (int i = 0; i < v.size(); i++) {
            if (v[i]->serverID == id) return i;
        }
        return -1;
    }

};

void RunServer(std::string port_no){
    //start thread to check heartbeats
    std::thread hb(checkHeartbeat);
    //localhost = 127.0.0.1
    std::string server_address("0.0.0.0:"+port_no);
    CoordServiceImpl service;
    //grpc::EnableDefaultHealthCheckService(true);
    //grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    log(INFO, "Coordinator listening on " + server_address);

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
}

int main(int argc, char** argv) {

    std::string port = "3010";
    int opt = 0;
    while ((opt = getopt(argc, argv, "p:")) != -1){
        switch(opt) {
            case 'p':
                port = optarg;
                break;
            default:
                std::cerr << "Invalid Command Line Argument\n";
        }
    }

    // ✅ Initialize glog
    std::string log_file_name = "coordinator-" + port;
    google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging initialized. Coordinator starting...");

    RunServer(port);

    log(INFO, "Coordinator shutting down...");
    google::ShutdownGoogleLogging(); // ✅ Close glog before exit
    return 0;
}



void checkHeartbeat(){
    while(true){
        //check servers for heartbeat > 10
        //if true turn missed heartbeat = true

        v_mutex.lock();

        // iterating through the clusters vector of vectors of znodes
        for (auto& c : clusters){
            for(auto& s : c){
                if(difftime(getTimeNow(),s->last_heartbeat)>10){
                    std::cout << "missed heartbeat from server " << s->serverID << std::endl;
                    log(WARNING, "Missed heartbeat from server " + std::to_string(s->serverID) +
                                 " (" + s->hostname + ":" + s->port + ")");
                    if(!s->missed_heartbeat){
                        s->missed_heartbeat = true;
                        s->last_heartbeat = getTimeNow();
                    }
                }
            }
        }

        v_mutex.unlock();

        sleep(3);
    }
}


std::time_t getTimeNow(){
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}