#include <iostream>
#include <WS2tcpip.h>
#include <array>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include "protocol.h"
#include <tbb/concurrent_unordered_map.h>
#include <mutex>
#include <unordered_set>


#include <tbb/concurrent_unordered_set.h>
#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "WS2_32.lib")
using namespace std;

constexpr int BUF_SIZE = 200;
constexpr int VIEW_RANGE = 5;

std::atomic<int> player_index = 1;

enum IOType { IO_SEND, IO_RECV, IO_ACCEPT };

class EXP_OVER {
public:
	WSAOVERLAPPED m_over;
	IOType  m_iotype;
	WSABUF	m_wsa;
	SOCKET  m_client_socket;
	char  m_buff[BUF_SIZE];
	EXP_OVER()
	{
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_buff;
		m_wsa.len = BUF_SIZE;
	}
	EXP_OVER(IOType iot) : m_iotype(iot)
	{
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_buff;
		m_wsa.len = BUF_SIZE;
	}
};

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
	//while (true);   // 디버깅 용
	LocalFree(lpMsgBuf);
}

enum CL_STATE { CS_CONNECT, CS_PLAYING, CS_LOGOUT };

constexpr int SECTOR_SIZE = VIEW_RANGE * 2 + 1;
constexpr int MAX_SECTORS_X = (WORLD_WIDTH + SECTOR_SIZE - 1) / SECTOR_SIZE;
constexpr int MAX_SECTORS_Y = (WORLD_HEIGHT + SECTOR_SIZE - 1) / SECTOR_SIZE;

constexpr int SECTOR_X(int x) { return x / SECTOR_SIZE; }
constexpr int SECTOR_Y(int y) { return y / SECTOR_SIZE; }
constexpr int SECTOR_ID(int sx, int sy) { return sy * MAX_SECTORS_X + sx; }

class SECTOR
{
private:
public:
	std::unordered_set<int> m_players; // IDs of players in this sector
	std::mutex m_mutex; // Mutex to protect access to m_players
public:
	SECTOR() {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_players.clear();
	}

	~SECTOR() {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_players.clear();
	}

public:
	void add_player(int player_id) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_players.insert(player_id);
	}
	void remove_player(int player_id) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_players.erase(player_id);
	}
};

class SectorManager
{
public:
	SectorManager()
	{
		for (int i = 0; i < MAX_SECTORS_X * MAX_SECTORS_Y; ++i) {
			sectors[i] = std::make_shared<SECTOR>();
		}
	}

	void add_player_to_sector(int player_id, short x, short y) {
		int sector_id = SECTOR_ID(SECTOR_X(x), SECTOR_Y(y));
		sectors[sector_id]->add_player(player_id);
	}

	void remove_player_from_sector(int player_id, short x, short y) {
		int sector_id = SECTOR_ID(SECTOR_X(x), SECTOR_Y(y));
		sectors[sector_id]->remove_player(player_id);
	}

public:
	std::vector<int> get_players_in_sector(short x, short y) {
		int sid = SECTOR_ID(SECTOR_X(x), SECTOR_Y(y));
		std::lock_guard<std::mutex> lg(sectors[sid]->m_mutex);
		return { sectors[sid]->m_players.begin(), sectors[sid]->m_players.end() };
	}

private:
	std::array<std::shared_ptr<SECTOR>, MAX_SECTORS_X* MAX_SECTORS_Y> sectors;
};


class SESSION {
public:
	SOCKET m_client;
	int m_id;
	CL_STATE m_state;
	EXP_OVER m_recv_over;
	int m_prev_recv;
	char m_username[MAX_NAME_LEN];
	short m_x, m_y;
	int m_move_time;
	std::unordered_set<int> m_visible_players; // IDs of players currently visible to this session
	std::mutex m_visible_mutex; // Mutex to protect access to m_visible_players

