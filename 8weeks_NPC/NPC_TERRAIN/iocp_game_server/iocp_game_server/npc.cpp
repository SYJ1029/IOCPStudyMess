#include <iostream>
#include <array>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <concurrent_priority_queue.h>
#include "protocol.h"

#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")
using namespace std;
using namespace std::chrono;

constexpr int VIEW_RANGE = 5;
constexpr int MOVE_COOL_TIME = 1000; // ms

constexpr int EVENT_MOVE = 1;

struct event_type {
	int obj_id;
	system_clock::time_point wakeup_time;
	int event_id;
	int target_id;

	constexpr bool operator < (const event_type& _Left) const
	{
		return (wakeup_time > _Left.wakeup_time);
	}
};
concurrency::concurrent_priority_queue<event_type> timer_queue;


enum COMP_TYPE { OP_ACCEPT, OP_RECV, OP_SEND, OP_NPCMOVE };
class OVER_EXP {
public:
	WSAOVERLAPPED _over;
	WSABUF _wsabuf;
	char _send_buf[BUF_SIZE];
	COMP_TYPE _comp_type;
	int _ai_target_obj;
	OVER_EXP()
	{
		_wsabuf.len = BUF_SIZE;
		_wsabuf.buf = _send_buf;
		_comp_type = OP_RECV;
		ZeroMemory(&_over, sizeof(_over));
	}
	OVER_EXP(char* packet)
	{
		_wsabuf.len = packet[0];
		_wsabuf.buf = _send_buf;
		ZeroMemory(&_over, sizeof(_over));
		_comp_type = OP_SEND;
		memcpy(_send_buf, packet, packet[0]);
	}
};

enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };
class SESSION {
	OVER_EXP _recv_over;

public:
	mutex _s_lock;
	S_STATE _state;
	int _id;
	SOCKET _socket;
	short	x, y;
	char	_name[NAME_SIZE];
	int		_prev_remain;
	unordered_set <int> _view_list;
	mutex	_vl;
	int last_move_time;
	std::atomic<bool> _active_npc;
	system_clock::time_point npc_last_move_time;

public:
	SESSION()
	{
		_id = -1;
		_socket = 0;
		x = y = 0;
		_name[0] = 0;
		_state = ST_FREE;
		_prev_remain = 0;
	}

	~SESSION() {}

	void do_recv()
	{
		DWORD recv_flag = 0;
		memset(&_recv_over._over, 0, sizeof(_recv_over._over));
		_recv_over._wsabuf.len = BUF_SIZE - _prev_remain;
		_recv_over._wsabuf.buf = _recv_over._send_buf + _prev_remain;
		WSARecv(_socket, &_recv_over._wsabuf, 1, 0, &recv_flag,
			&_recv_over._over, 0);
	}

	void do_send(void* packet)
	{
		OVER_EXP* sdata = new OVER_EXP{ reinterpret_cast<char*>(packet) };
		WSASend(_socket, &sdata->_wsabuf, 1, 0, 0, &sdata->_over, 0);
	}
	void send_login_info_packet()
	{
		SC_LOGIN_INFO_PACKET p;
		p.id = _id;
		p.size = sizeof(SC_LOGIN_INFO_PACKET);
		p.type = SC_LOGIN_INFO;
		p.x = x;
		p.y = y;
		do_send(&p);
	}
	void send_move_packet(int c_id);
	void send_add_player_packet(int c_id);
	void send_chat_packet(int c_id, const char* mess);
	void send_remove_player_packet(int c_id)
	{
		_vl.lock();
		if (_view_list.count(c_id))
			_view_list.erase(c_id);
		else {
			_vl.unlock();
			return;
		}
		_vl.unlock();
		SC_REMOVE_OBJECT_PACKET p;
		p.id = c_id;
		p.size = sizeof(p);
		p.type = SC_REMOVE_OBJECT;
		do_send(&p);
	}
	void do_random_move();
	void heart_beat()
	{
		// NPC의 경우, 일정 시간마다 랜덤한 방향으로 이동하는 기능을 구현한다.
		// 이동한 후에는, 이동한 위치를 주변 플레이어들에게 알려준다.
		do_random_move();
	}

