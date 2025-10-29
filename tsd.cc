/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ctime>
#include <unordered_map>
#include <algorithm>
#include <thread>   // Added for heartbeat thread support

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include<glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity); 

#include "sns.grpc.pb.h"
#include "coordinator.grpc.pb.h"   // Added for coordinator communication
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
using csce438::Message;
using csce438::ListReply;
using csce438::Request;
using csce438::Reply;
using csce438::SNSService;
using csce438::CoordService;       // Added
using csce438::ServerInfo;         // Added
using csce438::Confirmation;       // Added


struct Client {
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::vector<Client*> client_followers;
  std::vector<Client*> client_following;
  ServerReaderWriter<Message, Message>* stream = 0;
  bool operator==(const Client& c1) const{
    return (username == c1.username);
  }
};

//Vector that stores every client that has been created
std::vector<Client*> client_db;

// New: Heartbeat thread function
void SendHeartbeat(std::string coord_ip, std::string coord_port,
                   int cluster_id, int server_id, std::string server_port) {
  // Create a gRPC channel to the Coordinator
  auto channel = grpc::CreateChannel(coord_ip + ":" + coord_port,
                                     grpc::InsecureChannelCredentials());
  std::unique_ptr<CoordService::Stub> coord_stub = CoordService::NewStub(channel);

  // Prepare server info for registration
  csce438::ServerInfo info;
  info.set_serverid(cluster_id);          // Cluster ID as serverID
  info.set_hostname("127.0.0.1");
  info.set_port(server_port);
  info.set_type("SERVER");

  while (true) {
    grpc::ClientContext ctx;
    csce438::Confirmation conf;
    Status s = coord_stub->Heartbeat(&ctx, info, &conf);
    if (s.ok() && conf.status()) {
      log(INFO, "💓 Heartbeat sent to Coordinator (" + coord_ip + ":" + coord_port + ")");
    } else {
      log(ERROR, "❌ Heartbeat failed: " + s.error_message());
    }
    std::this_thread::sleep_for(std::chrono::seconds(5)); // send every 5s
  }
}


class SNSServiceImpl final : public SNSService::Service {
  
  Status List(ServerContext* context, const Request* request, ListReply* list_reply) override {
    // Get the username from the request
    std::string user = request->username();

    // Add all registered users to the all_users list in the reply
    for(auto c: client_db){
      list_reply->add_all_users(c->username);
    }

    // Find the current user and add their followers to the followers list
    for(auto c: client_db){
      if(c->username == user){
        for(auto f: c->client_followers){
          list_reply->add_followers(f->username);
        }
        break;
      }
    }
    return Status::OK;
  }

  Status Follow(ServerContext* context, const Request* request, Reply* reply) override {
    // Get the username of the requester
    std::string user = request->username();

    // Check if an argument (user to follow) is provided
    if (request->arguments_size() == 0) { reply->set_msg("INVALID"); return Status::OK; }
    std::string user_to_follow = request->arguments(0);

    // Prevent self-following
    if (user == user_to_follow) { reply->set_msg("INVALID_USERNAME"); return Status::OK; }

    // Find the client objects for both users
    Client* user_client = nullptr;
    Client* follow_client = nullptr;
    for (auto c : client_db) {
      if (c->username == user) user_client = c;
      if (c->username == user_to_follow) follow_client = c;
      if (user_client && follow_client) break;
    }

    // Check if both users exist
    if (!user_client || !follow_client) { reply->set_msg("User does not exist"); return Status::OK; }

    // Check if already following
    for (auto c : user_client->client_following) {
      if (c->username == user_to_follow) { reply->set_msg("Already following user"); return Status::OK; }
    }

    // Establish the follow relationship
    user_client->client_following.push_back(follow_client);
    follow_client->client_followers.push_back(user_client);

    // Record the follow time for timeline filtering
    {
      std::ofstream ofs(user + std::string("_follow_time.txt"), std::ios::app);
      ofs << user_to_follow << "|" << static_cast<long long>(time(nullptr)) << "\n";
    }

    reply->set_msg("OK");
    return Status::OK;
  }

