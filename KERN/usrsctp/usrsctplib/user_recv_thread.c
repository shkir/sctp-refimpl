#include <sys/types.h>
#if !defined (__Userspace_os_Windows)
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#endif
#include <sys/uio.h>
#include <netinet/sctp_os.h>
#include <netinet/sctp_var.h>
#include <netinet/sctp_pcb.h>

/* extern __Userspace__ variable in user_recv_thread.h */
int userspace_rawsctp = -1; /* needs to be declared = -1 */
int userspace_udpsctp = -1; 
int userspace_route = -1;

/* local macros and datatypes used to get IP addresses system independently */
#if defined IP_RECVDSTADDR
# define DSTADDR_SOCKOPT IP_RECVDSTADDR
# define DSTADDR_DATASIZE (CMSG_SPACE(sizeof(struct in_addr)))
# define dstaddr(x) (CMSG_DATA(x))
#elif defined IP_PKTINFO
# define DSTADDR_SOCKOPT IP_PKTINFO
# define DSTADDR_DATASIZE (CMSG_SPACE(sizeof(struct in_pktinfo)))
# define dstaddr(x) (&(((struct in_pktinfo *)(CMSG_DATA(x)))->ipi_addr))
#else
# error "can't determine socket option to use to get UDP IP"
#endif

void recv_thread_destroy_udp(void *);
void recv_thread_destroy_raw(void *);
#define MAXLEN_MBUF_CHAIN 32 /* What should this value be? */

/* need ref to this for destroy... */
struct mbuf **recvmbuf;

static void *
recv_function_raw(void *arg)
{
#if !defined (__Userspace_os_Windows)
	struct iovec recv_iovec[MAXLEN_MBUF_CHAIN];
	int iovcnt = MAXLEN_MBUF_CHAIN;
#else
	WSABUF recv_iovec[MAXLEN_MBUF_CHAIN];
	int nResult, m_ErrorCode;
	DWORD flags;
	struct sockaddr from;
	int fromlen;
#endif

	/*Initially the entire set of mbufs is to be allocated.
	  to_fill indicates this amount. */
	int to_fill = MAXLEN_MBUF_CHAIN;
	/* iovlen is the size of each mbuf in the chain */
	int i, n, ncounter = 0;
	int iovlen = MCLBYTES;
	int want_ext = (iovlen > MLEN)? 1 : 0;
	int want_header = 0;

	recvmbuf = malloc(sizeof(struct mbuf *) * MAXLEN_MBUF_CHAIN);

	while (1) {
		for (i = 0; i < to_fill; i++) {
			/* Not getting the packet header. Tests with chain of one run
			   as usual without having the packet header.
			   Have tried both sending and receiving
			 */
			recvmbuf[i] = sctp_get_mbuf_for_msg(iovlen, want_header, M_DONTWAIT, want_ext, MT_DATA);
#if !defined (__Userspace_os_Windows)
			recv_iovec[i].iov_base = (caddr_t)recvmbuf[i]->m_data;
			recv_iovec[i].iov_len = iovlen;
#else
			recv_iovec[i].buf = (caddr_t)recvmbuf[i]->m_data;
			recv_iovec[i].len = iovlen;
#endif
		}
		to_fill = 0;
#if defined (__Userspace_os_Windows)
		flags = 0;
		ncounter = 0;
		fromlen = sizeof(struct sockaddr);
		bzero((void *)&from, sizeof(struct sockaddr));

		nResult = WSARecvFrom(userspace_rawsctp, recv_iovec, MAXLEN_MBUF_CHAIN, (LPDWORD)&ncounter, (LPDWORD)&flags, &from, &fromlen, NULL, NULL);
		if (nResult != 0) {
			m_ErrorCode = WSAGetLastError();
			printf("error: %d\n", m_ErrorCode);
		}
		n = ncounter;
#else
		ncounter = n = readv(userspace_rawsctp, recv_iovec, iovcnt);
#endif
		assert (n <= (MAXLEN_MBUF_CHAIN * iovlen));
		SCTP_HEADER_LEN(recvmbuf[0]) = n; /* length of total packet */
		
		if (n <= iovlen) {
			SCTP_BUF_LEN(recvmbuf[0]) = n;
			(to_fill)++;
		} else {
			i = 0;
			SCTP_BUF_LEN(recvmbuf[0]) = iovlen;

			ncounter -= iovlen;
			(to_fill)++;
			do {
				recvmbuf[i]->m_next = recvmbuf[i+1];
				SCTP_BUF_LEN(recvmbuf[i]->m_next) = min(ncounter, iovlen);
				i++;
				ncounter -= iovlen;
				(to_fill)++;
			} while (ncounter > 0);
		}
		assert(to_fill <= MAXLEN_MBUF_CHAIN);
		SCTPDBG(SCTP_DEBUG_INPUT1, "%s: Received %d bytes.", __func__, n);
		SCTPDBG(SCTP_DEBUG_INPUT1, " - calling sctp_input with off=%d\n", (int)sizeof(struct ip));
		
		/* process incoming data */
		/* sctp_input frees this mbuf. */
		sctp_input_with_port(recvmbuf[0], sizeof(struct ip), 0);
	}
	return NULL;
}

