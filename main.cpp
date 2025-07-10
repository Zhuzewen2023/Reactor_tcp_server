#include "reactor.hpp"

int main() {
    auto acceptor = std::make_shared<HttpAcceptor>(8080);
    acceptor->init();
    Reactor::get_instance().run();
    return 0;
}