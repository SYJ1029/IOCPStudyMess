#include <iostream>
#include <WS2tcpip.h>
#include <array>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include "protocol.h"
#include <tbb/concurrent_unordered_map.h>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <concurrent_priority_queue.h>
#include "include/lua.hpp"

#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "WS2_32.lib")

constexpr int BUF_SIZE = 200;
constexpr int VIEW_RANGE = 5;
constexpr int MOVE_COOL_TIME = 1000; // ms

constexpr int EVENT_MOVE = 1;

struct event_type {
	int obj_id;
	std::chrono::system_clock::time_point wakeup_time;
	int event_id;
	int target_id;

	constexpr bool operator < (const event_type& _Left) const
	{
		return (wakeup_time > _Left.wakeup_time);
	}
};
concurrency::concurrent_priority_queue<event_type> timer_queue;

std::atomic<int> g_num_players = 0;
std::atomic<int> player_index = 1;

enum IOType { IO_SEND, IO_RECV, IO_ACCEPT, IO_NPCMOVE, IO_PLAYER_MOVE };

class EXP_OVER {
public:
	WSAOVERLAPPED m_over;
	IOType  m_iotype;
	WSABUF	m_wsa;
	SOCKET  m_client_socket;
	char  m_buff[BUF_SIZE];
	int m_target_id;
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

SOCKET g_server;
HANDLE g_iocp;

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
	std::unordered_set<int> m_visible_players;
	std::mutex m_visible_mutex;
	std::atomic<bool> _active_npc;
	std::chrono::system_clock::time_point npc_last_move_time;
	lua_State* L;
	std::mutex m_lua_mutex;
	
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
		if (is_player() == true)
			g_num_players--;
	}

	bool is_npc() const
	{
		return m_id >= NPC_ID_START;
	}

	bool is_player() const
	{
		return m_id < NPC_ID_START;
	}
	bool is_visible(short x, short y)
	{
		return abs(m_x - x) <= VIEW_RANGE
			&& abs(m_y - y) <= VIEW_RANGE;
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
		S2C_RemoveObject packet;
		packet.size = sizeof(S2C_RemoveObject);
		packet.type = S2C_REMOVE_OBJECT;
		packet.object_id = player_id;

		m_visible_mutex.lock();
		if (m_visible_players.count(player_id) == 0) {
			m_visible_mutex.unlock();
			return;
		}
		m_visible_players.erase(player_id);
		m_visible_mutex.unlock();

		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}

	void send_chat(int player_id, char* mess)
	{
		S2C_ChatMessage packet;;
		packet.size = sizeof(S2C_ChatMessage);
		packet.type = S2C_CHAT_MESSAGE;
		packet.object_id = player_id;

		strcpy_s(packet.message, mess);
		do_send(packet.size, reinterpret_cast<char*>(&packet));
	}
	bool process_packet(unsigned char* p);
	void do_random_move();

	void move_notice(int mover)
	{
		EXP_OVER* o = new EXP_OVER(IO_PLAYER_MOVE);
		o->m_target_id = mover;
		PostQueuedCompletionStatus(g_iocp, 0, m_id, &o->m_over);
	}
};

tbb::concurrent_unordered_map<int,
	std::atomic<std::shared_ptr<SESSION>>> clients;


