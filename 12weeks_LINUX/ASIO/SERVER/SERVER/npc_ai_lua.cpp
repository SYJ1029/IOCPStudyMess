#include <sdkddkver.h>
#include <iostream>
#include <array>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <memory>
#include <concurrent_unordered_map.h>
#include <concurrent_priority_queue.h>
#include <boost/unordered/concurrent_flat_map.hpp>
#include "protocol.h"

#include "include/lua.hpp"
#include <boost/asio.hpp>

#pragma comment(lib, "lua54.lib")
using namespace std;

constexpr int VIEW_RANGE = 5;
constexpr int NET_BUFF_SIZE = 8192;

enum EVENT_TYPE { EV_RANDOM_MOVE };

struct TIMER_EVENT {
	int obj_id;
	chrono::system_clock::time_point wakeup_time;
	EVENT_TYPE event_id;
	int target_id;
	constexpr bool operator < (const TIMER_EVENT& L) const
	{
		return (wakeup_time > L.wakeup_time);
	}
};
concurrency::concurrent_priority_queue<TIMER_EVENT> timer_queue;

bool is_pc(int object_id)
{
	return object_id < MAX_USER;
}

bool is_npc(int object_id)
{
	return !is_pc(object_id);
}

void WakeUpNPC(int npc_id, int waker);

enum COMP_TYPE { OP_ACCEPT, OP_RECV, OP_SEND, OP_NPC_MOVE, OP_AI_HELLO };
enum S_STATE { ST_FREE, ST_ALLOC, ST_INGAME };
class SESSION : public std::enable_shared_from_this<SESSION> 
{

public:
	mutex _s_lock;
	S_STATE _state;
	atomic_bool	_is_active;		// 주위에 플레이어가 있는가?
	int _id;
	boost::asio::ip::tcp::socket _socket;
	char	_recv_buff[NET_BUFF_SIZE];
	int		_prev_remain;
	short	x, y;
	char	_name[NAME_SIZE];
	unordered_set <int> _view_list;
	mutex	_vl;
	int		last_move_time;
	lua_State*	_L;
	mutex	_ll;
public:
	SESSION(boost::asio::ip::tcp::socket socket, int id) : _id(id), _socket(std::move(socket)), x(0), y(0), 
		_state(ST_FREE), _prev_remain(0)
	{
	}

	~SESSION() {}

	void disconnect();

	void process_packet(char* packet);

	void process_data(int num_bytes)
	{
		int remain_data = num_bytes + _prev_remain;
		char* p = _recv_buff;
		while (remain_data > 0) {
			int packet_size = p[0];
			if (packet_size <= remain_data) {
				process_packet(p);
				p = p + packet_size;
				remain_data = remain_data - packet_size;
			}
			else break;
		}
		_prev_remain = remain_data;
		if (remain_data > 0)
			memcpy(_recv_buff, p, remain_data);
	}
	void do_recv()
	{
		_socket.async_read_some(boost::asio::buffer(_recv_buff + _prev_remain, sizeof(_recv_buff) - _prev_remain),
			[this](boost::system::error_code ec, size_t bytes_read) {
				if (ec) {
					if (ec.value() == boost::asio::error::operation_aborted) return;
					cout << "Receive Error on Session[" << _id << "] EC[" << ec.what() << "]\n";
					disconnect();
					return;
				}
				process_data(static_cast<int>(bytes_read));
				do_recv();
			});
	}


