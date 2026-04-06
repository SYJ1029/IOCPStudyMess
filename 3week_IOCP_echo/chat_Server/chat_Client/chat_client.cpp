#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <SFML/System.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

const char* SERVER_IP = "127.0.0.1";
constexpr short SERVER_PORT = 4000;
constexpr int BUFFER_SIZE = 4096;

char g_recv_buffer[BUFFER_SIZE];
char g_send_buffer[BUFFER_SIZE];
WSABUF g_recv_wsa_buf{ BUFFER_SIZE, g_recv_buffer };
WSABUF g_send_wsa_buf{ BUFFER_SIZE, g_send_buffer };
WSAOVERLAPPED g_recv_overlapped{}, g_send_overlapped{};
SOCKET g_s_socket;
int g_my_id = -1;

// 채팅 로그 (SFML 윈도우 출력용)
std::vector<sf::String> g_chat_log;
const int MAX_CHAT_LINES = 20;

void add_chat_line(const sf::String& line)
{
    g_chat_log.push_back(line);
    if (g_chat_log.size() > MAX_CHAT_LINES)
        g_chat_log.erase(g_chat_log.begin());
}

#pragma pack(push, 1)
class PACKET {
public:
    unsigned char m_size;
    unsigned char m_sender_id;
    char m_buf[BUFFER_SIZE];
    PACKET() : m_size(0), m_sender_id(0) { m_buf[0] = '\0'; }
    PACKET(int sender, const char* mess) : m_sender_id(static_cast<unsigned char>(sender))
    {
        strcpy_s(m_buf, mess);
        m_size = static_cast<unsigned char>(strlen(mess) + 1 + 2); // data + null + header(2)
    }
};
#pragma pack(pop)

void error_display(const wchar_t* msg, int err_no)
{
    WCHAR* lpMsgBuf;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, err_no,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&lpMsgBuf, 0, NULL);
    std::wcout << msg;
    std::wcout << L" === 에러 " << lpMsgBuf << std::endl;
    while (true);
    LocalFree(lpMsgBuf);
}
void recv_from_server();

void CALLBACK recv_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
    if (error != 0) {
        error_display(L"데이터 수신 실패", WSAGetLastError());
        exit(1);
    }

    // 서버가 PACKET을 그대로 브로드캐스트했으므로 PACKET으로 파싱
    int remain = static_cast<int>(bytes_transferred);
    char* ptr = g_recv_buffer;

    while (remain >= 2) {
        unsigned char pkt_size = static_cast<unsigned char>(*ptr);
        if (pkt_size > remain) break;

        PACKET* packet = reinterpret_cast<PACKET*>(ptr);
        int id = packet->m_sender_id;
		// m_buf의 맨 첫번째 글자는 전달받은 문자열이 아니라 size이므로 얘는 출력하지 않아야 함
		std::string token(packet->m_buf);
		token.erase(0, 2); // 첫 글자 제거
		std::string msg = "Client[" + std::to_string(id) + "]: " + (token);
        std::cout << msg << std::endl;
        add_chat_line(sf::String(msg));

        ptr += pkt_size;
        remain -= pkt_size;
    }

    recv_from_server();
}

void CALLBACK send_callback(DWORD error, DWORD bytes_transferred, LPWSAOVERLAPPED overlapped, DWORD flags)
{
    if (error != 0) {
        error_display(L"데이터 전송 실패", WSAGetLastError());
        return;
    }
    std::cout << "Sent to server: SIZE: " << bytes_transferred << std::endl;
}

void recv_from_server()
{
    DWORD recv_flag = 0;
    ZeroMemory(&g_recv_overlapped, sizeof(g_recv_overlapped));
    int result = WSARecv(g_s_socket, &g_recv_wsa_buf, 1, nullptr, &recv_flag, &g_recv_overlapped, recv_callback);
    if (result == SOCKET_ERROR) {
        int err_no = WSAGetLastError();
        if (err_no != WSA_IO_PENDING) {
            error_display(L"데이터 수신 실패", err_no);
            exit(1);
        }
    }
}

int main()
{
    sf::RenderWindow window(sf::VideoMode(1180, 600), "CHAT WINDOW", sf::Style::Default);
    sf::Font font;
    if (!font.loadFromFile("cour.ttf"))
        return EXIT_FAILURE;

    sf::Event event;
    sf::String playerInput;
    sf::Text playerText("", font, 20);
    playerText.setPosition(60, 560);
    playerText.setFillColor(sf::Color::Yellow);

    // 입력창 구분선
    sf::RectangleShape divider(sf::Vector2f(1160, 2));
    divider.setPosition(10, 545);
    divider.setFillColor(sf::Color::White);

    // 입력 프롬프트
    sf::Text promptText("> ", font, 20);
    promptText.setPosition(10, 560);
    promptText.setFillColor(sf::Color::White);

    std::wcout.imbue(std::locale("korean"));
    WSADATA wsa_data{};
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
    g_s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    SOCKADDR_IN server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    int result = WSAConnect(g_s_socket, reinterpret_cast<SOCKADDR*>(&server_addr), sizeof(server_addr), nullptr, nullptr, nullptr, nullptr);
    if (result == SOCKET_ERROR) {
        error_display(L"서버 연결 실패", WSAGetLastError());
        return 1;
    }

    recv_from_server();

    while (window.isOpen())
    {
        window.clear();
        while (window.pollEvent(event))
        {
            if (event.type == sf::Event::Closed) {
                window.close();
                break;
            }

            if (event.type == sf::Event::TextEntered)
            {
                if (event.text.unicode < 128)
                {
                    if (13 == event.text.unicode) {
                        std::string message = playerInput.toAnsiString();
                        playerInput.clear();

                        if (!message.empty()) {
                            // PACKET으로 포장하여 전송
                            PACKET pkt(g_my_id, message.c_str());
                            g_send_wsa_buf.len = pkt.m_size;
                            memcpy(g_send_wsa_buf.buf, &pkt, pkt.m_size);
                            ZeroMemory(&g_send_overlapped, sizeof(g_send_overlapped));
                            DWORD sent_size = 0;
                            int result = WSASend(g_s_socket, &g_send_wsa_buf, 1, &sent_size, 0, &g_send_overlapped, send_callback);
                            if (result == SOCKET_ERROR) {
                                int err_no = WSAGetLastError();
                                if (err_no != WSA_IO_PENDING) {
                                    error_display(L"데이터 전송 실패", err_no);
                                    exit(1);
                                }
                            }
                        }
                    }
                    else if (8 == event.text.unicode) {
                        if (playerInput.getSize() > 0)
                            playerInput.erase(playerInput.getSize() - 1);
                    }
                    else playerInput += event.text.unicode;
                    playerText.setString(playerInput);
                }
            }
        }

        // 채팅 로그 출력
        for (size_t i = 0; i < g_chat_log.size(); ++i) {
            sf::Text chatLine(g_chat_log[i], font, 18);
            chatLine.setPosition(10, 10 + static_cast<float>(i) * 25);
            chatLine.setFillColor(sf::Color::White);
            window.draw(chatLine);
        }

        window.draw(divider);
        window.draw(promptText);
        window.draw(playerText);
        window.display();
        SleepEx(0, TRUE);
    }

    closesocket(g_s_socket);
    WSACleanup();
    return 0;
}