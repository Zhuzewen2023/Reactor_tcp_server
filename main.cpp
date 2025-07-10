#include "reactor.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    std::cout << "Usage: " << argv[0] << " <ip> <port> " << std::endl;
    if (argc != 3) {
        std::cerr << "Invalid arguments" << std::endl;
        return 1;
    }

    std::string ip = argv[1];
    int port = std::stoi(argv[2]);

    auto acceptor = std::make_shared<HttpAcceptor>(ip, port);
    acceptor->init();
    Reactor::get_instance().run();
    return 0;
}