#pragma once
#include <vector>
#include <iostream>
#include <mutex>
#include "Client.h"


std::mutex moreSendBuffer;

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
	std::mutex recvBufLock;

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
		std::lock_guard<std::mutex> lg(recvBufLock);
		if (emptyRecvBufIdx.empty())
			return nullptr;
		unsigned int idx = emptyRecvBufIdx.back();
		emptyRecvBufIdx.pop_back();
		return &recvRioBuffer[idx];
	}
	void return_recvBuffer(ExtendedRioBuf* rioBuf) {
		std::lock_guard<std::mutex> lg(recvBufLock);
		emptyRecvBufIdx.push_back(rioBuf->idx);
	}


};


class LocalSendBufferPool {
	char* sendBuffer;
	RIO_BUFFERID sendBufferId;

	std::vector<ExtendedRioBuf*> sendRioBuffers;
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
		mem_exceed_cnt.fetch_add(1);
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
			std::cout << "no more send buf" << std::endl;
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