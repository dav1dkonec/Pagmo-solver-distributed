#include <catch2/catch_test_macros.hpp>
#include "dealer_socket.h"
#include "router_socket.h"
#include "MsgType.h"
#include "zmq.hpp"
#include <thread>

TEST_CASE("Socket messaging with inproc transport", "[socket]") {
    zmq::context_t ctx;
    
    SECTION("dealer-router round-trip") {
        distributed::router_socket router(ctx);
        distributed::dealer_socket dealer(ctx);
        
        router.bind("inproc://test_socket_1");
        dealer.connect("inproc://test_socket_1");
        dealer.set_routing_id("client1");
        
        get_dll_request req("test.dll", "linux-x86_64");
        REQUIRE_NOTHROW(dealer.send(MsgType::GET_DLL, req));
    }
    
    SECTION("multiple clients") {
        distributed::router_socket router(ctx);
        distributed::dealer_socket dealer1(ctx);
        distributed::dealer_socket dealer2(ctx);
        
        router.bind("inproc://test_socket_2");
        dealer1.connect("inproc://test_socket_2");
        dealer1.set_routing_id("client1");
        dealer2.connect("inproc://test_socket_2");
        dealer2.set_routing_id("client2");
        
        REQUIRE_NOTHROW(dealer1.send(MsgType::GET_DLL, get_dll_request("lib1.dll", "linux-x86_64")));
        REQUIRE_NOTHROW(dealer2.send(MsgType::GET_DLL, get_dll_request("lib2.dll", "windows-x86_64")));
    }
}
