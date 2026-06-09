#include <WinSock2.h>
#include <mswsock.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <set>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <iterator>
#pragma comment(lib, "Ws2_32.lib")
#include "protocol.h"

using namespace std;
using namespace chrono;


#define MAX_CLIENT			16384
#define NUM_WORKER_THREADS	8
#define LOCAL_SEND_BUF_CNT	int((MAX_CLIENT * 128))
#define RECV_RQ_SIZE		50
#define SEND_RQ_SIZE		200
#define MAX_RIO_REUSLTS		int( MAX_CLIENT / NUM_WORKER_THREADS)
#define ZONE_SIZE			20
constexpr auto VIEW_RANGE = 7;

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };
RIO_EXTENSION_FUNCTION_TABLE gRIOFuncTable;
thread_local int tid;

// max_client 만 받도록 처리필요
//atomic_int num_client{ 0 };

struct ExtendedRioBuf {
	RIO_BUF rioBuf;
	EVENT_TYPE event_type;

	char* buf_addr;
	unsigned int idx;
};

#define MAX_ZONE_NODE		4
#define MAX_BUFFER			128

class ZoneNode {
public:
	int worker_id;
	int cid;
	unsigned long long next;
	unsigned long long retired_epoch{ 0 };
	bool used{ false };

	ZoneNode() {
		next = 0;
	}
	ZoneNode(int wid, int cid) : worker_id(wid), cid(cid) {
		next = 0;
	}
	~ZoneNode() {}

	ZoneNode* GetNext() {
		return reinterpret_cast<ZoneNode*>(next & 0xFFFFFFFFFFFFFFFE);
	}

	void SetNext(ZoneNode* ptr) {
		next = reinterpret_cast<unsigned long long>(ptr);
	}

	ZoneNode* GetNextWithMark(bool* mark) {
		long long temp = next;
		*mark = (temp % 2) == 1;
		return reinterpret_cast<ZoneNode*>(temp & 0xFFFFFFFFFFFFFFFE);
	}

	bool CAS(long long old_value, long long new_value)
	{
		return atomic_compare_exchange_strong(
			reinterpret_cast<atomic_llong*>(&next),
			&old_value, new_value);
	}

	bool CAS(ZoneNode* old_next, ZoneNode* new_next, bool old_mark, bool new_mark) {
		unsigned long long old_value = reinterpret_cast<unsigned long long>(old_next);
		if (old_mark) old_value = old_value | 0x1;
		else old_value = old_value & 0xFFFFFFFFFFFFFFFE;
		unsigned long long new_value = reinterpret_cast<unsigned long long>(new_next);
		if (new_mark) new_value = new_value | 0x1;
		else new_value = new_value & 0xFFFFFFFFFFFFFFFE;
		return CAS(old_value, new_value);
	}

	bool TryMark(ZoneNode* ptr)
	{
		unsigned long long old_value = reinterpret_cast<unsigned long long>(ptr) & 0xFFFFFFFFFFFFFFFE;
		unsigned long long new_value = old_value | 1;
		return CAS(old_value, new_value);
	}

	bool IsMarked() {
		return (0 != (next & 1));
	}
};


struct SOCKETINFO
{
	RIO_RQ rq;
	ExtendedRioBuf* recvBuf;
	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	SOCKET	socket;
	int		id;
	int		gid;
	char	name[MAX_STR_LEN];

	bool is_connected;
	bool is_active;
	short	x, y;
	int		seq_no;
	set <int> near_id;

	int my_woker_id;
	ZoneNode zone_nodes[MAX_ZONE_NODE];
	set<int> broadcast_zone;
	int my_zone_col;
	int my_zone_row;
};

enum class Msg {
	HI, BYE, MOVE, NEW_CLI
};

#define INIT_MSG_SIZE MAX_CLIENT*128
#define INIT_MSG_CAPACITY int(INIT_MSG_SIZE*(1.5))


class MsgNode {
public:
	int from_wid;
	int from_cid;
	Msg msg;
	int x;
	int y;
	int to;
	int gid;
	SOCKETINFO* info;


	MsgNode* next;

	MsgNode() { next = nullptr; }
	MsgNode(int wid, int cid, Msg msg, int x, int y, int to) :
		from_wid(wid), from_cid(cid), msg(msg), x(x), y(y), to(to) {
		next = nullptr;
	}
	~MsgNode() {}
};