	void do_send(void* packet)
	{
		size_t p_size = *reinterpret_cast<unsigned char*>(packet);
		_socket.async_write_some(boost::asio::buffer(packet, p_size),
			[](boost::system::error_code ec, std::size_t length){});
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

	void send_move_packet(const shared_ptr<SESSION>& mover);
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

	bool can_see(shared_ptr<SESSION>& target)
	{
		if (abs(x - target->x) > VIEW_RANGE) return false;
		return abs(y - target->y) <= VIEW_RANGE;
	}

	void do_npc_random_move();
};

boost::concurrent_flat_map<int, shared_ptr<SESSION>> objects;

void SESSION::disconnect()
{
	_vl.lock();
	unordered_set <int> vl = _view_list;
	_vl.unlock();
	for (auto& p_id : vl) {
		if (is_npc(p_id)) continue;
		int my_id = _id;
		objects.visit(p_id, [my_id](auto &apl) {
			shared_ptr<SESSION> pl = apl.second;
			{
				lock_guard<mutex> ll(pl->_s_lock);
				if (ST_INGAME != pl->_state) return;
			}
			if (pl->_id != my_id)
				pl->send_remove_player_packet(my_id);
			
			});

	}

	lock_guard<mutex> ll(_s_lock);
	_state = ST_FREE;
	objects.visit(_id, [](auto &apl) {
		apl.second = nullptr;
		});
}

void SESSION::process_packet(char* packet)
{
	switch (packet[1]) {
	case CS_LOGIN: {
		CS_LOGIN_PACKET* p = reinterpret_cast<CS_LOGIN_PACKET*>(packet);
		strcpy_s(_name, p->name);
		{
			lock_guard<mutex> ll{ _s_lock };
			x = rand() % W_WIDTH;
			y = rand() % W_HEIGHT;
			_state = ST_INGAME;
		}
		send_login_info_packet();
		objects.visit_all([this](auto &item) {
			shared_ptr<SESSION> ppl = item.second;
			{
				lock_guard<mutex> ll(ppl->_s_lock);
				if (ST_INGAME != ppl->_state) return;
			}
			if (ppl->_id == _id) return;
			if (false == can_see(ppl)) return;
			if (is_pc(ppl->_id)) ppl->send_add_player_packet(_id);
			else WakeUpNPC(ppl->_id, _id);
			send_add_player_packet(ppl->_id);
			});
		break;
	}
	case CS_MOVE: {
		CS_MOVE_PACKET* p = reinterpret_cast<CS_MOVE_PACKET*>(packet);
		last_move_time = p->move_time;
		switch (p->direction) {
		case 0: if (y > 0) y--; break;
		case 1: if (y < W_HEIGHT - 1) y++; break;
		case 2: if (x > 0) x--; break;
		case 3: if (x < W_WIDTH - 1) x++; break;
		}

		unordered_set<int> near_list;
		_vl.lock();
		unordered_set<int> old_vlist = _view_list;
		_vl.unlock();
		objects.visit_all([&near_list, this](auto& cl) {
			shared_ptr<SESSION> ccl = cl.second;
			if (ccl->_state != ST_INGAME) return;
			if (ccl->_id == _id) return;
			if (can_see(ccl))
				near_list.insert(ccl->_id);
			});

		send_move_packet(_id);

		for (auto& pl : near_list) {
			objects.visit(pl, [pl, this](auto& item) {
				shared_ptr<SESSION> cpl = item.second;
				if (is_pc(pl)) {
					cpl->_vl.lock();
					if (cpl->_view_list.count(_id)) {
						cpl->_vl.unlock();
						cpl->send_move_packet(_id);
					}
					else {
						cpl->_vl.unlock();
						cpl->send_add_player_packet(_id);
					}
				}
				else WakeUpNPC(pl, _id);
				});

			if (old_vlist.count(pl) == 0)
				send_add_player_packet(pl);
		}

		for (auto& pl : old_vlist)
			if (0 == near_list.count(pl)) {
				send_remove_player_packet(pl);
				if (is_pc(pl)) {
					objects.visit(pl, [this](auto &item) {
						shared_ptr<SESSION> ppl = item.second;
						ppl->send_remove_player_packet(_id);
						});
				}
			}
	}
				break;
	}
}

// NPC 구현 세번째 방법  (실제로 많이 사용되는 방법)
//   클래스 상속기능을 사용한다.
//     SESSION은 NPC클래스를 상속받아서 네트워크 관련 기능을 추가한 형태로 정의한다.
//       clients컨테이너를 objects컨테이너로 변경하고, 컨테이너는 NPC의 pointer를 저장한다.
//      장점 : 메모리 낭비가 없다, 함수의 중복작성이 필요없다.
//          (포인터로 관리되므로 player id의 중복사용 방지를 구현하기 쉬워진다 => Data Race 방지를 위한 추가 구현이 필요)
//      단점 : 포인터가 사용되고, reinterpret_cast가 필요하다. (별로 단점이 안니다).

void SESSION::send_move_packet(const shared_ptr<SESSION> &mover)
{
	SC_MOVE_OBJECT_PACKET p;
	p.id = mover->_id;
	p.size = sizeof(SC_MOVE_OBJECT_PACKET);
	p.type = SC_MOVE_OBJECT;
	p.x = mover->x;
	p.y = mover->y;
	p.move_time = mover->last_move_time;
	do_send(&p);
}

void SESSION::send_move_packet(int c_id)
{
	objects.visit(c_id, [this, c_id](auto &item) {
		shared_ptr<SESSION> mover = item.second;
		SC_MOVE_OBJECT_PACKET p;
		p.id = c_id;
		p.size = sizeof(SC_MOVE_OBJECT_PACKET);
		p.type = SC_MOVE_OBJECT;
		p.x = mover->x;
		p.y = mover->y;
		p.move_time = mover->last_move_time;
		do_send(&p);
		});
}

void SESSION::send_add_player_packet(int c_id)
{
	SC_ADD_OBJECT_PACKET add_packet;
	objects.visit(c_id, [&add_packet, c_id](auto &item) {
		shared_ptr<SESSION> ppl = item.second;
		SC_ADD_OBJECT_PACKET add_packet;
		add_packet.id = c_id;
		strcpy_s(add_packet.name, ppl->_name);
		add_packet.size = sizeof(add_packet);
		add_packet.type = SC_ADD_OBJECT;
		add_packet.x = ppl->x;
		add_packet.y = ppl->y;
		});
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
	static atomic_int id = 0;
	return id++;
}

void WakeUpNPC(int npc_id, int waker)
{
	boost::asio::post([npc_id, waker]() {
		objects.visit(npc_id, [waker](auto &item) {
			shared_ptr<SESSION> cl = item.second;
			cl->_ll.lock();
			auto L = cl->_L;
			lua_getglobal(L, "event_player_move");
			lua_pushnumber(L, waker);
			lua_pcall(L, 1, 0, 0);
			lua_pop(L, 1);
			cl->_ll.unlock();
			});
		});

	objects.visit(npc_id, [](auto &item) {
		shared_ptr<SESSION> cl = item.second;
		if (cl->_is_active) return;
		bool old_state = false;
		if (false == atomic_compare_exchange_strong(&cl->_is_active, &old_state, true))
			return;
		});
	TIMER_EVENT ev{ npc_id, chrono::system_clock::now(), EV_RANDOM_MOVE, 0 };
	timer_queue.push(ev);
}

void SESSION::do_npc_random_move()
{
	unordered_set<int> old_vl;

	objects.visit_all([&old_vl, this](auto &item) {
		shared_ptr<SESSION> pl = item.second;
		if (ST_INGAME == pl->_state)
			if (true == is_pc(pl->_id))
				if (true == can_see(pl))
					old_vl.insert(pl->_id);
		});

	switch (rand() % 4) {
	case 0: if (x < (W_WIDTH - 1)) x++; break;
	case 1: if (x > 0) x--; break;
	case 2: if (y < (W_HEIGHT - 1)) y++; break;
	case 3:if (y > 0) y--; break;
	}

	unordered_set<int> new_vl;
	objects.visit_all([&new_vl, this](auto &item) {
		shared_ptr<SESSION> pl = item.second;
		if (ST_INGAME == pl->_state)
			if (true == is_pc(pl->_id))
				if (true == can_see(pl))
					new_vl.insert(pl->_id);
		});

	for (auto pl : new_vl) {
		objects.visit(pl, [&old_vl, pl, this](auto &item) {
			shared_ptr<SESSION> ppl = item.second;
			if (0 == old_vl.count(pl)) {
				// 플레이어의 시야에 등장
				ppl->send_add_player_packet(_id);
			}
			else {
				// 플레이어가 계속 보고 있음.
				ppl->send_move_packet(_id);
			}
			});
		///vvcxxccxvvdsvdvds
		for (auto pl : old_vl) {
			if (0 == new_vl.count(pl)) {
				objects.visit(pl, [this](auto &item) {
					shared_ptr<SESSION> ppl = item.second;
					ppl->_vl.lock();
					if (0 != ppl->_view_list.count(_id)) {
						ppl->_vl.unlock();
						ppl->send_remove_player_packet(_id);
					}
					else {
						ppl->_vl.unlock();
					}
					});
			}
		}
	}
}

void worker_thread(boost::asio::io_context *io_context)
{
	io_context->run();
}

int API_get_x(lua_State* L)
{
	int user_id =(int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int x = 0;
	objects.visit(user_id, [&x](auto &item) {
		shared_ptr<SESSION> cl = item.second;
		x = cl->x;
		});
	lua_pushnumber(L, x);
	return 1;
}

int API_get_y(lua_State* L)
{
	int user_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int y = 0;
	objects.visit(user_id, [&y](auto &item) {
		shared_ptr<SESSION> cl = item.second;
		y = cl->y;
		});
	lua_pushnumber(L, y);
	return 1;
}

int API_SendMessage(lua_State* L)
{
	int my_id = (int)lua_tointeger(L, -3);
	int user_id = (int)lua_tointeger(L, -2);
	char* mess = (char*)lua_tostring(L, -1);

	lua_pop(L, 4);

	objects.visit(user_id, [my_id, mess](auto &item) {
		shared_ptr<SESSION> cl = item.second;
		cl->send_chat_packet(my_id, mess);
		});
	return 0;
}

void InitializeNPC()
{
	cout << "NPC intialize begin.\n";
	for (int i = MAX_USER; i < MAX_USER + MAX_NPC; ++i) {
		objects.visit(i, [i](auto &item) {
			shared_ptr<SESSION> npc = item.second;
			npc->x = rand() % W_WIDTH;
			npc->y = rand() % W_HEIGHT;
			npc->_id = i;
			sprintf_s(npc->_name, "NPC%d", i);
			npc->_state = ST_INGAME;

			auto L = npc->_L = luaL_newstate();
			luaL_openlibs(L);
			luaL_loadfile(L, "npc.lua");
			lua_pcall(L, 0, 0, 0);

			lua_getglobal(L, "set_uid");
			lua_pushnumber(L, i);
			lua_pcall(L, 1, 0, 0);
			// lua_pop(L, 1);// eliminate set_uid from stack after call

			lua_register(L, "API_SendMessage", API_SendMessage);
			lua_register(L, "API_get_x", API_get_x);
			lua_register(L, "API_get_y", API_get_y);
			});
	}
	cout << "NPC initialize end.\n";
}

void do_timer()
{
	while (true) {
		TIMER_EVENT ev;
		auto current_time = chrono::system_clock::now();
		if (true == timer_queue.try_pop(ev)) {
			if (ev.wakeup_time > current_time) {
				timer_queue.push(ev);		// 최적화 필요
				// timer_queue에 다시 넣지 않고 처리해야 한다.
				this_thread::sleep_for(1ms);  // 실행시간이 아직 안되었으므로 잠시 대기
				continue;
			}
			switch (ev.event_id) {
			case EV_RANDOM_MOVE:
				int obj_id = ev.event_id;
				boost::asio::post([obj_id]() {
					objects.visit(obj_id, [obj_id](auto &item) {
						shared_ptr<SESSION> npc = item.second;
						bool keep_alive = false;
						for (int j = 0; j < MAX_USER; ++j) {
							objects.visit(j, [&keep_alive, &npc](auto &item) {
								shared_ptr<SESSION> pl = item.second;
								if (pl->_state == ST_INGAME)
									if (pl->can_see(npc))
										keep_alive = true;
								});
							if (true == keep_alive) break;
						}
						if (true == keep_alive) {
							npc->do_npc_random_move();
							TIMER_EVENT ev{ obj_id, chrono::system_clock::now() + 1s, EV_RANDOM_MOVE, 0 };
							timer_queue.push(ev);
						}
						else {
							npc->_is_active = false;
						}
						});
					});
				break;
			}
			continue;		// 즉시 다음 작업 꺼내기
		}
		this_thread::sleep_for(1ms);   // timer_queue가 비어 있으니 잠시 기다렸다가 다시 시작
	}
}

class GAME_SERVER
{
public:
	GAME_SERVER(boost::asio::io_context& io_context, const boost::asio::ip::tcp::endpoint& endpoint)
		: acceptor_(io_context, endpoint)
	{
		do_accept();
	}

private:
	void do_accept()
	{
		acceptor_.async_accept(
			[this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket)
			{
				if (!ec)
				{
					int new_id = get_new_client_id();
					auto new_session = std::make_shared<SESSION>(std::move(socket), new_id);

					new_session->_state = ST_ALLOC;
					new_session->x = 0;
					new_session->y = 0;
					new_session->_id = new_id;
					new_session->_name[0] = 0;
					new_session->_prev_remain = 0;
					objects.try_emplace(new_id, new_session);

					new_session->do_recv();
				}

				do_accept();
			});
	}

	boost::asio::ip::tcp::acceptor acceptor_;
};

int main()
{

	InitializeNPC();

	boost::asio::io_context io_context;
	boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), PORT_NUM);

	GAME_SERVER s{ io_context, endpoint };

	vector <thread> worker_threads;
	int num_threads = std::thread::hardware_concurrency();
	for (int i = 0; i < num_threads; ++i)
		worker_threads.emplace_back(worker_thread, &io_context);
	thread timer_thread{ do_timer };
	timer_thread.join();
	for (auto& th : worker_threads)
		th.join();
}
