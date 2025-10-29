#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>
#include <grpc++/grpc++.h>
#include <chrono>
#include "client.h"

#include "sns.grpc.pb.h"
#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

// ✅ glog
#include <glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity);

using grpc::ClientContext;
using grpc::ClientReaderWriter;
using grpc::Status;
using csce438::Message;
using csce438::ListReply;
using csce438::Request;
using csce438::Reply;
using csce438::SNSService;
using csce438::CoordService;
using csce438::ServerInfo;
using csce438::ID;

Message MakeMessage(const std::string& username, const std::string& msg) {
    Message m;
    m.set_username(username);
    m.set_msg(msg);
    auto* ts = new google::protobuf::Timestamp();
    ts->set_seconds(time(NULL));
    ts->set_nanos(0);
    m.set_allocated_timestamp(ts);
    return m;
}

class Client : public IClient {
public:
    Client(const std::string& h, const std::string& u, const std::string& p)
        : hostname(h), username(u), port(p) {}

protected:
    int connectTo() override;
    IReply processCommand(std::string& input) override;
    void processTimeline() override;

private:
    std::string hostname, username, port;
    std::string server_address_;
    std::unique_ptr<SNSService::Stub> stub_;

    IReply Login();
    IReply List();
    IReply Follow(const std::string& username);
    IReply UnFollow(const std::string& username);
    void Timeline(const std::string& username);

    bool canReachServer();
};

//////////////////////// connectTo ////////////////////////
int Client::connectTo() {
    std::string coord_addr = hostname + ":" + port;
    auto coord_channel = grpc::CreateChannel(coord_addr, grpc::InsecureChannelCredentials());
    auto coord_stub = CoordService::NewStub(coord_channel);

    ID id; id.set_id(std::stoi(username));
    ServerInfo serverinfo;
    ClientContext ctx;

    std::cout << "Requesting server assignment from Coordinator (" << coord_addr << ")..." << std::endl;
    log(INFO, "Requesting server assignment from Coordinator at " + coord_addr);

    Status stat = coord_stub->GetServer(&ctx, id, &serverinfo);
    if (!stat.ok()) { 
        std::cout << "Command failed" << std::endl; 
        log(ERROR, "Coordinator GetServer failed: " + stat.error_message());
        return -1; 
    }

    server_address_ = serverinfo.hostname() + ":" + serverinfo.port();
    std::cout << "Assigned to Server at " << server_address_ << std::endl;
    log(INFO, "Assigned to Server at " + server_address_);

    auto channel = grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials());
    if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5))) {
        std::cout << "Command failed" << std::endl; 
        log(ERROR, "Failed to connect to server " + server_address_);
        return -1;
    }

    stub_ = SNSService::NewStub(channel);
    log(INFO, "Connected to SNS Server " + server_address_);

    IReply ire = Login();
    if (!ire.grpc_status.ok() || ire.comm_status != SUCCESS) { 
        std::cout << "Command failed" << std::endl; 
        log(ERROR, "Login RPC failed for user " + username);
        return -1; 
    }

    std::cout << "Command completed successfully" << std::endl;
    log(INFO, "Login successful for user " + username);
    return 1;
}

//////////////////////// utility ////////////////////////
bool Client::canReachServer() {
    if (server_address_.empty()) return false;
    auto ch = grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials());
    if (!ch->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(2))) return false;

    auto stub = SNSService::NewStub(ch);
    Request req; req.set_username(username);
    ListReply rep;
    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    Status s = stub->List(&ctx, req, &rep);
    return s.ok();
}

//////////////////////// processCommand ////////////////////////
IReply Client::processCommand(std::string& input) {
    IReply ire; // Default construction
    // Set default grpc_status to UNKNOWN with "unset" to allow framework to detect failure
    ire.grpc_status = Status(grpc::StatusCode::UNKNOWN, "unset");
    ire.comm_status = FAILURE_UNKNOWN;

    std::string cmd, arg;
    size_t pos = input.find(' ');
    if (pos != std::string::npos) { cmd = input.substr(0, pos); arg = input.substr(pos + 1); }
    else cmd = input;

    for (auto& c : cmd) c = std::tolower(c);
    log(INFO, "Processing command: " + cmd + (arg.empty() ? "" : " " + arg));

    if (cmd == "follow") {
        if (arg.empty()) {
            ire.grpc_status = Status(grpc::StatusCode::INVALID_ARGUMENT, "missing arg");
            ire.comm_status = FAILURE_INVALID;
            log(ERROR, "Follow command missing argument");
        } else {
            ire = Follow(arg);
        }
    } else if (cmd == "unfollow") {
        if (arg.empty()) {
            ire.grpc_status = Status(grpc::StatusCode::INVALID_ARGUMENT, "missing arg");
            ire.comm_status = FAILURE_INVALID;
            log(ERROR, "Unfollow command missing argument");
        } else {
            ire = UnFollow(arg);
        }
    } else if (cmd == "list") {
        ire = List();
    } else if (cmd == "timeline") {
        if (!canReachServer()) {
            ire.grpc_status = Status(grpc::StatusCode::UNAVAILABLE, "server unreachable");
            ire.comm_status = FAILURE_UNKNOWN;
            log(ERROR, "Server unreachable for timeline");
        } else {
            // Let run() call processTimeline() and handle the single prompt print
            ire.grpc_status = Status::OK;
            ire.comm_status = SUCCESS;
            input = "TIMELINE";
            log(INFO, "Entering timeline mode");
        }
    } else {
        ire.grpc_status = Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown cmd");
        ire.comm_status = FAILURE_INVALID;
        log(ERROR, "Unknown command: " + cmd);
    }

    if (!ire.grpc_status.ok() && ire.grpc_status.error_code() != grpc::StatusCode::OK) {
        std::cout << "Command failed" << std::endl;
    }

    return ire;
}