class MsgNodeBuffer {
	std::vector<MsgNode*> emptyNodes;
public:
	MsgNodeBuffer() {
		emptyNodes.reserve(INIT_MSG_CAPACITY);
		for (int i = 0; i < INIT_MSG_SIZE; ++i) {
			emptyNodes.emplace_back(new MsgNode);
		}
	}

	MsgNode* get() {
		if (emptyNodes.empty()) {
			cout << "Msg node empty" << endl;
			return (new MsgNode);
		}

		MsgNode* ret = emptyNodes.back();
		emptyNodes.pop_back();
		return ret;
	}
	void retire(MsgNode* msg) {
		emptyNodes.push_back(msg);
	}
};


thread_local MsgNodeBuffer msgNodeBuffer;

// MPSC
class MessageQueue {
	MsgNode* volatile head;
	MsgNode* volatile tail;
public:

	MessageQueue()
	{
		head = tail = new MsgNode();
	}
	~MessageQueue() {}

	void Init()
	{
		MsgNode* ptr;
		while (head->next != nullptr) {
			ptr = head->next;
			head->next = head->next->next;
			delete ptr;
		}
		tail = head;
	}

	bool CAS(MsgNode* volatile* addr, MsgNode* old_node, MsgNode* new_node)
	{
		return atomic_compare_exchange_strong(reinterpret_cast<volatile atomic_llong*>(addr),
			reinterpret_cast<long long*>(&old_node),
			reinterpret_cast<long long>(new_node));
	}

	void Enq(int wid, int cid, Msg msg, int x, int y, int to, int gid
		, SOCKETINFO* info = nullptr)
	{
		MsgNode* e = msgNodeBuffer.get();
		e->from_wid = wid;
		e->from_cid = cid;
		e->msg = msg;
		e->x = x; e->y = y;
		e->to = to;
		e->gid = gid;
		e->info = info;
		

		while (true) {
			MsgNode* last = tail;
			MsgNode* next = last->next;
			if (last != tail) continue;
			if (next != nullptr) {
				CAS(&tail, last, next);
				continue;
			}
			if (false == CAS(&last->next, nullptr, e)) continue;
			CAS(&tail, last, e);
			return;
		}
	}

	// 리턴 받은 thread가 retire 해야 한다
	MsgNode* Deq()
	{
		while (true) {
			MsgNode* first = head;
			MsgNode* next = first->next;
			MsgNode* last = tail;
			MsgNode* lastnext = last->next;

			if (last == first) {
				if (lastnext == nullptr) {
					return nullptr;
				}
				else
				{
					CAS(&tail, last, lastnext);
					continue;
				}
			}
			if (nullptr == next) continue;

			head = next;
			//if (false == CAS(&head, first, next)) continue;
			first->next = nullptr;

			msgNodeBuffer.retire(first);
			return head;
			//delete first;
		}
	}
};






MessageQueue msgQueue[NUM_WORKER_THREADS];

atomic_ullong reservations[NUM_WORKER_THREADS];
atomic_ullong epoch = 1;
thread_local int counter = 0;
const unsigned int epoch_freq = 10;

unsigned long long get_min_reservation() {
	unsigned long long min_re = 0xffffffffffffffff;
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
		min_re = min(min_re, reservations[i].load(memory_order_acquire));
	}
	return min_re;
}

int get_empty_zone_nodeIdx(SOCKETINFO* cli) {
	unsigned long long max_safe_epoch = get_min_reservation();

	// queue에 오래된것부터 쌓인다
	for (int i = 0; i < MAX_ZONE_NODE; ++i) {
		if (cli->zone_nodes[i].used == false
			&& cli->zone_nodes[i].retired_epoch < max_safe_epoch) {
			cli->zone_nodes[i].used = true;
			return i;
		}
	}

	cout << "zone node empty!!" << endl;
	exit(1);
	return -1;
}

void retire(ZoneNode* ptr) {
	ptr->retired_epoch = epoch.load(memory_order_acquire);
	ptr->used = false;

	counter++;
	if (counter % epoch_freq == 0)
		epoch.fetch_add(1, memory_order_release);
}


void start_op() {
	reservations[tid].store(epoch.load(memory_order_acquire), memory_order_release);
}

void end_op() {
	reservations[tid].store(0xffffffffffffffff, memory_order_release);
}