	void wake_up()
	{
		// _active_npc가 false일 때만 true로 바꾸고 타이머 등록 (중복 등록 방지)
		bool expected = false;
		if (false == _active_npc.compare_exchange_strong(expected, true))
			return;

		event_type ev;
		ev.obj_id = _id;
		ev.event_id = EVENT_MOVE;
		ev.target_id = -1;
		ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME);
		timer_queue.push(ev);
	}
};

HANDLE h_iocp;
array<SESSION, MAX_USER + MAX_NPC> clients;

// NPC 구현 첫번째 방법
//  NPC클래스를 별도 제작, NPC컨테이너를 따로 생성한다.
//  장점 : 깔끔하다, 군더더기가 없다.
//  단점 : 플레이어와 NPC가 따로논다. 똑같은 역할을 수행하는 함수를 여러개씩 중복 작성해야 한다.
//         예) bool can_see(int from, int to)
//                 => bool can_see_p2p()
//				    bool can_see_p2n()
//					bool can_see_n2n()

// NPC 구현 두번째 방법  <===== 실습에서 사용할 방법.
//   clients 컨테이너에 NPC도 추가한다.
//   장점 : 플레이어와 NPC를 동일하게 취급할 수 있어서, 프로그래밍 작성 부하가 줄어든다.
//   단점 : 사용하지 않는 멤버들로 인한 메모리 낭비.

// NPC 구현 세번째 방법  (실제로 많이 사용되는 방법)
//   클래스 상속기능을 사용한다.
//     SESSION은 NPC클래스를 상속받아서 네트워크 관련 기능을 추가한 형태로 정의한다.
//       clients컨테이너를 objects컨테이너로 변경하고, 컨테이너는 NPC의 pointer를 저장한다.
//      장점 : 메모리 낭비가 없다, 함수의 중복작성이 필요없다.
//          (포인터로 관리되므로 player id의 중복사용 방지를 구현하기 쉬워진다 => Data Race 방지를 위한 추가 구현이 필요)
//      단점 : 포인터가 사용되고, reinterpret_cast가 필요하다. (별로 단점이 안니다).

SOCKET g_s_socket, g_c_socket;
OVER_EXP g_a_over;

bool is_pc(int object_id)
{
	return object_id < MAX_USER;
}

bool is_npc(int object_id)
{
	return !is_pc(object_id);
}

bool can_see(int from, int to)
{
	if (abs(clients[from].x - clients[to].x) > VIEW_RANGE) return false;
	return abs(clients[from].y - clients[to].y) <= VIEW_RANGE;
}

void SESSION::do_random_move()
{
	unordered_set<int> old_vl;
	for (auto& obj : clients) {
		if (ST_INGAME != obj._state) continue;
		if (true == is_npc(obj._id)) continue;
		if (true == can_see(_id, obj._id))
			old_vl.insert(obj._id);
	}


	switch (rand() % 4) {
	case 0: if (x < (W_WIDTH - 1)) x++; break;
	case 1: if (x > 0) x--; break;
	case 2: if (y < (W_HEIGHT - 1)) y++; break;
	case 3:if (y > 0) y--; break;
	}

	unordered_set<int> new_vl;
	for (auto& obj : clients) {
		if (ST_INGAME != obj._state) continue;
		if (true == is_npc(obj._id)) continue;
		if (true == can_see(_id, obj._id))
			new_vl.insert(obj._id);
	}

	for (auto pl : new_vl) {
		if (0 == old_vl.count(pl)) {
			// 플레이어의 시야에 등장
			clients[pl].send_add_player_packet(_id);
		}
		else {
			// 플레이어가 계속 보고 있음.
			clients[pl].send_move_packet(_id);
		}
	}
	///vvcxxccxvvdsvdvds
	for (auto pl : old_vl)
		if (0 == new_vl.count(pl))
				clients[pl].send_remove_player_packet(_id);

	if (_id == MAX_USER) {
		auto delay = system_clock::now() - npc_last_move_time;
		std::cout << "NPC " << _id << " moved. Time since last move: " << duration_cast<milliseconds>(delay).count() << "ms\n";
	}

	npc_last_move_time = system_clock::now();
}

