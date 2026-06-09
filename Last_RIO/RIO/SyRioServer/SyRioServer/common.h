#pragma once
#include <iostream>
#include <WinSock2.h>
#include <mswsock.h>
#include <atomic>

#define MAX_CLIENT			16384
#define MAX_BUFFER			128
#define NUM_WORKER_THREADS	8
#define LOCAL_SEND_BUF_CNT	int((MAX_CLIENT * 128))
#define MAX_ZONE_NODE		16

#define RECV_RQ_SIZE		50
#define SEND_RQ_SIZE		400
#define MAX_RIO_REUSLTS		int( MAX_CLIENT / NUM_WORKER_THREADS)
#define ZONE_SIZE			20
constexpr auto VIEW_RANGE = 7;

thread_local int tid;
std::atomic_uint mem_exceed_cnt{ 0 };
//thread_local int per_mem_exceed_cnt{ 0 };
RIO_EXTENSION_FUNCTION_TABLE gRIOFuncTable;

#pragma comment(lib, "Ws2_32.lib")
