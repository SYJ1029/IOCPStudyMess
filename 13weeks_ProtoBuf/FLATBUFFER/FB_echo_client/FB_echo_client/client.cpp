#include <iostream>
#include <sdkddkver.h>
#include <asio.hpp>
#include <flatbuffers/flatbuffers.h>
#include "protocol_generated.h"

constexpr   int PORT = 3500;
constexpr   int BUFSIZE = 1024;

std::string read_message(asio::ip::tcp::socket& s)
{
    char buf[BUFSIZE];
    // 1. 메모리에 Flatbuffers로 생성된 데이터를 넣는다.
    size_t len = s.read_some(asio::buffer(buf, BUFSIZE));

    // 2. 패킷 타입에 맞는 flatbuffer 객체를 생성한다.
    //        GetRoot 템플릿을 사용한다.
    //        flatc가 생성한 table에 해당하는 객체의 포인터가 나온다.
    auto packet = flatbuffers::GetRoot<Message_S2C>(buf);

    // 3. 객체에서 원하는 값을 꺼낸다.
    //     값을 직접 access는 할 수 없고, 함수 형태로 read만 가능하다.
    auto mess = packet->msg();

    // 4. 읽은 값을 원하는 대로 사용
    //     std::string으로 사용하고 싶으면 GetString이라는 함수를 사용한다.
    return flatbuffers::GetString(mess);
}

void send_message(asio::ip::tcp::socket& s, char *buf)
{
    std::string mess(buf);

    // 1. 무조건 처음에 작성하는 빌더 객체  fbb
    flatbuffers::FlatBufferBuilder fbb;

    // 2. 패킷에 들어갈 데이터 fbb 작성
    //   단, 기본 자료 구조는 예외 (int, char, bool)
    //   string, struct 는 반드시 먼저 작성해야 한다.
    auto fb_mess = fbb.CreateString(mess);

    // 3. 패킷 생성 객체를 만든다.  "패킷"Builder 객체
    Message_C2SBuilder packet(fbb);

    // 4. 패킷에 값들을 저장
    //    add_"필드이름" 메소드를 사용한다.
    packet.add_msg(fb_mess);


    // 5. Finish() 호출해 준다.
    auto message = packet.Finish();

    // 6. fbb도 Finish()를 호출해 준다.
    //    Finish를 호출하기 전에는 데이터가 하나도 들어가 있지 않고, Finish를 호출해야 add_"필드이름"으로 추가한 data가 저장된다.
    //    Finish를 호출했으면 이후 더 추가할 수 없다. 끝이다.
    fbb.Finish(message);

    // 7. Binary Data가 만들어 졌으니 사용하면 된다.
    //       GetBUfferPointer()로 주소를 알 수 있고, GetSize()로 크기를 알 수 있다.
    asio::write(s, asio::buffer(fbb.GetBufferPointer(), fbb.GetSize()));
}

int main(int argc, char* argv[])
{
    try {
        asio::io_context io_context;
        asio::ip::tcp::socket c_socket(io_context);
        c_socket.connect(asio::ip::tcp::endpoint(asio::ip::address::from_string("127.0.0.1"), PORT));
        for (;;) {
            char buf[1024 + 1];
            std::cout << "Enter Message : ";
            std::cin.getline(buf, BUFSIZE);
            send_message(c_socket, buf);
            std::string message = read_message(c_socket);
            std::cout << message.size() << " bytes received: " << message << std::endl;
        }
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
}