/* need ref to this for destroy... */
struct mbuf **udprecvmbuf;

static void *
recv_function_udp(void *arg)
{
	/*Initially the entire set of mbufs is to be allocated.
	  to_fill indicates this amount. */
	int to_fill = MAXLEN_MBUF_CHAIN;
	/* iovlen is the size of each mbuf in the chain */
	int i, n, ncounter;
	int iovlen = MCLBYTES;
	int want_ext = (iovlen > MLEN)? 1 : 0;
	int want_header = 0;
	struct ip *ip;
	struct mbuf *ip_m;
	struct sockaddr_in src, dst;
	char cmsgbuf[DSTADDR_DATASIZE];
#if !defined (__Userspace_os_Windows)
	struct iovec iov[MAXLEN_MBUF_CHAIN];
	struct msghdr msg;
	struct cmsghdr *cmsgptr;
#else
	GUID WSARecvMsg_GUID = WSAID_WSARECVMSG;
	LPFN_WSARECVMSG WSARecvMsg;
	char ControlBuffer[1024];
	WSABUF iov[MAXLEN_MBUF_CHAIN];
	WSAMSG win_msg;
	int nResult, m_ErrorCode;
	WSACMSGHDR *pCMsgHdr;
#endif

	udprecvmbuf = malloc(sizeof(struct mbuf *) * MAXLEN_MBUF_CHAIN);

	while (1) {
		for (i = 0; i < to_fill; i++) {
			/* Not getting the packet header. Tests with chain of one run
			   as usual without having the packet header.
			   Have tried both sending and receiving
			 */
			udprecvmbuf[i] = sctp_get_mbuf_for_msg(iovlen, want_header, M_DONTWAIT, want_ext, MT_DATA);
#if !defined (__Userspace_os_Windows)
			iov[i].iov_base = (caddr_t)udprecvmbuf[i]->m_data;
			iov[i].iov_len = iovlen;
#else
			iov[i].buf = (caddr_t)udprecvmbuf[i]->m_data;
			iov[i].len = iovlen;
#endif
		}
		to_fill = 0;
#if !defined (__Userspace_os_Windows)
		bzero((void *)&msg, sizeof(struct msghdr));
#else
		bzero((void *)&win_msg, sizeof(WSAMSG));
#endif
		bzero((void *)&src, sizeof(struct sockaddr_in));
		bzero((void *)&dst, sizeof(struct sockaddr_in));
		bzero((void *)cmsgbuf, DSTADDR_DATASIZE);

#if !defined (__Userspace_os_Windows)
		msg.msg_name = (void *)&src;
		msg.msg_namelen = sizeof(struct sockaddr_in);
		msg.msg_iov = iov;
		msg.msg_iovlen = MAXLEN_MBUF_CHAIN;
		msg.msg_control = (void *)cmsgbuf;
		msg.msg_controllen = DSTADDR_DATASIZE;
		msg.msg_flags = 0;

		ncounter = n = recvmsg(userspace_udpsctp, &msg, 0);
#else
		nResult = WSAIoctl(userspace_udpsctp, SIO_GET_EXTENSION_FUNCTION_POINTER,
		 &WSARecvMsg_GUID, sizeof WSARecvMsg_GUID,
		 &WSARecvMsg, sizeof WSARecvMsg,
		 &ncounter, NULL, NULL);
		if (nResult == SOCKET_ERROR) {
			m_ErrorCode = WSAGetLastError();
			WSARecvMsg = NULL;
		}
		win_msg.name = (void *)&src;
		win_msg.namelen = sizeof(struct sockaddr_in);
		win_msg.lpBuffers = iov;
		win_msg.dwBufferCount = MAXLEN_MBUF_CHAIN;
		win_msg.Control.len = sizeof ControlBuffer;
		win_msg.Control.buf = ControlBuffer;
		win_msg.dwFlags = 0;
		nResult = WSARecvMsg(userspace_udpsctp, &win_msg, &ncounter, NULL, NULL);
		if (nResult != 0) {
			m_ErrorCode = WSAGetLastError();
		}
		n = ncounter;
#endif
		assert (n <= (MAXLEN_MBUF_CHAIN * iovlen));
		SCTP_HEADER_LEN(udprecvmbuf[0]) = n; /* length of total packet */

		if (n <= iovlen) {
			SCTP_BUF_LEN(udprecvmbuf[0]) = n;
			(to_fill)++;
		} else {
			i = 0;
			SCTP_BUF_LEN(udprecvmbuf[0]) = iovlen;

			ncounter -= iovlen;
			(to_fill)++;
			do {
				udprecvmbuf[i]->m_next = udprecvmbuf[i+1];
				SCTP_BUF_LEN(udprecvmbuf[i]->m_next) = min(ncounter, iovlen);
				i++;
				ncounter -= iovlen;
				(to_fill)++;
			} while (ncounter > 0);
		}
		assert(to_fill <= MAXLEN_MBUF_CHAIN);

#if !defined (__Userspace_os_Windows)
		for (cmsgptr = CMSG_FIRSTHDR(&msg); cmsgptr != NULL; cmsgptr = CMSG_NXTHDR(&msg, cmsgptr)) {
			if ((cmsgptr->cmsg_level == IPPROTO_IP) && (cmsgptr->cmsg_type == DSTADDR_SOCKOPT)) {
				dst.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
				dst.sin_len = sizeof(struct sockaddr_in);
#endif
				dst.sin_port = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
				memcpy((void *)&dst.sin_addr, (const void *) dstaddr(cmsgptr), sizeof(struct in_addr));
			}
		}
#else
		for (pCMsgHdr = WSA_CMSG_FIRSTHDR(&win_msg); pCMsgHdr != NULL; pCMsgHdr = WSA_CMSG_NXTHDR(&win_msg, pCMsgHdr)) {
			if ((pCMsgHdr->cmsg_level == IPPROTO_IP) && (pCMsgHdr->cmsg_type == DSTADDR_SOCKOPT)) {
				dst.sin_family = AF_INET;
				dst.sin_port = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
				memcpy((void *)&dst.sin_addr, (const void *) dstaddr(pCMsgHdr), sizeof(struct in_addr));
			}
		}
#endif

		ip_m = sctp_get_mbuf_for_msg(sizeof(struct ip), 1, M_DONTWAIT, 1, MT_DATA);

		ip = mtod(ip_m, struct ip *);
		bzero((void *)ip, sizeof(struct ip));
		ip->ip_v = IPVERSION;
		ip->ip_len = n;
#if defined(__Userspace_os_Linux) ||  defined (__Userspace_os_Windows)
		ip->ip_len += sizeof(struct ip);
#endif
#if defined (__Userspace_os_Windows)
		ip->ip_len = htons(ip->ip_len);
#endif
		ip->ip_src = src.sin_addr;
		ip->ip_dst = dst.sin_addr;
		SCTP_HEADER_LEN(ip_m) = sizeof(struct ip) + n;
		SCTP_BUF_LEN(ip_m) = sizeof(struct ip);
		SCTP_BUF_NEXT(ip_m) = udprecvmbuf[0];

		SCTPDBG(SCTP_DEBUG_INPUT1, "%s: Received %d bytes.", __func__, n);
		SCTPDBG(SCTP_DEBUG_INPUT1, " - calling sctp_input with off=%d\n", (int)sizeof(struct ip));

		/* process incoming data */
		/* sctp_input frees this mbuf. */
		sctp_input_with_port(ip_m, sizeof(struct ip), src.sin_port);
	}
	return NULL;
}


