#pragma once
#include <atomic>
#include <algorithm>
#include <memory>
#include "MessageQueue.h"


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
		return std::atomic_compare_exchange_strong(
			reinterpret_cast<std::atomic_llong*>(&next),
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

std::atomic_ullong reservations[NUM_WORKER_THREADS];
std::atomic_ullong epoch = 1;
const unsigned int epoch_freq = 1;

void start_op() {
	reservations[tid].store(epoch.load(std::memory_order_acquire), std::memory_order_release);
}

void end_op() {
	reservations[tid].store(0xffffffffffffffff, std::memory_order_release);
}

void retire(ZoneNode* ptr) {
	ptr->retired_epoch = epoch.load(std::memory_order_acquire);
	ptr->used = false;

	//counter++;
	//if (counter % epoch_freq == 0)
	epoch.fetch_add(1, std::memory_order_release);
}

unsigned long long get_min_reservation() {
	unsigned long long min_re = 0xffffffffffffffff;
	for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
		min_re = min(min_re, reservations[i].load(std::memory_order_acquire));
	}
	return min_re;
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