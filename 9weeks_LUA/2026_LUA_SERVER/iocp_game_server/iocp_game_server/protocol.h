#pragma once

constexpr short PORT = 3500;
constexpr int WORLD_WIDTH = 200;
constexpr int WORLD_HEIGHT = 200;
constexpr int MAX_PLAYERS = 10000;
constexpr int NUM_NPCS = 1000;
constexpr int NPC_ID_START = 1000000;
constexpr int NPC_MOVE_INTERVAL = 1000; // in milliseconds
constexpr int MAX_NAME_LEN = 20;
constexpr int MAX_CHAT_MSG_LEN = 200;

enum PACKET_TYPE { 
	C2S_LOGIN,			// Client to Server: Login request
						// 사용자 이름을 포함한 로그인 요청 패킷	
	C2S_MOVE,			// Client to Server: Move request
						// 이동 방향과 이동 시간을 포함한 이동 요청 패킷
	C2S_CHAT,			// Client to Server: Chat message
						// 채팅 메시지를 포함한 채팅 요청 패킷
	C2S_ATTACK,			// Client to Server: Attack request
						// 공격 요청 패킷 (4 방향 동시 공격)
	C2S_TELEPORT,		// Client to Server: Teleport request
						// 텔레포트 요청 패킷 (목적지 좌표 포함)
						// STRESS TEST용으로 추가한 패킷입니다. 시작 마을에 몰리는 것을 방지.
	C2S_LOGOUT,			// Client to Server: Logout request

	S2C_LOGIN_RESULT,	//	Server to Client: Login result
						// 로그인 결과 패킷 (성공 여부와 메시지 포함)
	S2C_AVATAR_INFO,	//	Server to Client: Avatar information
	S2C_ADD_OBJECT,		//	Server to Client: Add player or NPC		
	S2C_REMOVE_OBJECT,	//	Server to Client: Remove player or NPC
	S2C_MOVE_OBJECT,	//	Server to Client: Move player or NPC
	S2C_CHAT_MESSAGE,	//	Server to Client: Chat message
	S2C_STATUS_CHANGE,	//	Server to Client: Update player or NPC status (e.g., health, buffs)	
};

#pragma pack(push, 1) // Ensure no padding between struct members
struct C2S_Login {
	unsigned char size;
	PACKET_TYPE   type;
	char username[MAX_NAME_LEN];
};

struct C2S_Move {
	unsigned char size;
	PACKET_TYPE   type;
	short x;
	short y;
	int move_time; // in milliseconds
};

struct C2S_Chat {
	unsigned char size;
	PACKET_TYPE   type;
	char message[MAX_CHAT_MSG_LEN];
};

struct C2S_Attack {
	unsigned char size;
	PACKET_TYPE   type;
};

struct C2S_Teleport {
	unsigned char size;
	PACKET_TYPE   type;
	short x;
	short y;
};

struct C2S_Logout {
	unsigned char size;
	PACKET_TYPE   type;
};

struct S2C_LoginResult {
	unsigned char size;
	PACKET_TYPE   type;
	bool success;
	char message[50];
};

struct S2C_AvatarInfo {
	unsigned char size;
	PACKET_TYPE   type;
	int playerId;
	int visualId; // for future use (different visual appearances)
	short x;
	short y;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

struct S2C_AddObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	int visual_id; // for future use (different visual appearances)
	char obj_name[MAX_NAME_LEN];
	short x;
	short y;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

struct S2C_RemoveObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
};

struct S2C_MoveObject {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	short x;
	short y;
	int move_time; // in milliseconds
};

struct S2C_ChatMessage {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	char message[MAX_CHAT_MSG_LEN];
};

struct S2C_StatusChange {
	unsigned char size;
	PACKET_TYPE   type;
	int object_id;
	int hp;
	int max_hp;
	unsigned long long exp;
	unsigned char level;
};

#pragma pack(pop) // Restore default packing