static int
setReceiveBufferSize(int sfd, int new_size)
{
	int ch = new_size;
	if (setsockopt (sfd, SOL_SOCKET, SO_RCVBUF, (void*)&ch, sizeof(ch)) < 0) {
		perror("setReceiveBufferSize setsockopt: SO_RCVBUF failed !\n");
		exit(1);
	}
	return 0;
}

static int
setSendBufferSize(int sfd, int new_size)
{
	int ch = new_size;
	if (setsockopt (sfd, SOL_SOCKET, SO_SNDBUF, (void*)&ch, sizeof(ch)) < 0) {
		perror("setSendBufferSize setsockopt: SO_RCVBUF failed !\n");
		exit(1);
	}
	return 0;
}

void 
recv_thread_init()
{
	userland_thread_t recvthreadraw , recvthreadudp;
	const int hdrincl = 1;
	const int on = 1;
	struct sockaddr_in addr_ipv4;

	/* use raw socket, create if not initialized */
	if (userspace_rawsctp == -1) {
		if ((userspace_rawsctp = socket(AF_INET, SOCK_RAW, IPPROTO_SCTP)) < 0) {
			perror("raw socket failure. continue with only UDP socket...\n");
		} else {
			/* complete setting up the raw SCTP socket */
			if (setsockopt(userspace_rawsctp, IPPROTO_IP, IP_HDRINCL,(const void*)&hdrincl, sizeof(int)) < 0) {
				perror("raw setsockopt failure\n");
				exit(1);
			}

		memset((void *)&addr_ipv4, 0, sizeof(struct sockaddr_in));
#ifdef HAVE_SIN_LEN
		addr_ipv4.sin_len         = sizeof(struct sockaddr_in);
#endif
		addr_ipv4.sin_family      = AF_INET;
		addr_ipv4.sin_port        = htons(0);
		/*inet_pton(AF_INET, "10.0.1.115", &addr_ipv4.sin_addr.s_addr);*/
		addr_ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(userspace_rawsctp, (const struct sockaddr *)&addr_ipv4, sizeof(struct sockaddr_in)) < 0) {
			perror("bind");
			exit(1);
		}

			setReceiveBufferSize(userspace_rawsctp, SB_RAW); /* 128K */
			setSendBufferSize(userspace_rawsctp, SB_RAW); /* 128K Is this setting net.inet.raw.maxdgram value? Should it be set to 64K? */
		}
	}

	 /* use UDP socket, create if not initialized */
	if (userspace_udpsctp == -1) {
		if ((userspace_udpsctp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
			perror("UDP socket failure");
			exit(1);
		}
		if (setsockopt(userspace_udpsctp, IPPROTO_IP, DSTADDR_SOCKOPT, (const void *)&on, (int)sizeof(int)) < 0) {
			perror("setsockopt: DSTADDR_SOCKOPT");
			exit(1);
		}
		memset((void *)&addr_ipv4, 0, sizeof(struct sockaddr_in));
#ifdef HAVE_SIN_LEN
		addr_ipv4.sin_len         = sizeof(struct sockaddr_in);
#endif
		addr_ipv4.sin_family      = AF_INET;
		addr_ipv4.sin_port        = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
		addr_ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(userspace_udpsctp, (const struct sockaddr *)&addr_ipv4, sizeof(struct sockaddr_in)) < 0) {
			perror("bind");
			exit(1);
		}
		setReceiveBufferSize(userspace_udpsctp, SB_RAW); /* 128K */
		setSendBufferSize(userspace_udpsctp, SB_RAW); /* 128K Is this setting net.inet.raw.maxdgram value? Should it be set to 64K? */
	}

	/* start threads here for receiving incoming messages */