void SESSION::send_move_packet(int c_id)
{
	SC_MOVE_OBJECT_PACKET p;
	p.id = c_id;
	p.size = sizeof(SC_MOVE_OBJECT_PACKET);
	p.type = SC_MOVE_OBJECT;
	p.x = clients[c_id].x;
	p.y = clients[c_id].y;
	p.move_time = clients[c_id].last_move_time;
	do_send(&p);
}

void SESSION::send_add_player_packet(int c_id)
{
	SC_ADD_OBJECT_PACKET add_packet;
	add_packet.id = c_id;
	strcpy_s(add_packet.name, clients[c_id]._name);
	add_packet.size = sizeof(add_packet);
	add_packet.type = SC_ADD_OBJECT;
	add_packet.x = clients[c_id].x;
	add_packet.y = clients[c_id].y;
	_vl.lock();
	_view_list.insert(c_id);
	_vl.unlock();
	do_send(&add_packet);
}

void SESSION::send_chat_packet(int p_id, const char* mess)
{
	SC_CHAT_PACKET packet;
	packet.id = p_id;
	packet.size = sizeof(packet);
	packet.type = SC_CHAT;
	strcpy_s(packet.mess, mess);
	do_send(&packet);
}

int get_new_client_id()
{
	for (int i = 0; i < MAX_USER; ++i) {
		lock_guard <mutex> ll{ clients[i]._s_lock };
		if (clients[i]._state == ST_FREE)
			return i;
	}
	return -1;
}

void process_packet(int c_id, char* packet)
{
	switch (packet[1]) {
	case CS_LOGIN: {
		CS_LOGIN_PACKET* p = reinterpret_cast<CS_LOGIN_PACKET*>(packet);
		strcpy_s(clients[c_id]._name, p->name);
		{
			lock_guard<mutex> ll{ clients[c_id]._s_lock };
			clients[c_id].x = rand() % W_WIDTH;
			clients[c_id].y = rand() % W_HEIGHT;
			clients[c_id]._state = ST_INGAME;
		}
		clients[c_id].send_login_info_packet();
		for (auto& pl : clients) {
			{
				lock_guard<mutex> ll(pl._s_lock);
				if (ST_INGAME != pl._state) continue;
			}
			if (pl._id == c_id) continue;
			if (false == can_see(c_id, pl._id))
				continue;
			if (is_pc(pl._id)) pl.send_add_player_packet(c_id);
			clients[c_id].send_add_player_packet(pl._id);

			// 시야에 들어온 NPC를 깨움
			if (is_npc(pl._id))
				pl.wake_up();
		}
		break;
	}
	case CS_MOVE: {
		CS_MOVE_PACKET* p = reinterpret_cast<CS_MOVE_PACKET*>(packet);
		clients[c_id].last_move_time = p->move_time;
		short x = clients[c_id].x;
		short y = clients[c_id].y;
		switch (p->direction) {
		case 0: if (y > 0) y--; break;
		case 1: if (y < W_HEIGHT - 1) y++; break;
		case 2: if (x > 0) x--; break;
		case 3: if (x < W_WIDTH - 1) x++; break;
		}
		clients[c_id].x = x;
		clients[c_id].y = y;

		unordered_set<int> near_list;
		clients[c_id]._vl.lock();
		unordered_set<int> old_vlist = clients[c_id]._view_list;
		clients[c_id]._vl.unlock();
		for (auto& cl : clients) {
			if (cl._state != ST_INGAME) continue;
			if (cl._id == c_id) continue;
			if (can_see(c_id, cl._id))
				near_list.insert(cl._id);
		}

		clients[c_id].send_move_packet(c_id);

		for (auto& pl : near_list) {
			auto& cpl = clients[pl];
			if (is_pc(pl)) {
				cpl._vl.lock();
				if (clients[pl]._view_list.count(c_id)) {
					cpl._vl.unlock();
					clients[pl].send_move_packet(c_id);
				}
				else {
					cpl._vl.unlock();
					clients[pl].send_add_player_packet(c_id);
				}
			}

			if (old_vlist.count(pl) == 0) {
				clients[c_id].send_add_player_packet(pl);
				// 새로 시야에 들어온 NPC를 깨움
				if (is_npc(pl))
					clients[pl].wake_up();
			}
		}

		for (auto& pl : old_vlist)
			if (0 == near_list.count(pl)) {
				clients[c_id].send_remove_player_packet(pl);
				if (is_pc(pl))
					clients[pl].send_remove_player_packet(c_id);
			}
		break;
	}
	}
}

