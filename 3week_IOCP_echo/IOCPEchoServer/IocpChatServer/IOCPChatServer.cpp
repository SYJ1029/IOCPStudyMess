#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <unordered_map>
using namespace std;

#pragma comment(lib, "WS2_32.LIB")
#pragma comment(lib, "MSWSOCK.LIB")

const short SERVER_PORT = 4000;
const int BUFSIZE = 256;

enum IOType { IO_SEND, IO_RECV, IO_ACCPET };

class EXP_OVER {
public:
	WSAOVERLAPPED _wsa_over;
	IOType m_ioType;
	WSABUF m_wsa;
	int client_id;
	char m_buf[BUFSIZE];

public:
	EXP_OVER() : m_ioType(IO_RECV), client_id(-1)
	{
		ZeroMemory(&_wsa_over, sizeof(_wsa_over));
		m_wsa.buf = m_buf;
		m_wsa.len = sizeof(m_buf);
		ZeroMemory(m_buf, sizeof(m_buf));
	}
	EXP_OVER(IOType iot) : m_ioType(iot), client_id(-1)
	{
		ZeroMemory(&_wsa_over, sizeof(_wsa_over));
		m_wsa.buf = m_buf;
		m_wsa.len = sizeof(m_buf);
		ZeroMemory(m_buf, sizeof(m_buf));
	}
	~EXP_OVER() {}
};

class SESSION {
private:
	SOCKET _socket;
	int m_id;

public:
	EXP_OVER recv_over;

	SESSION() {
		cout << "Unexpected Constructor Call Error!\n";
		exit(-1);
	}

	SESSION(int id, SOCKET s) : _socket(s), m_id(id), recv_over(IO_RECV) {
		recv_over.client_id = id;
	}

	~SESSION() { closesocket(_socket); }

	void do_recv() {
		DWORD recv_flag = 0;
		DWORD recv_bytes = 0;
		ZeroMemory(&recv_over._wsa_over, sizeof(recv_over._wsa_over));
		recv_over.m_ioType = IO_RECV;
		WSARecv(_socket, &recv_over.m_wsa, 1, &recv_bytes, &recv_flag, &recv_over._wsa_over, nullptr);
	}

	void do_send(int sender_id, int num_bytes, const char* mess) {
		EXP_OVER* o = new EXP_OVER(IO_SEND);

		// [0]=전체 패킷 크기, [1]=sender_id, [2..]=메시지
		const int payload = min(num_bytes, BUFSIZE - 2);
		o->m_buf[0] = static_cast<char>(payload + 2);  // 헤더(2) + payload
		o->m_buf[1] = static_cast<char>(sender_id);
		if (payload > 0) memcpy(o->m_buf + 2, mess, payload);

		o->m_wsa.len = payload + 2;
		DWORD send_bytes = 0;
		WSASend(_socket, &o->m_wsa, 1, &send_bytes, 0, &o->_wsa_over, nullptr);
	}
};

unordered_map<int, SESSION> clients;

void error_display(const wchar_t* msg, int err_no)
{
	WCHAR* lpMsgBuf = nullptr;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);

	std::wcout << msg;
	std::wcout << L" === 에러 " << (lpMsgBuf ? lpMsgBuf : L"(null)") << std::endl;

	if (lpMsgBuf) LocalFree(lpMsgBuf);
}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);

	SOCKET s_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	ZeroMemory(&server_addr, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	bind(s_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(s_socket, SOMAXCONN);

	HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	CreateIoCompletionPort((HANDLE)s_socket, iocp, 0, 0);

	SOCKET c_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);
	EXP_OVER accept_over(IO_ACCPET);

	AcceptEx(s_socket, c_socket, &accept_over.m_buf,
		0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
		NULL, &accept_over._wsa_over);

	int next_client_id = 1;

	for (;;)
	{
		DWORD num_bytes = 0;
		ULONG_PTR key = 0;
		LPWSAOVERLAPPED o = nullptr;

		GetQueuedCompletionStatus(iocp, &num_bytes, &key, &o, INFINITE);
		if (o == nullptr) {
			error_display(L"GetQueuedCompletionStatus() Error!", WSAGetLastError());
			continue;
		}

		EXP_OVER* exp_over = reinterpret_cast<EXP_OVER*>(o);

		switch (exp_over->m_ioType)
		{
		case IO_ACCPET:
		{
			cout << "New Client Connected! Socket : " << c_socket << endl;

			const int client_id = next_client_id++;
			CreateIoCompletionPort((HANDLE)c_socket, iocp, (ULONG_PTR)client_id, 0);
			clients.try_emplace(client_id, client_id, c_socket);
			clients[client_id].do_recv();

			c_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);
			ZeroMemory(&accept_over._wsa_over, sizeof(accept_over._wsa_over));
			accept_over.m_ioType = IO_ACCPET;
			AcceptEx(s_socket, c_socket, &accept_over.m_buf,
				0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
				NULL, &accept_over._wsa_over);
			break;
		}
		case IO_RECV:
		{
			const int client_id = exp_over->client_id;

			if (num_bytes == 0) {
				cout << "Client Disconnected : " << client_id << endl;
				clients.erase(client_id);
				break;
			}

			cout << "Client[" << client_id << "] Sent [" << num_bytes << " bytes] : "
				<< clients[client_id].recv_over.m_buf << endl;

			for (auto& kv : clients) {
				if (kv.first == client_id) continue;
				kv.second.do_send(client_id, (int)num_bytes, clients[client_id].recv_over.m_buf);
			}

			clients[client_id].do_recv();
			break;
		}
		case IO_SEND:
		{
			delete exp_over;
			break;
		}
		default:
			break;
		}
	}

	clients.clear();
	closesocket(s_socket);
	WSACleanup();
	return 0;
}