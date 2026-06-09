#pragma once
#include "common.h"
#include "Zone.h"
#include "protocol.h"
#include <set>

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };


struct ExtendedRioBuf {
	RIO_BUF rioBuf;
	EVENT_TYPE event_type;

	char* buf_addr;
	unsigned int idx;
};


struct SOCKETINFO
{
	RIO_RQ rq;
	ExtendedRioBuf* recvBuf;
	//char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	int		recv_buf_start_idx;
	int		origin_offset;
	SOCKET	socket;
	int		idx;
	int		gid;
	char	name[MAX_STR_LEN];

	bool is_connected;
	bool is_active;
	short	x, y;
	unsigned	move_time;
	std::set <int> near_id;

	int my_woker_id;
	ZoneNode zone_nodes[MAX_ZONE_NODE];
	std::set<int> broadcast_zone;
	int my_zone_col;
	int my_zone_row;

	int rem_send_cnt{ SEND_RQ_SIZE };
};

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

	std::cout << "zone node empty!!" << std::endl;
	//exit(1);
	return -1;
}