#include <iostream>
#include <sdkddkver.h>
#include <asio.hpp>
#include <flatbuffers/flatbuffers.h>
#include "protocol_generated.h"

constexpr int PORT = 3500;
constexpr   int BUFSIZE = 1024;

std::string read_message(asio::ip::tcp::socket& s)
{
    char buf[BUFSIZE];

    size_t len = s.read_some(asio::buffer(buf, BUFSIZE));

    auto packet = flatbuffers::GetRoot<Message_C2S>(buf);
    auto mess = packet->msg();
    return flatbuffers::GetString(mess);
}

void send_message(asio::ip::tcp::socket& s, std::string &mess)
{
    flatbuffers::FlatBufferBuilder fbb;
    auto fb_mess = fbb.CreateString(mess);
    Message_S2CBuilder packet(fbb);
    packet.add_msg(fb_mess);
    auto message = packet.Finish();
    fbb.Finish(message);
    asio::write(s, asio::buffer(fbb.GetBufferPointer(), fbb.GetSize()));
}

int main(int argc, char* argv[])
{
    try {
        asio::io_context io_context;
        asio::ip::tcp::acceptor my_acceptor{ io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), PORT) };
        asio::ip::tcp::socket c_socket = my_acceptor.accept();
        for (;;) {
            std::string mess = read_message(c_socket);
            std::cout << mess.size() << " bytes received: " << mess << std::endl;
            send_message(c_socket, mess);
        }
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
}