class Zone {
	ZoneNode head, tail;
public:
	Zone()
	{
		head.worker_id = -1;
		head.cid = -1;
		tail.worker_id = -1;
		tail.cid = -1;
		head.SetNext(&tail);
	}
	void Init()
	{
		while (head.GetNext() != &tail) {
			ZoneNode* temp = head.GetNext();
			head.next = temp->next;
		}
	}

	void Add(ZoneNode* e)
	{
		start_op();
		while (true) {
			ZoneNode* first = &head;
			ZoneNode* next = first->GetNext();
			e->SetNext(next);
			if (false == first->CAS(next, e, false, false))
				continue;
			end_op();
			return;
		}

	}

	void Find(int wid, int cid, ZoneNode** pred, ZoneNode** curr)
	{
	retry:
		ZoneNode* pr = &head;
		ZoneNode* cu = pr->GetNext();
		while (true) {
			bool removed;
			ZoneNode* su = cu->GetNextWithMark(&removed);
			if (true == removed) {
				goto retry;
			}
			
			if (cu->worker_id == wid && cu->cid == cid) {
				*pred = pr; *curr = cu;
				return;
			}
			pr = cu;
			cu = cu->GetNext();
		}
	}

	
	void Remove(int wid, int cid)
	{
		start_op();
		ZoneNode* pred, * curr;
		while (true) {
			Find(wid, cid, &pred, &curr);
			ZoneNode* succ = curr->GetNext();
			// 나밖에 안 호출
			curr->TryMark(succ);
			if (true == pred->CAS(curr, succ, false, false)) {
				retire(curr);
				end_op();
				return;
			}
		}
	}

	void Broadcast(int wid, int cid, Msg msg, int x, int y, int gid) {
		start_op();
		ZoneNode* curr = head.GetNext();
		while (curr != &tail) {
			if (false == curr->IsMarked() && !(curr->worker_id == wid && curr->cid == cid)) {
				msgQueue[curr->worker_id].Enq(wid, cid, msg, x, y, curr->cid, gid);
			}
			curr = curr->GetNext();
		}
		end_op();
	}

};
const unsigned int zone_width = int(WORLD_WIDTH / ZONE_SIZE);
const unsigned int zone_heigt = int(WORLD_HEIGHT / ZONE_SIZE);
Zone zone[zone_heigt][zone_width];


void echo(const char* buf, ExtendedRioBuf* sendBuf, ULONG bytes) {
	char* dest = sendBuf->buf_addr;

	for (int i = 0; i < bytes; ++i) {
		dest[i] = buf[i];
	}
	dest[bytes] = 0;
	sendBuf->rioBuf.Length = bytes;
}