  Status UnFollow(ServerContext* context, const Request* request, Reply* reply) override {
    // Get the username of the requester
    std::string user = request->username();

    // Check if an argument (user to unfollow) is provided
    if (request->arguments_size() == 0) { reply->set_msg("INVALID"); return Status::OK; }
    std::string user_to_unfollow = request->arguments(0);

    // Prevent self-unfollowing
    if (user == user_to_unfollow) { reply->set_msg("INVALID_USERNAME"); return Status::OK; }

    // Find the client objects for both users
    Client* user_client = nullptr;
    Client* unfollow_client = nullptr;
    for (auto c : client_db) {
      if (c->username == user) user_client = c;
      if (c->username == user_to_unfollow) unfollow_client = c;
      if (user_client && unfollow_client) break;
    }

    // Check if both users exist
    if (!user_client || !unfollow_client) { reply->set_msg("User does not exist"); return Status::OK; }

    // Remove from following list
    bool found = false;
    for (auto it = user_client->client_following.begin(); it != user_client->client_following.end(); ++it) {
      if ((*it)->username == user_to_unfollow) {
        user_client->client_following.erase(it);
        found = true;
        break;
      }
    }
    if (!found) { reply->set_msg("Not following user"); return Status::OK; }

    // Remove from followers list of the unfollowed user
    for (auto it = unfollow_client->client_followers.begin(); it != unfollow_client->client_followers.end(); ++it) {
      if ((*it)->username == user) { unfollow_client->client_followers.erase(it); break; }
    }

    reply->set_msg("OK");
    return Status::OK;
  }

  Status Login(ServerContext* context, const Request* request, Reply* reply) override {
    // Get the username from the request
    std::string user = request->username();

    // Check if user already exists
    for (auto c : client_db) {
      if (c->username == user) {
        // If already connected, reject login
        if (c->connected) { reply->set_msg("User already logged in"); return Status::OK; }
        // Reconnect existing user
        c->connected = true;
        reply->set_msg("Login successful");
        return Status::OK;
      }
    }

    // Create new user if not found
    Client* new_client = new Client();
    new_client->username = user;
    new_client->connected = true;
    client_db.push_back(new_client);
    reply->set_msg("New user created and logged in");
    return Status::OK;
  }

  Status Timeline(ServerContext* context, ServerReaderWriter<Message, Message>* stream) override {
    // Original MP1 timeline implementation — unchanged
    std::string username;
    const auto& md = context->client_metadata();
    auto it = md.find("username");
    if (it != md.end()) {
      username.assign(it->second.data(), it->second.length());
    } else {
      return Status(grpc::StatusCode::UNAUTHENTICATED, "No username in metadata");
    }

    Client* user_client = nullptr;
    for (auto c : client_db) if (c->username == username) { user_client = c; break; }
    if (!user_client) return Status(grpc::StatusCode::NOT_FOUND, "User not found");

    user_client->stream = stream;

    Message incoming;
    while (stream->Read(&incoming)) {
      const std::string self_file = user_client->username + ".timeline";
      std::ofstream fout(self_file, std::ios::app);
      if (fout) {
        char buf[32];
        time_t sec = static_cast<time_t>(incoming.timestamp().seconds());
        std::tm* tm_ptr = localtime(&sec);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_ptr);
        fout << "T " << buf << "\n"
            << "U " << incoming.username() << "\n"
            << "W " << incoming.msg() << "\n\n";
      }

      for (auto f : user_client->client_followers) {
        if (f->stream) {
          f->stream->Write(incoming);
        }
      }
    }

    user_client->stream = nullptr;
    return Status::OK;
  }
};

void RunServer(std::string port_no, std::string coord_ip, std::string coord_port,
               int cluster_id, int server_id) {   // Added new args
  std::string server_address = "127.0.0.1:"+port_no;
  SNSServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  log(INFO, "Server listening on "+server_address);

  // Start heartbeat thread after server starts
  std::thread hb(SendHeartbeat, coord_ip, coord_port, cluster_id, server_id, port_no);
  hb.detach();

  server->Wait();
}

int main(int argc, char** argv) {

  std::string port = "3010";
  std::string coord_ip = "localhost";   // ✅ new
  std::string coord_port = "9090";      // ✅ new
  int cluster_id = 1;                   // ✅ new
  int server_id = 1;                    // ✅ new
  
  int opt = 0;
  while ((opt = getopt(argc, argv, "c:s:h:k:p:")) != -1){   // ✅ expanded args
    switch(opt) {
      case 'c': cluster_id = atoi(optarg); break;
      case 's': server_id = atoi(optarg); break;
      case 'h': coord_ip = optarg; break;
      case 'k': coord_port = optarg; break;
      case 'p': port = optarg; break;
      default:
	  std::cerr << "Invalid Command Line Argument\n";
    }
  }
  
  std::string log_file_name = std::string("server-") + port;
  google::InitGoogleLogging(log_file_name.c_str());
  log(INFO, "Logging Initialized. Server starting...");

  RunServer(port, coord_ip, coord_port, cluster_id, server_id);  // ✅ updated

  return 0;
}