void disconnect(int c_id)
{
	clients[c_id]._vl.lock();
	unordered_set <int> vl = clients[c_id]._view_list;
	clients[c_id]._vl.unlock();
	for (auto& p_id : vl) {
		if (is_npc(p_id)) continue;
		auto& pl = clients[p_id];
		{
			lock_guard<mutex> ll(pl._s_lock);
			if (ST_INGAME != pl._state) continue;
		}
		if (pl._id == c_id) continue;
		pl.send_remove_player_packet(c_id);
	}
	closesocket(clients[c_id]._socket);

	lock_guard<mutex> ll(clients[c_id]._s_lock);
	clients[c_id]._state = ST_FREE;
}

void do_npc_random_move(int npc_id)
{
	// NPC의 랜덤 이동을 처리하는 함수
	// npc_id에 해당하는 NPC를 랜덤한 방향으로 이동시키고, 주변 플레이어들에게 이동 정보를 알려준다.
	clients[npc_id].do_random_move();
}

void worker_thread(HANDLE h_iocp)
{
	while (true) {
		DWORD num_bytes;
		ULONG_PTR key;
		WSAOVERLAPPED* over = nullptr;
		BOOL ret = GetQueuedCompletionStatus(h_iocp, &num_bytes, &key, &over, INFINITE);
		OVER_EXP* ex_over = reinterpret_cast<OVER_EXP*>(over);
		if (FALSE == ret) {
			if (ex_over->_comp_type == OP_ACCEPT) cout << "Accept Error";
			else {
				cout << "GQCS Error on client[" << key << "]\n";
				disconnect(static_cast<int>(key));
				if (ex_over->_comp_type == OP_SEND) delete ex_over;
				continue;
			}
		}

		if ((0 == num_bytes) && ((ex_over->_comp_type == OP_RECV) || (ex_over->_comp_type == OP_SEND))) {
			disconnect(static_cast<int>(key));
			if (ex_over->_comp_type == OP_SEND) delete ex_over;
			continue;
		}

		switch (ex_over->_comp_type) {
		case OP_ACCEPT: {
			int client_id = get_new_client_id();
			if (client_id != -1) {
				{
					lock_guard<mutex> ll(clients[client_id]._s_lock);
					clients[client_id]._state = ST_ALLOC;
				}
				clients[client_id].x = 0;
				clients[client_id].y = 0;
				clients[client_id]._id = client_id;
				clients[client_id]._name[0] = 0;
				clients[client_id]._prev_remain = 0;
				clients[client_id]._socket = g_c_socket;
				CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_c_socket),
					h_iocp, client_id, 0);
				clients[client_id].do_recv();
				g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			}
			else {
				cout << "Max user exceeded.\n";
			}
			ZeroMemory(&g_a_over._over, sizeof(g_a_over._over));
			int addr_size = sizeof(SOCKADDR_IN);
			AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, 0, &g_a_over._over);
			break;
		}
		case OP_RECV: {
			int remain_data = num_bytes + clients[key]._prev_remain;
			char* p = ex_over->_send_buf;
			while (remain_data > 0) {
				int packet_size = p[0];
				if (packet_size <= remain_data) {
					process_packet(static_cast<int>(key), p);
					p = p + packet_size;
					remain_data = remain_data - packet_size;
				}
				else break;
			}
			clients[key]._prev_remain = remain_data;
			if (remain_data > 0) {
				memcpy(ex_over->_send_buf, p, remain_data);
			}
			clients[key].do_recv();
			break;
		}
		case OP_SEND:
			delete ex_over;
			break;
		case OP_NPCMOVE: {
			delete ex_over;
			int npc_id = static_cast<int>(key);
			do_npc_random_move(npc_id);

			// 시야 내에 플레이어가 있는지 확인
			bool has_nearby_player = false;
			for (int i = 0; i < MAX_USER; ++i) {
				if (clients[i]._state == ST_INGAME && can_see(npc_id, i)) {
					has_nearby_player = true;
					break;
				}
			}

			if (has_nearby_player) {
				// 다음 이동 이벤트 재등록
				event_type ev;
				ev.event_id = EVENT_MOVE;
				ev.obj_id = npc_id;
				ev.target_id = -1;
				ev.wakeup_time = system_clock::now() + milliseconds(MOVE_COOL_TIME);
				timer_queue.push(ev);
			}
			else {
				// 시야 내 플레이어 없음 → AI 비활성화 (다음 wake_up() 호출까지 대기)
				clients[npc_id]._active_npc = false;
			}
			break;
		}
		}
	}
}
void InitializeNPC()
{
	cout << "NPC intialize begin.\n";
	for (int i = MAX_USER; i < MAX_USER + MAX_NPC; ++i) {
		clients[i].x = rand() % W_WIDTH;
		clients[i].y = rand() % W_HEIGHT;
		clients[i]._id = i;
		sprintf_s(clients[i]._name, "NPC%d", i);
		clients[i]._state = ST_INGAME;
		clients[i].last_move_time = 0;
		clients[i].npc_last_move_time = system_clock::now();
	}
	cout << "NPC initialize end.\n";
}