class GlobalRecvBufferPool {
	std::vector<unsigned int> emptyRecvBufIdx;
	char* recvBuffer;
	RIO_BUFFERID recvBufferId;
	ExtendedRioBuf* recvRioBuffer;
	mutex recvBufLock;

public:
	GlobalRecvBufferPool() {

	}
	void init() {
		//2. Buffer 할당 및 등록
		// recv buffer
		recvBuffer = reinterpret_cast<char*>(VirtualAllocEx(GetCurrentProcess(),
			0, MAX_BUFFER * MAX_CLIENT,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (recvBuffer == 0) {
			std::cout << "allocate recv buf error" << std::endl;
			exit(1);
		}
		recvBufferId = gRIOFuncTable.RIORegisterBuffer(recvBuffer, MAX_BUFFER * MAX_CLIENT);
		if (recvBufferId == RIO_INVALID_BUFFERID) {
			std::cout << "recv rio buf id error" << std::endl;
			exit(1);
		}

		recvRioBuffer = new ExtendedRioBuf[MAX_CLIENT];
		for (int i = 0; i < MAX_CLIENT; ++i) {
			recvRioBuffer[i].event_type = EV_RECV;
			recvRioBuffer[i].idx = i;
			recvRioBuffer[i].rioBuf.BufferId = recvBufferId;
			recvRioBuffer[i].rioBuf.Offset = i * MAX_BUFFER;
			recvRioBuffer[i].rioBuf.Length = MAX_BUFFER;
			recvRioBuffer[i].buf_addr = recvBuffer + (i * MAX_BUFFER);
		}
		for (int i = MAX_CLIENT - 1; i >= 0; --i) {
			emptyRecvBufIdx.push_back(i);
		}
	}

	ExtendedRioBuf* get_recvBuffer() {
		lock_guard<mutex> lg(recvBufLock);
		if (emptyRecvBufIdx.empty())
			return nullptr;
		unsigned int idx = emptyRecvBufIdx.back();
		emptyRecvBufIdx.pop_back();
		return &recvRioBuffer[idx];
	}
	void return_recvBuffer(ExtendedRioBuf* rioBuf) {
		lock_guard<mutex> lg(recvBufLock);
		emptyRecvBufIdx.push_back(rioBuf->idx);
	}

	
} globalBufferPool;

mutex moreSendBuffer;

class LocalSendBufferPool {
	char* sendBuffer;
	RIO_BUFFERID sendBufferId;

	vector<ExtendedRioBuf*> sendRioBuffers;
public:
	LocalSendBufferPool() {
	}
	void init() {
		// send buffer
		sendBuffer = reinterpret_cast<char*>(VirtualAllocEx(GetCurrentProcess(),
			0, MAX_BUFFER * LOCAL_SEND_BUF_CNT,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (sendBuffer == 0) {
			std::cout << "allocate send buf error" << std::endl;
			exit(1);
		}
		sendBufferId = gRIOFuncTable.RIORegisterBuffer(sendBuffer, MAX_BUFFER * LOCAL_SEND_BUF_CNT);
		if (sendBufferId == RIO_INVALID_BUFFERID) {
			std::cout << "recv rio buf id error" << std::endl;
			exit(1);
		}

		sendRioBuffers.reserve(int(LOCAL_SEND_BUF_CNT * 1.5));
		for (int i = 0; i < LOCAL_SEND_BUF_CNT; ++i) {
			ExtendedRioBuf* sendBuf = new ExtendedRioBuf;
			sendBuf->event_type = EV_SEND;
			sendBuf->rioBuf.BufferId = sendBufferId;
			sendBuf->rioBuf.Offset = i * MAX_BUFFER;
			sendBuf->rioBuf.Length = MAX_BUFFER;
			sendBuf->buf_addr = sendBuffer + (i * MAX_BUFFER);

			sendRioBuffers.push_back(sendBuf);
		}
	}

	void get_more_send_buffer() {
		char* nsendBuffer;
		RIO_BUFFERID nsendBufferId;

		nsendBuffer = reinterpret_cast<char*>(VirtualAllocEx(GetCurrentProcess(),
			0, MAX_BUFFER * LOCAL_SEND_BUF_CNT,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (nsendBuffer == 0) {
			std::cout << "allocate send buf error" << std::endl;
			exit(1);
		}
		moreSendBuffer.lock();
		nsendBufferId = gRIOFuncTable.RIORegisterBuffer(nsendBuffer, MAX_BUFFER * LOCAL_SEND_BUF_CNT);
		if (nsendBufferId == RIO_INVALID_BUFFERID) {
			std::cout << "recv rio buf id error" << std::endl;
			exit(1);
		}
		moreSendBuffer.unlock();

		for (int i = 0; i < LOCAL_SEND_BUF_CNT; ++i) {
			ExtendedRioBuf* sendBuf = new ExtendedRioBuf;
			sendBuf->event_type = EV_SEND;
			sendBuf->rioBuf.BufferId = nsendBufferId;
			sendBuf->rioBuf.Offset = i * MAX_BUFFER;
			sendBuf->rioBuf.Length = MAX_BUFFER;
			sendBuf->buf_addr = nsendBuffer + (i * MAX_BUFFER);

			sendRioBuffers.push_back(sendBuf);
		}

	}

	ExtendedRioBuf* get_sendBuffer() {
		if (sendRioBuffers.empty()) {
			cout << "no more send buf" << endl;
			get_more_send_buffer();
		}

		ExtendedRioBuf* ret = sendRioBuffers.back();
		sendRioBuffers.pop_back();
		return ret;
	}
	void return_sendBuffer(ExtendedRioBuf* rioBuf) {
		rioBuf->rioBuf.Length = MAX_BUFFER;
		sendRioBuffers.push_back(rioBuf);
	}

};

LocalSendBufferPool sendBufferPool[NUM_WORKER_THREADS];
RIO_CQ compQueue[NUM_WORKER_THREADS];

thread_local int new_user_id = 0;
thread_local unordered_map<int, SOCKETINFO*> my_clients;

void Disconnect(int id);

void error_display(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	cout << msg;
	wcout << L"에러 " << lpMsgBuf << endl;
	while (true);
	LocalFree(lpMsgBuf);
}


bool is_near(int x1, int y1, int x2, int y2)
{
	if (VIEW_RANGE < abs(x1 - x2)) return false;
	if (VIEW_RANGE < abs(y1 - y2)) return false;
	return true;
}

void send_packet(RIO_RQ& rq, void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	int packet_size = packet[0];

	ExtendedRioBuf* sendBuf = sendBufferPool[tid].get_sendBuffer();
	echo(packet, sendBuf, packet_size);

	if (false == gRIOFuncTable.RIOSend(rq, &(sendBuf->rioBuf), 1, NULL, (PVOID)sendBuf)) {
		std::cout << "RIO send Error" << std::endl;
		exit(1);
	}
}

void send_login_ok_packet(RIO_RQ& rq, int gid, int x, int y)
{
	sc_packet_login_ok packet;
	packet.id = gid;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_OK;
	packet.x = x;
	packet.y = y;
	packet.hp = 100;
	packet.level = 1;
	packet.exp = 1;
	send_packet(rq, &packet);
}


void send_login_fail(RIO_RQ& rq, int gid)
{
	sc_packet_login_fail packet;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_FAIL;
	send_packet(rq, &packet);
}

void send_put_object_packet(RIO_RQ& rq, int new_gid, int nx, int ny)
{
	sc_packet_put_object packet;
	packet.id = new_gid;
	packet.size = sizeof(packet);
	packet.type = SC_PUT_OBJECT;
	packet.x = nx;
	packet.y = ny;
	packet.o_type = 1;
	send_packet(rq, &packet);
}

void send_pos_packet(RIO_RQ& rq, int mover, int mx, int my, int my_seq)
{
	sc_packet_pos packet;
	packet.id = mover;
	packet.size = sizeof(packet);
	packet.type = SC_POS;
	packet.x = mx;
	packet.y = my;
	packet.seq_no = my_seq;


	send_packet(rq, &packet);
}


void send_remove_object_packet(RIO_RQ& rq, int leaver)
{
	sc_packet_remove_object packet;
	packet.id = leaver;
	packet.size = sizeof(packet);
	packet.type = SC_REMOVE_OBJECT;
	send_packet(rq, &packet);
}

void send_chat_packet(RIO_RQ& rq, int teller, char* mess)
{
	sc_packet_chat packet;
	packet.id = teller;
	packet.size = sizeof(packet);
	packet.type = SC_CHAT;
	send_packet(rq, &packet);
}



void Disconnect(int id)
{
	//std::cout << "client [" << id << "] closed" << std::endl;
	//num_client.fetch_add(-1, memory_order_release);
	
	//1. zone 에서 제거
	zone[my_clients[id]->my_zone_row][my_clients[id]->my_zone_col].Remove(tid, id);

	//2. broadcast
	for (auto i : my_clients[id]->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::BYE, -1, -1, my_clients[id]->gid);
	}


	my_clients[id]->is_connected = false;
	closesocket(my_clients[id]->socket);
	globalBufferPool.return_recvBuffer(my_clients[id]->recvBuf);

	my_clients.erase(id);
}

void get_new_zone(set<int>& new_zone, int x, int y) {
	int x1 = int((x - VIEW_RANGE) / ZONE_SIZE);
	if (x1 < 0) x1 = 0;
	int x2 = int((x + VIEW_RANGE) / ZONE_SIZE);
	if (x2 >= zone_width) x2 = zone_width - 1;
	int y1 = int((y - VIEW_RANGE) / ZONE_SIZE);
	if (y1 < 0) y1 = 0;
	int y2 = int((y + VIEW_RANGE) / ZONE_SIZE);
	if (y2 >= zone_heigt) y2 = zone_heigt - 1;

	new_zone.insert(x1 * zone_width + y1);
	new_zone.insert(x1 * zone_width + y2);
	new_zone.insert(x2 * zone_width + y1);
	new_zone.insert(x2 * zone_width + y2);
}


void ProcessMove(int id, unsigned char dir)
{
	short x = my_clients[id]->x;
	short y = my_clients[id]->y;
	
	switch (dir) {
	case D_UP: if (y > 0) y--;
		break;
	case D_DOWN: if (y < WORLD_HEIGHT - 1) y++;
		break;
	case D_LEFT: if (x > 0) x--;
		break;
	case D_RIGHT: if (x < WORLD_WIDTH - 1) x++;
		break;
	case 99:
		x = rand() % WORLD_WIDTH;
		y = rand() % WORLD_HEIGHT;
		break;
	default: cout << "Invalid Direction Error\n";
		while (true);
	}

	my_clients[id]->x = x;
	my_clients[id]->y = y;

	send_pos_packet(my_clients[id]->rq, my_clients[id]->gid, x, y, my_clients[id]->seq_no);

	// 1. zone in/out
	int zc = int(x / ZONE_SIZE);
	int zr = int(y / ZONE_SIZE);
	if (zc != my_clients[id]->my_zone_col || zr != my_clients[id]->my_zone_row) {
		int idx = get_empty_zone_nodeIdx(my_clients[id]);
		zone[zr][zc].Add(&(my_clients[id]->zone_nodes[idx]));
		zone[my_clients[id]->my_zone_row][my_clients[id]->my_zone_col].Remove(tid, id);
		my_clients[id]->my_zone_col = zc;
		my_clients[id]->my_zone_row = zr;
	}

	// 2. broadcast
	for (auto i : my_clients[id]->broadcast_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, my_clients[id]->gid);
	}
	set<int> new_zone;
	get_new_zone(new_zone, x, y);
	
	// 새로 들어 온 zone
	for (auto i : new_zone) {
		if (my_clients[id]->broadcast_zone.count(i) == 0) {
			zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, id, Msg::MOVE, x, y, my_clients[id]->gid);
		}
	}
	my_clients[id]->broadcast_zone.swap(new_zone);

}

void ProcessLogin(int user_id, char* id_str)
{
	strcpy_s(my_clients[user_id]->name, id_str);
	my_clients[user_id]->is_connected = true;
	send_login_ok_packet(my_clients[user_id]->rq, my_clients[user_id]->gid
		, my_clients[user_id]->x, my_clients[user_id]->y);

	// 1. zone in
	int zc = int(my_clients[user_id]->x / ZONE_SIZE);
	int zr = int(my_clients[user_id]->y / ZONE_SIZE);
	int idx = get_empty_zone_nodeIdx(my_clients[user_id]);
	zone[zr][zc].Add(&(my_clients[user_id]->zone_nodes[idx]));
	my_clients[user_id]->my_zone_col = zc;
	my_clients[user_id]->my_zone_row = zr;
	

	// 2. broadcast
	set<int> new_zone;
	get_new_zone(new_zone, my_clients[user_id]->x, my_clients[user_id]->y);

	// 새로 들어 온 zone
	for (auto i : new_zone) {
		zone[i % ZONE_SIZE][i / ZONE_SIZE].Broadcast(tid, user_id, Msg::MOVE
			, my_clients[user_id]->x, my_clients[user_id]->y, my_clients[user_id]->gid);
	}
	my_clients[user_id]->broadcast_zone.swap(new_zone);
}


void ProcessPacket(int id, void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	switch (packet[1]) {
	case CS_LOGIN: {
		cs_packet_login* login_packet = reinterpret_cast<cs_packet_login*>(packet);
		ProcessLogin(id, login_packet->id);
	}
				 break;
	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		my_clients[id]->seq_no = move_packet->seq_no;
		ProcessMove(id, move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT: {
		cs_packet_chat* chat_packet = reinterpret_cast<cs_packet_chat*>(packet);
		//ProcessChat(id, chat_packet->chat_str);
	}
				break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		ProcessMove(id, 99);
		break;
	default: cout << "Invalid Packet Type Error\n";
		while (true);
	}
}

void handle_recv(unsigned int uid, ExtendedRioBuf* riobuf, ULONG bytes) {
	char* p = riobuf->buf_addr;
	int remain = bytes;
	int packet_size;
	int prev_packet_size = my_clients[uid]->prev_packet_size;
	if (0 == prev_packet_size)
		packet_size = 0;
	else packet_size = my_clients[uid]->pre_net_buf[0];
	while (remain > 0) {
		if (0 == packet_size) packet_size = p[0];
		int required = packet_size - prev_packet_size;
		if (required <= remain) {
			memcpy(my_clients[uid]->pre_net_buf + prev_packet_size, p, required);
			ProcessPacket(uid, my_clients[uid]->pre_net_buf);
			remain -= required;
			p += required;
			prev_packet_size = 0;
			packet_size = 0;
		}
		else {
			memcpy(my_clients[uid]->pre_net_buf + prev_packet_size, p, remain);
			prev_packet_size += remain;
			remain = 0;
		}
	}
	my_clients[uid]->prev_packet_size = prev_packet_size;


	if (false == gRIOFuncTable.RIOReceive(my_clients[uid]->rq, &(my_clients[uid]->recvBuf->rioBuf), 1,
		NULL, (PVOID)my_clients[uid]->recvBuf)) {
		std::cout << "RIO recv Error" << std::endl;
		exit(1);
	}
}

void handle_move_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;
	
	// 시야 검사
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. 처음 보는 애
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) == 0)) {
		// list에 넣고 나한테 send_put_packet
		my_clients[my_id]->near_id.insert(msg->gid);
		send_put_object_packet(my_clients[my_id]->rq, msg->gid, msg->x, msg->y);
		// 상대한테 HI 보내주기
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::HI, mx, my, msg->from_cid, my_clients[my_id]->gid);
		return;
	}
	
	// 2. 알던 애
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) != 0)) {
		// 그냥 나한테 send_pos_packet
		send_pos_packet(my_clients[my_id]->rq, msg->gid, msg->x, msg->y
			, my_clients[my_id]->seq_no);
		return;
	}

	// 3. 헤어지는 애
	if (!in_view && (my_clients[my_id]->near_id.count(msg->gid) != 0)) {
		// list에서 빼고 send_remove_packet
		my_clients[my_id]->near_id.erase(msg->gid);
		send_remove_object_packet(my_clients[my_id]->rq, msg->gid);
		// 상대한테도 BYE 보내주기
		msgQueue[msg->from_wid].Enq(tid, my_id, Msg::BYE, -1, -1, msg->from_cid, my_clients[my_id]->gid);
		return;
	}
}