	SESSION() {
		std::cout << "SESSION Creation Error!\n";
		exit(-1);
	}
	SESSION(SOCKET s, int id) : m_client(s), m_id(id) {
		m_state = CS_CONNECT;
		m_recv_over.m_iotype = IO_RECV;
		m_x = rand() % WORLD_WIDTH;
		m_y = rand() % WORLD_HEIGHT;
		m_prev_recv = 0;
		m_move_time = 0;
	}
	~SESSION()
	{
		closesocket(m_client);
	}

	bool is_visible(short other_x, short other_y) {
		// 3D 게임에서는 사각형보다 원이 낫지만... 지금은 2D라서 사각형이여도 문제는 없다
		return abs(m_x - other_x) <= VIEW_RANGE
			&& abs(m_y - other_y) <= VIEW_RANGE;
	}

	void do_recv()
	{
		DWORD recv_flag = 0;
		memset(&m_recv_over.m_over, 0, sizeof(m_recv_over.m_over));
		WSARecv(m_client, &m_recv_over.m_wsa, 1, 0, &recv_flag, &m_recv_over.m_over, nullptr);
	}
	void do_send(int num_bytes, char* mess)
	{
		EXP_OVER* o = new EXP_OVER(IO_SEND);
		o->m_wsa.len = num_bytes;
		memcpy(o->m_buff, mess, num_bytes);
		WSASend(m_client, &o->m_wsa, 1, 0, 0, &o->m_over, nullptr);
	}
	void send_avatar_info()
	{
		S2C_AvatarInfo packet;
		packet.size = sizeof(S2C_AvatarInfo);
		packet.type = S2C_AVATAR_INFO;
		packet.playerId = m_id;
		packet.x = m_x;
		packet.y = m_y;
		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}
	void send_move_packet(int mover);
	void send_add_player(int player_id);
	void send_login_success()
	{
		S2C_LoginResult packet;
		packet.size = sizeof(S2C_LoginResult);
		packet.type = S2C_LOGIN_RESULT;
		packet.success = true;
		strncpy_s(packet.message, "Login successful.", sizeof(packet.message));
		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}
	void send_remove_player(int player_id)
	{
		S2C_RemovePlayer packet;
		packet.size = sizeof(S2C_RemovePlayer);
		packet.type = S2C_REMOVE_PLAYER;
		packet.playerId = player_id;

		m_visible_mutex.lock();
		if (m_visible_players.count(player_id) == 0) {
			m_visible_mutex.unlock();
			return; // Not visible, no need to send remove
		}
		m_visible_players.erase(player_id);
		m_visible_mutex.unlock();

		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}
	bool process_packet(unsigned char* p);
	void do_move(DIRECTION dir);
};

tbb::concurrent_unordered_map<int,
	std::atomic<std::shared_ptr<SESSION>>> clients;



SectorManager sector_manager;

SOCKET g_server;
HANDLE g_iocp;

void SESSION::send_add_player(int player_id)
{
	S2C_AddPlayer packet;
	packet.size = sizeof(S2C_AddPlayer);
	packet.type = S2C_ADD_PLAYER;
	packet.playerId = player_id;
	std::shared_ptr<SESSION> pl = clients[player_id].load();
	if (nullptr == pl) return;
	memcpy(packet.username, pl->m_username, sizeof(packet.username));
	packet.x = pl->m_x;
	packet.y = pl->m_y;

	m_visible_mutex.lock();
	if (m_visible_players.count(player_id) > 0) {
		m_visible_mutex.unlock();
		return; // Already visible, no need to send again
	}
	m_visible_players.insert(player_id);
	m_visible_mutex.unlock();
	do_send(packet.size, reinterpret_cast<char*>(&packet));
}