void SESSION::do_random_move()
{
	if (false == is_npc()) return;
	npc_last_move_time = std::chrono::system_clock::now();
	int dir = rand() % 4;
	short new_x = m_x, new_y = m_y;
	switch (dir) {
	case 0: new_y -= 1; break; // Up
	case 1: new_y += 1; break; // Down
	case 2: new_x -= 1; break; // Left
	case 3: new_x += 1; break; // Right
	}
	// 월드 경계 체크
	if (new_x < 0 || new_x >= WORLD_WIDTH || new_y < 0 || new_y >= WORLD_HEIGHT) {
		return;
	}
	std::unordered_set<int> old_v;
	for (auto &obj : clients) {
		std::shared_ptr<SESSION> pl = obj.second.load();
		if (nullptr == pl) continue;
		if (pl->is_player() == false) continue;
		if (pl->m_state != CS_PLAYING) continue;
		if (is_visible(pl->m_x, pl->m_y))
			old_v.insert(pl->m_id);
	}
	m_x = new_x;
	m_y = new_y;
	std::unordered_set<int> new_v;
	for (auto& obj : clients) {
		std::shared_ptr<SESSION> pl = obj.second.load();
		if (nullptr == pl) continue;
		if (pl->is_player() == false) continue;
		if (pl->m_state != CS_PLAYING) continue;
		if (is_visible(pl->m_x, pl->m_y))
			new_v.insert(pl->m_id);
	}

	for (int pl : new_v) {
		if (old_v.count(pl) == 0) {  // 새로 시야에 들어옴
			std::shared_ptr<SESSION> pl_session = clients[pl].load();
			if (nullptr == pl_session) continue;
			pl_session->send_add_player(m_id);
		}
		else {  // 여전히 시야에 있음
			std::shared_ptr<SESSION> pl_session = clients[pl].load();
			if (nullptr == pl_session) continue;
			pl_session->send_move_packet(m_id);
		}
	}
	for (int pl : old_v) {
		if (new_v.count(pl) == 0) {  // 시야에서 사라짐
			std::shared_ptr<SESSION> pl_session = clients[pl].load();
			if (nullptr == pl_session) continue;
			pl_session->send_remove_player(m_id);
		}
	}
}
void SESSION::send_add_player(int player_id)
{
	S2C_AddObject packet;
	packet.size = sizeof(S2C_AddObject);
	packet.type = S2C_ADD_OBJECT;
	packet.object_id = player_id;
	std::shared_ptr<SESSION> pl = clients[player_id].load();
	if (nullptr == pl) return;
	memcpy(packet.obj_name, pl->m_username, sizeof(packet.obj_name));
	packet.x = pl->m_x;
	packet.y = pl->m_y;

	m_visible_mutex.lock();
	if (m_visible_players.count(player_id) > 0) {
		m_visible_mutex.unlock();
		return;
	}
	m_visible_players.insert(player_id);
	m_visible_mutex.unlock();

	do_send(packet.size, reinterpret_cast<char*>(&packet));
}