#if !defined (__Userspace_os_Windows)
	if (userspace_rawsctp != -1) {
		int rc;

		if ((rc = pthread_create(&recvthreadraw, NULL, &recv_function_raw, (void *)NULL))) {
			printf("ERROR; return code from recvthread pthread_create() is %d\n", rc);
			exit(1);
		}
	}
	if (userspace_udpsctp != -1) {
		int rc;

		if ((rc = pthread_create(&recvthreadudp, NULL, &recv_function_udp, (void *)NULL))) {
			printf("ERROR; return code from recvthread pthread_create() is %d\n", rc);
			exit(1);
		}
	}
#else
	if (userspace_rawsctp != -1) {
		if ((recvthreadraw = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&recv_function_raw, NULL, 0, NULL))==NULL) {
			printf("ERROR; Creating recvthreadraw failed\n");
			exit(1);
		}
	}
	if (userspace_udpsctp != -1) {
		if ((recvthreadudp = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&recv_function_udp, NULL, 0, NULL))==NULL) {
			printf("ERROR; Creating recvthreadudp failed\n");
			exit(1);
		}
	}
#endif
}

void
recv_thread_destroy_raw(void *parm)
{
	int i;

	/* close sockets if they are open */
	if (userspace_route != -1)
		close(userspace_route);
	if (userspace_rawsctp != -1)
		close(userspace_rawsctp);

	/*
	 *  call m_free on contents of recvmbuf array
	*/
	for(i=0; i < MAXLEN_MBUF_CHAIN; i++) {
		m_free(recvmbuf[i]);
	}

	/* free the array itself */
	free(recvmbuf);
}

void
recv_thread_destroy_udp(void *parm)
{
	int i;

	/* socket closed in 
	void sctp_over_udp_stop(void)
	*/

	/*
	 *   call m_free on contents of udprecvmbuf array
	 */
	for(i=0; i < MAXLEN_MBUF_CHAIN; i++) {
		m_free(udprecvmbuf[i]);
	}

	/* free the array itself */
	free(udprecvmbuf);
}