void handle_hi_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;
	// 시야 검사
	bool in_view = is_near(mx, my, msg->x, msg->y);
	// 1. 처음 보는 애
	if (in_view && (my_clients[my_id]->near_id.count(msg->gid) == 0)) {
		// list에 넣고 나한테 send_put_packet
		my_clients[my_id]->near_id.insert(msg->gid);
		send_put_object_packet(my_clients[my_id]->rq, msg->gid, msg->x, msg->y);
	}
}

void handle_bye_msg(MsgNode* msg) {
	int my_id = msg->to;
	int mx = my_clients[my_id]->x;
	int my = my_clients[my_id]->y;

	if(my_clients[my_id]->near_id.count(msg->gid) != 0) {
		my_clients[my_id]->near_id.erase(msg->gid);
		send_remove_object_packet(my_clients[my_id]->rq, msg->gid);
	}
}


void handle_new_client(MsgNode* msg) {
	int user_id = new_user_id++;
	SOCKETINFO* new_player = msg->info;
	new_player->id = user_id;
	new_player->my_woker_id = tid;

	for (int i = 0; i < MAX_ZONE_NODE; ++i) {
		new_player->zone_nodes[i].worker_id = tid;
		new_player->zone_nodes[i].cid = user_id;
	}

	// 5. RequestQueue 작성	
	new_player->rq = gRIOFuncTable.RIOCreateRequestQueue(new_player->socket,
		RECV_RQ_SIZE, 1, SEND_RQ_SIZE, 1,
		compQueue[tid], compQueue[tid], (PVOID)user_id);;

	my_clients.insert(make_pair(user_id, new_player));

	// 6. RIO recv
	if (false == gRIOFuncTable.RIOReceive(new_player->rq
		, &(new_player->recvBuf->rioBuf), 1,
		NULL, (PVOID)(new_player->recvBuf))) {
		std::cout << "RIO recv Error" << std::endl;
		exit(1);
	}



}