bool SESSION::process_packet(unsigned char* p)
{
	PACKET_TYPE type = *reinterpret_cast<PACKET_TYPE*>(&p[1]);
	switch (type) {
	case C2S_LOGIN: {
		C2S_Login* packet = reinterpret_cast<C2S_Login*>(p);
		strncpy_s(m_username, packet->username, MAX_NAME_LEN);
		//cout << "Player[" << m_id << "] logged in as " << m_username << endl;
		std::cout << ".";
		send_avatar_info();
		m_state = CS_PLAYING;

		for (auto& c : clients) {
			std::shared_ptr<SESSION> pl = c.second.load();
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
		m_move_time = packet->move_time;
		auto new_x = packet->x;
		auto new_y = packet->y;

		auto old_v = m_visible_players;

		if (abs(m_x - new_x) > 1 || abs(m_y - new_y) > 1) {
			std::cout << "Invalid move packet received from player[" << m_id << "].\n";
			send_move_packet(m_id);  // 클라이언트의 위치를 서버의 위치로 보정
			return false;
		}

		m_x = new_x;
		m_y = new_y;

		std::unordered_set<int> new_v;
		for (auto& c : clients) {
			std::shared_ptr<SESSION> pl = c.second.load();
			if (nullptr == pl) continue;
			if (pl->m_id == m_id) continue;
			if (pl->m_state != CS_PLAYING) continue;
			if (is_visible(pl->m_x, pl->m_y))
			{
				if (pl->is_npc())
				{
					pl->move_notice(m_id);
				}
				new_v.insert(pl->m_id);
			}
		}

		if (packet->move_time != 0)		//  STRESS TEST이므로 이동 delay를 전해 주어야 한다.
			send_move_packet(m_id);

		for (int id : new_v) {
			if (old_v.count(id) == 0) {  // 새로 시야에 들어옴
				send_add_player(id);
				std::shared_ptr<SESSION> pl = clients[id].load();
				if (nullptr == pl) continue;
				if (true == pl->is_player()) pl->send_add_player(m_id);
			}
			else {  // 여전히 시야에 있음
				std::shared_ptr<SESSION> pl = clients[id].load();
				if (nullptr == pl) continue;
				if (true == pl->is_player()) pl->send_move_packet(m_id);
			}
		}

		for (int id : old_v) {
			if (new_v.count(id) == 0) {  // 시야에서 사라짐
				send_remove_player(id);
				std::shared_ptr<SESSION> pl = clients[id].load();
				if (nullptr == pl) continue;
				if (true == pl->is_player()) pl->send_remove_player(m_id);
			}
		}
	}
				 break;
	default:
		std::cout << "Unknown packet type received from player[" << m_id << "].\n";
		return false;
		break;
	}
	return true;
}

void SESSION::send_move_packet(int mover)
{
	S2C_MoveObject packet;
	packet.size = sizeof(S2C_MoveObject);
	packet.type = S2C_MOVE_OBJECT;
	packet.object_id = mover;
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
	DWORD sent_bytes = 0;
	WSASend(client, &wsa_buf, 1, &sent_bytes, 0, nullptr, nullptr);
}

void disconnect(int key)
{
	std::cout << "client[" << key << "] Disconnected.\n";
	std::shared_ptr<SESSION> cl = clients[key].load();
	if (nullptr != cl) {
		cl->m_state = CS_LOGOUT;
		auto visible_copy = cl->m_visible_players;
		for (auto& other : visible_copy) {
			std::shared_ptr<SESSION> o = clients[other];
			if (nullptr == o) continue;
			if (CS_PLAYING == o->m_state)
				o->send_remove_player(key);
		}
		closesocket(cl->m_client);
		cl->m_client = INVALID_SOCKET;
	}
	clients[key].store(nullptr);
}

int API_SendMessage(lua_State* L)
{
	int my_id = (int)lua_tointeger(L, -3);
	int user_id = (int)lua_tointeger(L, -2);
	char* mess = (char*)lua_tostring(L, -1);
	lua_pop(L, 4);

	std::shared_ptr<SESSION> cl = clients[user_id];
	if (nullptr == cl)
	{
		std::cout << "API_SendMessage: User not found.\n";
		return 0;
	}
	else
		cl->send_chat(my_id, mess);	
	return 0;
}

int API_get_y(lua_State* L)
{
	int user_id =
		(int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	std::shared_ptr<SESSION> cl = clients[user_id];
	if(nullptr != cl)
	{
		int y = cl->m_y;
		lua_pushnumber(L, y);
		return 1;
	}
	else
	{
		lua_pushnumber(L, -1);
	}
	return 1;
}

int API_get_x(lua_State* L)
{
	int user_id =
		(int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	std::shared_ptr<SESSION> cl = clients[user_id];
	if (nullptr != cl)
	{
		int x = cl->m_x;
		lua_pushnumber(L, x);
		return 1;
	}
	else
	{
		lua_pushnumber(L, -1);
	}
	return 1;
}


void initialize_npcs()
{
	std::cout << "NPC Initialization begins.\n";
	for (int i = 0; i < NUM_NPCS; ++i) {
		int npc_id = i + NPC_ID_START;
		auto npc = std::make_shared<SESSION>(INVALID_SOCKET, npc_id);
		npc->npc_last_move_time = std::chrono::system_clock::now();
		npc->m_state = CS_PLAYING;
		sprintf_s(npc->m_username, "NPC_%d", npc_id);
		npc->m_x = rand() % WORLD_WIDTH;
		npc->m_y = rand() % WORLD_HEIGHT;
		event_type ev;
		ev.event_id = EVENT_MOVE;
		ev.obj_id = npc_id;
		ev.wakeup_time = npc->npc_last_move_time + std::chrono::seconds(1);
		timer_queue.push(ev);
		auto L = luaL_newstate();
		npc->L = L;
		
		luaL_loadfile(L, "NPC.lua");
		lua_pcall(L, 0, 0, 0);

		lua_register(L, "API_SendMessage", API_SendMessage);
		lua_register(L, "API_get_x", API_get_x);
		lua_register(L, "API_get_y", API_get_y);

		clients[npc_id] = npc;
	}
	std::cout << "NPC Initialization ended.\n";
}

void timer_thread()
{
	using namespace std::chrono;
	while (true) {
		event_type ev;
		if (timer_queue.try_pop(ev)) {
			auto now = system_clock::now();
			if (ev.wakeup_time <= now) {
				switch (ev.event_id) {
				case EVENT_MOVE:
					EXP_OVER* move_over = new EXP_OVER;
					move_over->m_iotype = IO_NPCMOVE; // 이동 이벤트는 OP_SEND로 처리
					PostQueuedCompletionStatus(g_iocp, -1, ev.obj_id, &move_over->m_over); // 이동 이벤트를 워커 스레드로 전달
					break;
				}
				continue;
			}
			else {
				// 아직 시간이 안 됐으면 다시 큐에 넣음
				timer_queue.push(ev);
			}
		}
		std::this_thread::sleep_for(milliseconds(1));
	}
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
			std::cout << "Client connected." << std::endl;
			if (g_num_players >= MAX_PLAYERS) {
				std::cout << "No more player can be accepted." << std::endl;
				send_login_fail(exp_over->m_client_socket, "Server is full.");
				closesocket(exp_over->m_client_socket);
			}
			else {
				g_num_players++;
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
				std::cout << "Session not found for client[" << player_index << "].\n";
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
			if (data_size > 0) memmove(cl->m_recv_over.m_buff, p, data_size);
			cl->m_prev_recv = data_size;
			cl->do_recv();
		}
		break;
		case IO_SEND: {
			// cout << "Message sent. to client[" << key << "]\n";
			EXP_OVER* o = reinterpret_cast<EXP_OVER*>(over);
			delete o;
		}
					break;
		case IO_NPCMOVE: {
			using namespace std::chrono;

			EXP_OVER* o = reinterpret_cast<EXP_OVER*>(over);
			delete o;

			int npc_id = static_cast<int>(key);
			std::shared_ptr<SESSION> npc = clients[npc_id];
			if (nullptr != npc) {
				npc->do_random_move();

				// 다음 이동 이벤트 재등록
				event_type ev;
				ev.event_id = EVENT_MOVE;
				ev.obj_id = npc_id;
				ev.target_id = -1;
				ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME);
				timer_queue.push(ev);
			}
		}
		break;
		case IO_PLAYER_MOVE:
		{
			EXP_OVER* o = reinterpret_cast<EXP_OVER*>(over);
			int player_id = o->m_target_id;
			int npc_id = static_cast<int>(key);
			std::shared_ptr<SESSION> npc = clients[npc_id];
			if (nullptr != npc) {
				auto L = npc->L;
				npc->m_lua_mutex.lock();
				lua_getglobal(L, "event_player_move");
				lua_pushinteger(L, player_id);
				int error = lua_pcall(L, 1, 0, 0);
				if (error != LUA_OK) {
					const char* error_msg = lua_tostring(L, -1);
					std::cerr << "Lua error: " << error_msg << std::endl;
					lua_pop(L, 1); // remove error message from stack
				}
				npc->m_lua_mutex.unlock();
			}
			delete o;
		}
		break;
		default:
			std::cout << "Unknown IO type." << std::endl;
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

	std::vector <std::thread> worker_threads;
	int num_threads = std::thread::hardware_concurrency();

	initialize_npcs();

	auto npc_ai = std::thread(timer_thread);
	for (int i = 0; i < num_threads; ++i)
		worker_threads.emplace_back(worker_thread);
	for (auto& th : worker_threads)
		th.join();
	npc_ai.join();
	closesocket(g_server);
	WSACleanup();
}