void HB_thread ()
{
	using namespace chrono;

	while (true) {
		auto start_time = chrono::system_clock::now();
		for (int i = MAX_USER; i < MAX_USER + MAX_NPC; ++i) {
			if (clients[i]._state != ST_INGAME) continue;
			clients[i].heart_beat();
		}
		auto end_time = chrono::system_clock::now();
		auto elapsed = end_time - start_time;
		if (elapsed < chrono::milliseconds(MOVE_COOL_TIME)) {
			this_thread::sleep_for(chrono::milliseconds(MOVE_COOL_TIME) - elapsed);
		}

		std::cout << "Elapsed Time : "
			<< duration_cast<milliseconds>(elapsed).count()
			<< "ms.\n";
	}
}

void ai_thread()
{
	while (true) {
		int elapsed_time = 1000;
		auto current_time = system_clock::now();
		for (int i = MAX_USER; i < MAX_USER + MAX_NPC; ++i) {
			if (clients[i]._state != ST_INGAME) continue;
			auto duration = duration_cast<milliseconds>(current_time - clients[i].npc_last_move_time).count();
			if (duration >= MOVE_COOL_TIME) {
				do_npc_random_move(i);
				clients[i].npc_last_move_time = current_time;

				if (duration > elapsed_time) elapsed_time++;
				else if (duration < elapsed_time) elapsed_time--;

			}
		}
		std::cout << "Elapsed Time : " << elapsed_time << "ms.\n";
	}
}

void timer_thread()
{
	while (true) {
		event_type ev;
		if (timer_queue.try_pop(ev)) {
			auto now = system_clock::now();
			if (ev.wakeup_time <= now) {
				switch (ev.event_id) {
				case EVENT_MOVE:
					OVER_EXP* move_over = new OVER_EXP;
					move_over->_comp_type = OP_NPCMOVE; // 이동 이벤트는 OP_SEND로 처리
					PostQueuedCompletionStatus(h_iocp, -1, ev.obj_id, &move_over->_over); // 이동 이벤트를 워커 스레드로 전달
					break;
				}
			}
			else {
				// 아직 시간이 안 됐으면 다시 큐에 넣음
				timer_queue.push(ev);
				this_thread::sleep_for(chrono::milliseconds(1));
			}
		}
		else {
			this_thread::sleep_for(chrono::milliseconds(1));
		}
	}
}


int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	g_s_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT_NUM);
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	bind(g_s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(g_s_socket, SOMAXCONN);
	SOCKADDR_IN cl_addr;
	int addr_size = sizeof(cl_addr);

	InitializeNPC();

	h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(g_s_socket), h_iocp, 9999, 0);
	g_c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	g_a_over._comp_type = OP_ACCEPT;
	AcceptEx(g_s_socket, g_c_socket, g_a_over._send_buf, 0, addr_size + 16, addr_size + 16, 0, &g_a_over._over);

	vector<thread> worker_threads;
	thread timer_th(timer_thread);   // HB_thread 대신 timer_thread 사용
	int num_threads = std::thread::hardware_concurrency();
	for (int i = 0; i < num_threads; ++i)
		worker_threads.emplace_back(worker_thread, h_iocp);
	for (auto& th : worker_threads)
		th.join();
	timer_th.join();
	closesocket(g_s_socket);
	WSACleanup();
}
