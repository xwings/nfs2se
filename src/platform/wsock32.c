/* Winsock imports. Browsers cannot open TCP/UDP sockets, so the network
 * subsystem reports itself as unavailable and every socket call fails; the
 * game then offers single-player and split-screen modes only. */
#include "platform.h"

#define WSASYSNOTREADY 10091
#define WSAENETDOWN 10050
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (-1)
#define INADDR_NONE (-1)

int32_t WSAStartup_wrap(int32_t version, void *data)
{
	(void)version;
	(void)data;
	return WSASYSNOTREADY;
}

int32_t WSACleanup_wrap(void)
{
	return 0;
}

int32_t WSAGetLastError_wrap(void)
{
	return WSAENETDOWN;
}

int32_t WSAFDIsSet_wrap(int32_t sock, void *set)
{
	(void)sock;
	(void)set;
	return 0;
}

int32_t socket_wrap(int32_t af, int32_t type, int32_t protocol)
{
	(void)af;
	(void)type;
	(void)protocol;
	return INVALID_SOCKET;
}

int32_t closesocket_wrap(int32_t sock)
{
	(void)sock;
	return SOCKET_ERROR;
}

int32_t accept_wrap(int32_t sock, void *addr, void *addrLen)
{
	(void)sock;
	(void)addr;
	(void)addrLen;
	return INVALID_SOCKET;
}

int32_t bind_wrap(int32_t sock, const void *addr, int32_t addrLen)
{
	(void)sock;
	(void)addr;
	(void)addrLen;
	return SOCKET_ERROR;
}

int32_t connect_wrap(int32_t sock, const void *addr, int32_t addrLen)
{
	(void)sock;
	(void)addr;
	(void)addrLen;
	return SOCKET_ERROR;
}

int32_t listen_wrap(int32_t sock, int32_t backlog)
{
	(void)sock;
	(void)backlog;
	return SOCKET_ERROR;
}

int32_t getsockname_wrap(int32_t sock, void *addr, void *addrLen)
{
	(void)sock;
	(void)addr;
	(void)addrLen;
	return SOCKET_ERROR;
}

int32_t ioctlsocket_wrap(int32_t sock, int32_t cmd, void *arg)
{
	(void)sock;
	(void)cmd;
	(void)arg;
	return SOCKET_ERROR;
}

int32_t setsockopt_wrap(int32_t sock, int32_t level, int32_t name, const void *value, int32_t length)
{
	(void)sock;
	(void)level;
	(void)name;
	(void)value;
	(void)length;
	return SOCKET_ERROR;
}

int32_t select_wrap(int32_t nfds, void *readFds, void *writeFds, void *exceptFds, void *timeout)
{
	(void)nfds;
	(void)readFds;
	(void)writeFds;
	(void)exceptFds;
	(void)timeout;
	return SOCKET_ERROR;
}

int32_t send_wrap(int32_t sock, const void *buf, int32_t len, int32_t flags)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)flags;
	return SOCKET_ERROR;
}

int32_t recv_wrap(int32_t sock, void *buf, int32_t len, int32_t flags)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)flags;
	return SOCKET_ERROR;
}

int32_t sendto_wrap(int32_t sock, const void *buf, int32_t len, int32_t flags, const void *to, int32_t toLen)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)flags;
	(void)to;
	(void)toLen;
	return SOCKET_ERROR;
}

int32_t recvfrom_wrap(int32_t sock, void *buf, int32_t len, int32_t flags, void *from, void *fromLen)
{
	(void)sock;
	(void)buf;
	(void)len;
	(void)flags;
	(void)from;
	(void)fromLen;
	return SOCKET_ERROR;
}

int32_t gethostbyname_wrap(const char *name)
{
	(void)name;
	return 0;
}

int32_t gethostname_wrap(char *name, int32_t length)
{
	(void)name;
	(void)length;
	return SOCKET_ERROR;
}

int32_t htons_wrap(uint32_t value)
{
	return (int32_t)(((value & 0xFF) << 8) | ((value >> 8) & 0xFF));
}

int32_t inet_addr_wrap(const char *text)
{
	(void)text;
	return INADDR_NONE;
}

int32_t inet_ntoa_wrap(uint32_t addr)
{
	(void)addr;
	static const char none[] = "0.0.0.0";
	return (int32_t)(uintptr_t)none;
}