void do_worker(int t)
{
	tid = t;
	while (true) {
		int checked_queue = 0;
		while (checked_queue++ < MAX_RIO_REUSLTS)
		{
			MsgNode* msg = msgQueue[tid].Deq();
			if (msg == nullptr) break;
			
			if (msg->msg == Msg::NEW_CLI) {
				handle_new_client(msg);
				//msgNodeBuffer.retire(msg);
				break;
			}

			if (my_clients.count(msg->to) == 0)
				break;

			switch (msg->msg)
			{
			case Msg::MOVE: 
				handle_move_msg(msg);
				break;
			case Msg::HI:
				handle_hi_msg(msg);
				break;
			case Msg::BYE:
				handle_bye_msg(msg);
				break;
			default:
				cout << "UNKOWN MSG" << endl;
				break;
			}

			//msgNodeBuffer.retire(msg);
		}

		RIORESULT rioResults[MAX_RIO_REUSLTS];

		ULONG numResults;
		//7. RIODequeueComletion
		numResults = gRIOFuncTable.RIODequeueCompletion(compQueue[tid], rioResults, MAX_RIO_REUSLTS);

		if (RIO_CORRUPT_CQ == numResults) {
			std::cout << "Rio DQ error" << std::endl;
			exit(1);
		}


		for (ULONG i = 0; i < numResults; ++i) {
			ExtendedRioBuf* rioBuf = reinterpret_cast<ExtendedRioBuf*>(rioResults[i].RequestContext);
			unsigned int uid = (unsigned int)rioResults[i].SocketContext;
			ULONG bytes = rioResults[i].BytesTransferred;


			switch (rioBuf->event_type)
			{
			case EV_RECV:
				if (bytes == 0) {
					Disconnect(uid);
					continue;
				}
				handle_recv(uid, rioBuf, bytes);
				break;
			case EV_SEND:
				sendBufferPool[tid].return_sendBuffer(rioBuf);
				break;
			default:
				std::cout << "unknown Event error" << std::endl;
				break;
			}
		}
	}
}