void Client::processTimeline() { Timeline(username); }

//////////////////////// RPCs ////////////////////////
IReply Client::List() {
    IReply ire;

    Request req;
    req.set_username(username);
    ListReply lr;
    ClientContext ctx;

    log(INFO, "Sending List RPC from client " + username);
    Status s = stub_->List(&ctx, req, &lr);
    ire.grpc_status = s;

    if (s.ok()) {
        ire.comm_status = SUCCESS;

        // Populate all_users
        for (const auto& user : lr.all_users()) {
            ire.all_users.push_back(user);
        }

        // Populate followers
        for (const auto& follower : lr.followers()) {
            ire.followers.push_back(follower);
        }

        log(INFO, "List RPC success. Total users: " + std::to_string(lr.all_users_size()));
    } else {
        ire.comm_status = FAILURE_UNKNOWN;
        log(ERROR, "List RPC failed: " + s.error_message());
    }

    return ire;
}

IReply Client::Follow(const std::string& u2) {
    IReply ire;
    Request req; req.set_username(username); req.add_arguments(u2);
    Reply rep; ClientContext ctx;
    log(INFO, "Sending Follow RPC from " + username + " → " + u2);
    Status s = stub_->Follow(&ctx, req, &rep);
    ire.grpc_status = s;
    ire.comm_status = s.ok() ? SUCCESS : FAILURE_UNKNOWN;
    if (!s.ok()) log(ERROR, "Follow RPC failed: " + s.error_message());
    return ire;
}

IReply Client::UnFollow(const std::string& u2) {
    IReply ire;
    Request req; req.set_username(username); req.add_arguments(u2);
    Reply rep; ClientContext ctx;
    log(INFO, "Sending UnFollow RPC from " + username + " → " + u2);
    Status s = stub_->UnFollow(&ctx, req, &rep);
    ire.grpc_status = s;
    ire.comm_status = s.ok() ? SUCCESS : FAILURE_UNKNOWN;
    if (!s.ok()) log(ERROR, "UnFollow RPC failed: " + s.error_message());
    return ire;
}

IReply Client::Login() {
    IReply ire;
    Request req; req.set_username(username);
    Reply rep; ClientContext ctx;
    log(INFO, "Attempting Login RPC for user " + username);
    Status s = stub_->Login(&ctx, req, &rep);
    ire.grpc_status = s;
    ire.comm_status = s.ok() ? SUCCESS : FAILURE_UNKNOWN;
    if (!s.ok()) log(ERROR, "Login RPC failed: " + s.error_message());
    return ire;
}

//////////////////////// Timeline(Pass "Now you are in the timeline" to framework) ////////////////////////
extern std::string getPostMessage();
void Client::Timeline(const std::string& username) {
    grpc::ClientContext ctx;
    ctx.AddMetadata("username", username);
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    auto ch = grpc::CreateChannel(server_address_, grpc::InsecureChannelCredentials());
    if (!ch->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(2))) {
        // Do not print message here either, let run() print once based on Finish() status
        log(ERROR, "Timeline connection failed for user " + username);
        return;
    }

    auto stub = SNSService::NewStub(ch);
    auto stream = stub->Timeline(&ctx);
    if (!stream) {
        log(ERROR, "Timeline stream creation failed for user " + username);
        return;
    }

    log(INFO, "Timeline stream started for user " + username);
    Message handshake = MakeMessage(username, "[handshake]");
    if (!stream->Write(handshake)) return;

    auto* s = stream.get();
    std::thread reader([&]() {
        Message msg;
        while (s->Read(&msg)) {
            std::time_t tt = static_cast<std::time_t>(msg.timestamp().seconds());
            displayPostMessage(msg.username(), msg.msg(), tt);
        }
        log(INFO, "Timeline reader thread ended for user " + username);
    });

    std::thread writer([&]() {
        while (true) {
            std::string text = getPostMessage();
            Message m = MakeMessage(username, text);
            if (!s->Write(m)) break;
        }
        s->WritesDone();
        log(INFO, "Timeline writer thread ended for user " + username);
    });

    writer.join();
    reader.join();

    Status st = stream->Finish();
    (void)st; // Framework handles unified printing
    log(INFO, "Timeline session closed for user " + username);
}

//////////////////////// main ////////////////////////
int main(int argc, char** argv) {
    std::string host = "localhost", user = "1", port = "9090";
    int opt = 0;
    while ((opt = getopt(argc, argv, "h:k:u:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'k': port = optarg; break;
        case 'u': user = optarg; break;
        default: std::cout << "Invalid Command Line Argument\n";
        }
    }

    // ✅ Initialize glog
    std::string log_file_name = "client-" + user;
    google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging Initialized. Client starting...");

    std::cout << "Logging Initialized. Client starting..." << std::endl;
    Client c(host, user, port);
    c.run();  // Framework handles unified printing: success/failure + "Now you are in the timeline"

    log(INFO, "Client exiting normally");
    google::ShutdownGoogleLogging(); // ✅ Shutdown glog
    return 0;
}