void SESSION::do_move(DIRECTION dir)
{
	auto old_v = m_visible_players; // 새로 검색할 필요가 없다

	switch (dir) {
	case UP: m_y = max(0, m_y - 1); break;
	case DOWN: m_y = min(WORLD_HEIGHT - 1, m_y + 1); break;
	case LEFT: m_x = max(0, m_x - 1); break;
	case RIGHT: m_x = min(WORLD_WIDTH - 1, m_x + 1); break;
	}

	std::unordered_set<int> new_v;

	// cout << "Player[" << m_id << "] moved to (" << m_x << ", " << m_y << ")\n";

	for (auto& cl : sector_manager.get_players_in_sector(m_x, m_y)) {
		std::shared_ptr<SESSION> pl = clients[cl];
		if (nullptr == pl) continue;
		if (CS_PLAYING != pl->m_state) continue;
		if (is_visible(pl->m_x, pl->m_y)) {
			new_v.insert(pl->m_id);
		}

	}

	send_move_packet(m_id); // 자기 자신에게도 이동 사실을 전달

	for (int id : new_v) {
		if (old_v.count(id) == 0) {
			// 새로 보이는 플레이어
			send_add_player(id);
			std::shared_ptr<SESSION> pl = clients[id];
			if (nullptr == pl) continue;
			pl->send_add_player(m_id);
		}
	}


	for (int id : old_v) {
		if (new_v.count(id) == 0) {
			// 더 이상 보이지 않는 플레이어
			send_remove_player(id);
			std::shared_ptr<SESSION> pl = clients[id];
			if (nullptr == pl) continue;
			pl->send_remove_player(m_id);
		}
		else
		{
			// 계속 보이는 플레이어
			std::shared_ptr<SESSION> pl = clients[id];
			if (nullptr == pl) continue;
			pl->send_move_packet(m_id);
		}
	}
}

bool SESSION::process_packet(unsigned char* p)
{
	PACKET_TYPE type = *reinterpret_cast<PACKET_TYPE*>(&p[1]);
	switch (type) {
	case C2S_LOGIN: {
		C2S_Login* packet = reinterpret_cast<C2S_Login*>(p);
		strncpy_s(m_username, packet->username, MAX_NAME_LEN);
		cout << "Player[" << m_id << "] logged in as " << m_username << endl;
		send_avatar_info();
		m_state = CS_PLAYING;
		sector_manager.add_player_to_sector(m_id, m_x, m_y);
		for (auto& c : sector_manager.get_players_in_sector(m_x, m_y)) {
			std::shared_ptr<SESSION> pl = clients[c];
			if (nullptr == pl) continue;
			if (pl->m_id == m_id) continue;
			if (false == is_visible(pl->m_x, pl->m_y)) continue;
			if (pl->m_state != CS_PLAYING) continue;
			send_add_player(pl->m_id);
			pl->send_add_player(m_id);
		}
	}
				  break;
	case C2S_MOVE: {
		C2S_Move* packet = reinterpret_cast<C2S_Move*>(p);
		DIRECTION dir = packet->dir;
		m_move_time = packet->move_time;
		do_move(dir);
	}
				 break;
	default:
		cout << "Unknown packet type received from player[" << m_id << "].\n";
		return false;
		break;
	}
	return true;
}

void SESSION::send_move_packet(int mover)
{
	S2C_MovePlayer packet;
	packet.size = sizeof(S2C_MovePlayer);
	packet.type = S2C_MOVE_PLAYER;
	packet.playerId = mover;
	std::shared_ptr<SESSION> pl = clients[mover];
	if (nullptr == pl) return;
	packet.x = pl->m_x;
	packet.y = pl->m_y;
	packet.move_time = pl->m_move_time;
	do_send(packet.size, reinterpret_cast<char*>(&packet));
}

void send_login_fail(SOCKET client, const char* message)
{
	S2C_LoginResult packet;
	packet.size = sizeof(S2C_LoginResult);
	packet.type = S2C_LOGIN_RESULT;
	packet.success = false;
	strncpy_s(packet.message, message, sizeof(packet.message));
	WSABUF wsa_buf;
	wsa_buf.buf = reinterpret_cast<char*>(&packet);
	wsa_buf.len = packet.size;
	WSASend(client, &wsa_buf, 1, 0, 0, nullptr, nullptr);
}