int main()
{
	epoch.store(1);
	for (int r = 0; r < NUM_WORKER_THREADS; ++r)
		reservations[r] = 0xffffffffffffffff;

	wcout.imbue(std::locale("korean"));

	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	if (SOCKET_ERROR == ::bind(listenSocket, (struct sockaddr*) & serverAddr, sizeof(SOCKADDR_IN))) {
		error_display("WSARecv Error :", WSAGetLastError());
	}
	listen(listenSocket, 5);
	SOCKADDR_IN clientAddr;
	int addrLen = sizeof(SOCKADDR_IN);
	memset(&clientAddr, 0, addrLen);
	DWORD flags;

	vector <thread> worker_threads;
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) worker_threads.emplace_back(do_worker, i);

	// 1. RIO함수 table 설정
	GUID funcTableID = WSAID_MULTIPLE_RIO;
	DWORD dwBytes = 0;
	WSAIoctl(listenSocket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
		&funcTableID, sizeof(GUID),
		(void**)&gRIOFuncTable, sizeof(gRIOFuncTable),
		&dwBytes, NULL, NULL);

	// 2. Buffer 할당 및 등록
	globalBufferPool.init();
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
		sendBufferPool[i].init();
	}


	// 3. CompletionQueue 작성

	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {

		compQueue[i] = gRIOFuncTable.RIOCreateCompletionQueue(int((RECV_RQ_SIZE + SEND_RQ_SIZE) * (MAX_CLIENT / NUM_WORKER_THREADS)), 0);
		if (compQueue[i] == RIO_INVALID_CQ) {
			std::cout << "create CQ error" << std::endl;
			exit(1);
		}
	}


	int cq_idx = 0;
	int global_id = 0;
	//4. Accept
	while (true) {
		//if (num_client.load(memory_order_acquire) >= 10)
		//	continue;
		SOCKET clientSocket = accept(listenSocket, (struct sockaddr*) & clientAddr, &addrLen);
		if (INVALID_SOCKET == clientSocket) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("Accept Error :", err_no);
		}
		
		//num_client.fetch_add(1, memory_order_release);
		
		cq_idx = ++cq_idx % NUM_WORKER_THREADS;
		SOCKETINFO* new_player = new SOCKETINFO;
		ExtendedRioBuf* recvBuf = globalBufferPool.get_recvBuffer();
		new_player->recvBuf = recvBuf;
		new_player->x = rand() % WORLD_WIDTH;
		new_player->y = rand() % WORLD_HEIGHT;

		new_player->socket = clientSocket;
		new_player->prev_packet_size = 0;
		new_player->is_connected = false;
		new_player->gid = global_id++;

		msgQueue[cq_idx].Enq(-1, -1, Msg::NEW_CLI, -1, -1, -1, -1, new_player);
	}

	for (auto& th : worker_threads) th.join();
	closesocket(listenSocket);
	WSACleanup();
}