void disconnect(int key)
{
	std::cout << "client[" << key << "] Disconnected.\n";
	std::shared_ptr<SESSION> cl = clients[key].load();
	if (nullptr != cl) {
		cl->m_state = CS_LOGOUT;
		for (auto& other : cl->m_visible_players) {
			std::shared_ptr<SESSION> o = clients[other];
			if (nullptr == o) continue;
			if (CS_PLAYING == o->m_state)
				o->send_remove_player(key);
		}
		closesocket(cl->m_client);
		cl->m_client = INVALID_SOCKET;

		sector_manager.remove_player_from_sector(key, cl->m_x, cl->m_y);
	}
	clients[key].store(nullptr);
}

void worker_thread()
{
	for (;;) {
		DWORD num_bytes;
		ULONG_PTR long_key;
		LPOVERLAPPED over;
		BOOL ret = GetQueuedCompletionStatus(g_iocp, &num_bytes, &long_key, &over, INFINITE);
		int key = static_cast<int>(long_key);
		if (TRUE != ret) {
			error_display(L"GQCS Errror: ", WSAGetLastError());
			if (key == -1) {
				exit(-1);
			}
			disconnect(key);
			continue;
		}
		EXP_OVER* exp_over = reinterpret_cast<EXP_OVER*>(over);
		switch (exp_over->m_iotype) {
		case IO_ACCEPT:
			cout << "Client connected." << endl;
			if (MAX_PLAYERS <= clients.size()) {
				cout << "No more player can be accepted." << endl;
				send_login_fail(exp_over->m_client_socket, "Server is full.");
				closesocket(exp_over->m_client_socket);
			}
			else {
				int my_id = player_index++;
				CreateIoCompletionPort((HANDLE)exp_over->m_client_socket, g_iocp, my_id, 0);
				std::shared_ptr<SESSION> new_pl = std::make_shared<SESSION>(exp_over->m_client_socket, my_id);
				clients[my_id] = new_pl;
				new_pl->send_login_success();
				new_pl->do_recv();
			}
			exp_over->m_client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			AcceptEx(g_server, exp_over->m_client_socket, &exp_over->m_buff, 0,
				sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
				NULL, &exp_over->m_over);
			break;
		case IO_RECV:
		{
			// cout << "Client[" << key << "] sent a message." << endl;
			if (0 == num_bytes) {
				disconnect(key);
				break;
			}
			std::shared_ptr<SESSION> cl = clients[key];
			if (nullptr == cl) {
				cout << "Session not found for client[" << player_index << "].\n";
				break;
			}
			unsigned char* p = reinterpret_cast<unsigned char*>(exp_over->m_buff);
			int data_size = num_bytes + cl->m_prev_recv;
			while (data_size > 0) {
				int packet_size = p[0];
				if (packet_size > data_size) break;
				if (false == cl->process_packet(p)) {
					disconnect(key);
					break;
				}
				p += packet_size;
				data_size -= packet_size;
			}
			if (data_size > 0) {
				memmove(cl->m_recv_over.m_buff, p, data_size);
				cl->m_prev_recv = data_size;
			}
			cl->do_recv();
		}
		break;
		case IO_SEND: {
			// cout << "Message sent. to client[" << key << "]\n";
			EXP_OVER* o = reinterpret_cast<EXP_OVER*>(over);
			delete o;
		}
					break;
		default:
			cout << "Unknown IO type." << endl;
			exit(-1);
			break;
		}
	}
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	g_server = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	::bind(g_server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(g_server, SOMAXCONN);
	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	CreateIoCompletionPort((HANDLE)g_server, g_iocp, -1, 0);

	EXP_OVER accept_over(IO_ACCEPT);
	accept_over.m_client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	AcceptEx(g_server, accept_over.m_client_socket, &accept_over.m_buff, 0,
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
		NULL, &accept_over.m_over);

	vector <thread> worker_threads;
	int num_threads = thread::hardware_concurrency();

	for (int i = 0; i < num_threads; ++i)
		worker_threads.emplace_back(worker_thread);
	for (auto& th : worker_threads)
		th.join();

	closesocket(g_server);
	WSACleanup();
}
