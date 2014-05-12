/*
  The oRTP library is an RTP (Realtime Transport Protocol - rfc3550) stack.
  Copyright (C) 2001  Simon MORLAT simon.morlat@linphone.org

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(WIN32) || defined(_WIN32_WCE)
#include "ortp-config-win32.h"
#elif HAVE_CONFIG_H
#include "ortp-config.h" /*needed for HAVE_SYS_UIO_H and HAVE_ARC4RANDOM */
#endif
#include "ortp/ortp.h"
#include "utils.h"
#include "ortp/rtpsession.h"
#include "rtpsession_priv.h"

#if (_WIN32_WINNT >= 0x0600)
#include <delayimp.h>
#undef ExternC
#ifndef WINAPI_FAMILY_PHONE_APP
#include <QOS2.h>
#endif
#endif

#if (defined(WIN32) || defined(_WIN32_WCE)) && !defined(WINAPI_FAMILY_PHONE_APP)
#include <mswsock.h>
#endif

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#define USE_SENDMSG 1
#endif

#define can_connect(s)	( (s)->use_connect && !(s)->symmetric_rtp)

#if defined(WIN32) || defined(_WIN32_WCE)
#ifndef WSAID_WSARECVMSG
/* http://source.winehq.org/git/wine.git/blob/HEAD:/include/mswsock.h */
#define WSAID_WSARECVMSG {0xf689d7c8,0x6f1f,0x436b,{0x8a,0x53,0xe5,0x4f,0xe3,0x51,0xc3,0x22}}
#define MAX_NATURAL_ALIGNMENT sizeof(DWORD)
#define TYPE_ALIGNMENT(t) FIELD_OFFSET(struct { char x; t test; },test)
typedef WSACMSGHDR *LPWSACMSGHDR;
#define WSA_CMSGHDR_ALIGN(length) (((length) + TYPE_ALIGNMENT(WSACMSGHDR)-1) & (~(TYPE_ALIGNMENT(WSACMSGHDR)-1)))
#define WSA_CMSGDATA_ALIGN(length) (((length) + MAX_NATURAL_ALIGNMENT-1) & (~(MAX_NATURAL_ALIGNMENT-1)))
#define WSA_CMSG_FIRSTHDR(msg) (((msg)->Control.len >= sizeof(WSACMSGHDR)) ? (LPWSACMSGHDR)(msg)->Control.buf : (LPWSACMSGHDR)NULL)
#define WSA_CMSG_NXTHDR(msg,cmsg) ((!(cmsg)) ? WSA_CMSG_FIRSTHDR(msg) : ((((u_char *)(cmsg) + WSA_CMSGHDR_ALIGN((cmsg)->cmsg_len) + sizeof(WSACMSGHDR)) > (u_char *)((msg)->Control.buf) + (msg)->Control.len) ? (LPWSACMSGHDR)NULL : (LPWSACMSGHDR)((u_char *)(cmsg) + WSA_CMSGHDR_ALIGN((cmsg)->cmsg_len))))
#define WSA_CMSG_DATA(cmsg) ((u_char *)(cmsg) + WSA_CMSGDATA_ALIGN(sizeof(WSACMSGHDR)))
#endif
#undef CMSG_FIRSTHDR
#define CMSG_FIRSTHDR WSA_CMSG_FIRSTHDR
#undef CMSG_NXTHDR
#define CMSG_NXTHDR WSA_CMSG_NXTHDR
#undef CMSG_DATA
#define CMSG_DATA WSA_CMSG_DATA
typedef INT  (WINAPI * LPFN_WSARECVMSG)(SOCKET, LPWSAMSG, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static LPFN_WSARECVMSG ortp_WSARecvMsg = NULL;

#endif

#if defined(WIN32) || defined(_WIN32_WCE) || defined(__QNX__)
/* Mingw32 does not define AI_V4MAPPED, however it is supported starting from Windows Vista. QNX also does not define AI_V4MAPPED. */
#	ifndef AI_V4MAPPED
#	define AI_V4MAPPED 0x00000800
#	endif
#endif

static bool_t try_connect(int fd, const struct sockaddr *dest, socklen_t addrlen){
	if (connect(fd,dest,addrlen)<0){
		ortp_warning("Could not connect() socket: %s",getSocketError());
		return FALSE;
	}
	return TRUE;
}

static ortp_socket_t create_and_bind(const char *addr, int *port, int *sock_family, bool_t reuse_addr){
	int err;
	int optval = 1;
	ortp_socket_t sock=-1;
	char num[8];
	struct addrinfo hints, *res0, *res;

	if (*port==-1) *port=0;
	if (*port==0) reuse_addr=FALSE;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	snprintf(num, sizeof(num), "%d",*port);
	err = getaddrinfo(addr,num, &hints, &res0);
	if (err!=0) {
		ortp_warning ("Error in getaddrinfo on (addr=%s port=%i): %s", addr, *port, gai_strerror(err));
		return -1;
	}

	for (res = res0; res; res = res->ai_next) {
		sock = socket(res->ai_family, res->ai_socktype, 0);
		if (sock==-1)
			continue;

		if (reuse_addr){
			err = setsockopt (sock, SOL_SOCKET, SO_REUSEADDR,
					(SOCKET_OPTION_VALUE)&optval, sizeof (optval));
			if (err < 0)
			{
				ortp_warning ("Fail to set rtp address reusable: %s.", getSocketError());
			}
		}
#if defined(ORTP_TIMESTAMP)
		err = setsockopt (sock, SOL_SOCKET, SO_TIMESTAMP,
			(SOCKET_OPTION_VALUE)&optval, sizeof (optval));
		if (err < 0)
		{
			ortp_warning ("Fail to set rtp timestamp: %s.",getSocketError());
		}
#endif
		err = 0;
		switch (res->ai_family) {
			default:
			case AF_INET:
#ifdef IP_RECVTTL
				err = setsockopt(sock, IPPROTO_IP, IP_RECVTTL, &optval, sizeof(optval));
#endif
				break;
			case AF_INET6:
#ifdef IPV6_RECVHOPLIMIT
				err = setsockopt(sock, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &optval, sizeof(optval));
#endif
				break;
		}
		if (err < 0) {
			ortp_warning("Fail to set recv TTL/HL socket option: %s.", getSocketError());
		}

		*sock_family=res->ai_family;
		err = bind (sock, res->ai_addr, res->ai_addrlen);
		if (err != 0){
			ortp_debug ("Fail to bind rtp socket to (addr=%s port=%i) : %s.", addr, *port, getSocketError());
			close_socket (sock);
			sock=-1;
			continue;
		}
#ifndef __hpux
		switch (res->ai_family)
		  {
			case AF_INET:
			  if (IN_MULTICAST(ntohl(((struct sockaddr_in *) res->ai_addr)->sin_addr.s_addr)))
			{
				  struct ip_mreq mreq;
			  mreq.imr_multiaddr.s_addr = ((struct sockaddr_in *) res->ai_addr)->sin_addr.s_addr;
			  mreq.imr_interface.s_addr = INADDR_ANY;
			  err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (SOCKET_OPTION_VALUE) &mreq, sizeof(mreq));
			  if (err < 0){
				ortp_warning ("Fail to join address group: %s.", getSocketError());
				close_socket (sock);
				sock=-1;
				continue;
				}
			}
			  break;
			case AF_INET6:
			  if (IN6_IS_ADDR_MULTICAST(&(((struct sockaddr_in6 *) res->ai_addr)->sin6_addr)))
			{
			  struct ipv6_mreq mreq;
			  mreq.ipv6mr_multiaddr = ((struct sockaddr_in6 *) res->ai_addr)->sin6_addr;
			  mreq.ipv6mr_interface = 0;
			  err = setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, (SOCKET_OPTION_VALUE)&mreq, sizeof(mreq));
			  if (err < 0)
				{
				  ortp_warning ("Fail to join address group: %s.", getSocketError());
				  close_socket (sock);
				  sock=-1;
				  continue;
				}
			}
			  break;
		  }
#endif /*hpux*/
		break;
	}
	freeaddrinfo(res0);

#if defined(WIN32) || defined(_WIN32_WCE)
	if (ortp_WSARecvMsg == NULL) {
		GUID guid = WSAID_WSARECVMSG;
		DWORD bytes_returned;
		if (WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
			&ortp_WSARecvMsg, sizeof(ortp_WSARecvMsg), &bytes_returned, NULL, NULL) == SOCKET_ERROR) {
			ortp_warning("WSARecvMsg function not found.");
		}
	}
#endif
	if (sock!=-1){
		set_non_blocking_socket (sock);
		if (*port==0){
			struct sockaddr_storage saddr;
			socklen_t slen=sizeof(saddr);
			err=getsockname(sock,(struct sockaddr*)&saddr,&slen);
			if (err==-1){
				ortp_error("getsockname(): %s",getSocketError());
				close(sock);
				return (ortp_socket_t)-1;
			}
			err=getnameinfo((struct sockaddr*)&saddr, slen, NULL, 0, num, sizeof(num), NI_NUMERICHOST | NI_NUMERICSERV);
			if (err!=0){
				ortp_error("getnameinfo(): %s",gai_strerror(err));
				close(sock);
				return (ortp_socket_t)-1;
			}
			*port=atoi(num);
		}

	}
	return sock;
}

static void set_socket_sizes(int sock, unsigned int sndbufsz, unsigned int rcvbufsz){
	int err;
	bool_t done=FALSE;
	if (sndbufsz>0){
#ifdef SO_SNDBUFFORCE
		err = setsockopt(sock, SOL_SOCKET, SO_SNDBUFFORCE, (void *)&sndbufsz, sizeof(sndbufsz));
		if (err == -1) {
			ortp_error("Fail to increase socket's send buffer size with SO_SNDBUFFORCE: %s.", getSocketError());
		}else done=TRUE;
#endif
		if (!done){
			err = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (void *)&sndbufsz, sizeof(sndbufsz));
			if (err == -1) {
				ortp_error("Fail to increase socket's send buffer size with SO_SNDBUF: %s.", getSocketError());
			}
		}
	}
	done=FALSE;
	if (rcvbufsz>0){
#ifdef SO_RCVBUFFORCE
		err = setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, (void *)&rcvbufsz, sizeof(rcvbufsz));
		if (err == -1) {
			ortp_error("Fail to increase socket's recv buffer size with SO_RCVBUFFORCE: %s.", getSocketError());
		}
#endif
		if (!done){
			err = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void *)&rcvbufsz, sizeof(rcvbufsz));
			if (err == -1) {
				ortp_error("Fail to increase socket's recv buffer size with SO_RCVBUF: %s.", getSocketError());
			}
		}

	}
}

/**
 *rtp_session_set_local_addr:
 *@session:		a rtp session freshly created.
 *@addr:		a local IP address in the xxx.xxx.xxx.xxx form.
 *@rtp_port:		a local port or -1 to let oRTP choose the port randomly
 *@rtcp_port:		a local port or -1 to let oRTP choose the port randomly
 *
 *	Specify the local addr to be use to listen for rtp packets or to send rtp packet from.
 *	In case where the rtp session is send-only, then it is not required to call this function:
 *	when calling rtp_session_set_remote_addr(), if no local address has been set, then the
 *	default INADRR_ANY (0.0.0.0) IP address with a random port will be used. Calling
 *	rtp_sesession_set_local_addr() is mandatory when the session is recv-only or duplex.
 *
 *	Returns: 0 on success.
**/

int
rtp_session_set_local_addr (RtpSession * session, const char * addr, int rtp_port, int rtcp_port)
{
	ortp_socket_t sock;
	int sockfamily;
	if (session->rtp.socket!=(ortp_socket_t)-1){
		/* don't rebind, but close before*/
		rtp_session_release_sockets(session);
	}
	/* try to bind the rtp port */

	sock=create_and_bind(addr,&rtp_port,&sockfamily,session->reuseaddr);
	if (sock!=-1){
		set_socket_sizes(sock,session->rtp.snd_socket_size,session->rtp.rcv_socket_size);
		session->rtp.sockfamily=sockfamily;
		session->rtp.socket=sock;
		session->rtp.loc_port=rtp_port;
		/*try to bind rtcp port */
		sock=create_and_bind(addr,&rtcp_port,&sockfamily,session->reuseaddr);
		if (sock!=(ortp_socket_t)-1){
			session->rtcp.sockfamily=sockfamily;
			session->rtcp.socket=sock;
			session->rtcp.loc_port=rtcp_port;
		}else {
			ortp_debug("Could not create and bind rtcp socket.");
			return -1;
		}

		/* set socket options (but don't change chosen states) */
		rtp_session_set_dscp( session, -1 );
		rtp_session_set_multicast_ttl( session, -1 );
		rtp_session_set_multicast_loopback( session, -1 );
		return 0;
	}
	ortp_debug("Could not bind RTP socket on port to %s port %i",addr,rtp_port);
	return -1;
}


/**
 *rtp_session_set_pktinfo:
 *@session: a rtp session
 *@activate: activation flag (0 to deactivate, other value to activate)
 *
 * (De)activates packet info for incoming and outgoing packets.
 *
 * Returns: 0 on success.
 *
**/
int rtp_session_set_pktinfo(RtpSession *session, int activate)
{
	int retval;
	int optname;
#if defined(WIN32) || defined(_WIN32_WCE)
	char optval[sizeof(DWORD)];
	int optlen = sizeof(optval);
#else
	int *optval = &activate;
	int optlen = sizeof(activate);
#endif

	// Dont't do anything if socket hasn't been created yet
	if (session->rtp.socket == (ortp_socket_t)-1) return 0;

#if defined(WIN32) || defined(_WIN32_WCE)
	memset(optval, activate, sizeof(optval));
#endif

	switch (session->rtp.sockfamily) {
		case AF_INET:
#ifdef IP_PKTINFO
			optname = IP_PKTINFO;
#else
			optname = IP_RECVDSTADDR;
#endif
			retval = setsockopt(session->rtp.socket, IPPROTO_IP, optname, optval, optlen);
			if (retval < 0) break;
			retval = setsockopt(session->rtcp.socket, IPPROTO_IP, optname, optval, optlen);
			break;
		case AF_INET6:
#ifdef IPV6_PKTINFO
			optname = IPV6_PKTINFO;
#else
			optname = IPV6_RECVDSTADDR;
#endif
			retval = setsockopt(session->rtp.socket, IPPROTO_IPV6, optname, optval, optlen);
			if (retval < 0) break;
			retval = setsockopt(session->rtcp.socket, IPPROTO_IPV6, optname, optval, optlen);
			break;
		default:
			retval = -1;
			break;
	}

	if (retval < 0) ortp_warning("Failed to set packet info on socket.");
	return retval;
}


/**
 *rtp_session_set_multicast_ttl:
 *@session: a rtp session
 *@ttl: desired Multicast Time-To-Live
 *
 * Sets the TTL (Time-To-Live) for outgoing multicast packets.
 *
 * Returns: 0 on success.
 *
**/
int rtp_session_set_multicast_ttl(RtpSession *session, int ttl)
{
	int retval;

	// Store new TTL if one is specified
	if (ttl>0) session->multicast_ttl = ttl;

	// Don't do anything if socket hasn't been created yet
	if (session->rtp.socket == (ortp_socket_t)-1) return 0;

	switch (session->rtp.sockfamily) {
		case AF_INET: {

			retval= setsockopt(session->rtp.socket, IPPROTO_IP, IP_MULTICAST_TTL,
						 (SOCKET_OPTION_VALUE)  &session->multicast_ttl, sizeof(session->multicast_ttl));

			if (retval<0) break;

			retval= setsockopt(session->rtcp.socket, IPPROTO_IP, IP_MULTICAST_TTL,
					 (SOCKET_OPTION_VALUE)	   &session->multicast_ttl, sizeof(session->multicast_ttl));

		} break;
		case AF_INET6: {

			retval= setsockopt(session->rtp.socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
					 (SOCKET_OPTION_VALUE)&session->multicast_ttl, sizeof(session->multicast_ttl));

			if (retval<0) break;

			retval= setsockopt(session->rtcp.socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
					 (SOCKET_OPTION_VALUE) &session->multicast_ttl, sizeof(session->multicast_ttl));
		} break;
	default:
		retval=-1;
	}

	if (retval<0)
		ortp_warning("Failed to set multicast TTL on socket.");


	return retval;
}


/**
 *rtp_session_get_multicast_ttl:
 *@session: a rtp session
 *
 * Returns the TTL (Time-To-Live) for outgoing multicast packets.
 *
**/
int rtp_session_get_multicast_ttl(RtpSession *session)
{
	return session->multicast_ttl;
}


/**
 *rtp_session_set_multicast_loopback:
 *@session: a rtp session
 *@ttl: desired Multicast Time-To-Live
 *
 * Sets the TTL (Time-To-Live) for outgoing multicast packets.
 *
 * Returns: 0 on success.
 *
**/
int rtp_session_set_multicast_loopback(RtpSession *session, int yesno)
{
	int retval;

	// Store new loopback state if one is specified
	if (yesno==0) {
		// Don't loop back
		session->multicast_loopback = 0;
	} else if (yesno>0) {
		// Do loop back
		session->multicast_loopback = 1;
	}

	// Don't do anything if socket hasn't been created yet
	if (session->rtp.socket == (ortp_socket_t)-1) return 0;

	switch (session->rtp.sockfamily) {
		case AF_INET: {

			retval= setsockopt(session->rtp.socket, IPPROTO_IP, IP_MULTICAST_LOOP,
						 (SOCKET_OPTION_VALUE)   &session->multicast_loopback, sizeof(session->multicast_loopback));

			if (retval<0) break;

			retval= setsockopt(session->rtcp.socket, IPPROTO_IP, IP_MULTICAST_LOOP,
						 (SOCKET_OPTION_VALUE)   &session->multicast_loopback, sizeof(session->multicast_loopback));

		} break;
		case AF_INET6: {

			retval= setsockopt(session->rtp.socket, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
				 (SOCKET_OPTION_VALUE)	&session->multicast_loopback, sizeof(session->multicast_loopback));

			if (retval<0) break;

			retval= setsockopt(session->rtcp.socket, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
				 (SOCKET_OPTION_VALUE)	&session->multicast_loopback, sizeof(session->multicast_loopback));
		} break;
	default:
		retval=-1;
	}

	if (retval<0)
		ortp_warning("Failed to set multicast loopback on socket.");

	return retval;
}


/**
 *rtp_session_get_multicast_loopback:
 *@session: a rtp session
 *
 * Returns the multicast loopback state of rtp session (true or false).
 *
**/
int rtp_session_get_multicast_loopback(RtpSession *session)
{
	return session->multicast_loopback;
}

/**
 *rtp_session_set_dscp:
 *@session: a rtp session
 *@dscp: desired DSCP PHB value
 *
 * Sets the DSCP (Differentiated Services Code Point) for outgoing RTP packets.
 *
 * Returns: 0 on success.
 *
**/
int rtp_session_set_dscp(RtpSession *session, int dscp){
	int retval=0;
	int tos;
	int proto;
	int value_type;
#if (_WIN32_WINNT >= 0x0600)
	OSVERSIONINFOEX ovi;
#endif

	// Store new DSCP value if one is specified
	if (dscp>=0) session->dscp = dscp;

	// Don't do anything if socket hasn't been created yet
	if (session->rtp.socket == (ortp_socket_t)-1) return 0;

#if (_WIN32_WINNT >= 0x0600) && !defined(WINAPI_FAMILY_PHONE_APP)
	memset(&ovi, 0, sizeof(ovi));
	ovi.dwOSVersionInfoSize = sizeof(ovi);
	GetVersionEx((LPOSVERSIONINFO) & ovi);

	ortp_message("check OS support for qwave.lib: %i %i %i\n",
				ovi.dwMajorVersion, ovi.dwMinorVersion, ovi.dwBuildNumber);
	if (ovi.dwMajorVersion > 5) {

		if (FAILED(__HrLoadAllImportsForDll("qwave.dll"))) {
			ortp_warning("Failed to load qwave.dll: no QoS available\n" );
		}
		else
		{
			if (session->dscp==0)
				tos=QOSTrafficTypeBestEffort;
			else if (session->dscp==0x8)
				tos=QOSTrafficTypeBackground;
			else if (session->dscp==0x28)
				tos=QOSTrafficTypeAudioVideo;
			else if (session->dscp==0x38)
				tos=QOSTrafficTypeVoice;
			else
				tos=QOSTrafficTypeExcellentEffort; /* 0x28 */

			if (session->rtp.QoSHandle==NULL) {
				QOS_VERSION version;
				BOOL QoSResult;

				version.MajorVersion = 1;
				version.MinorVersion = 0;

				QoSResult = QOSCreateHandle(&version, &session->rtp.QoSHandle);

				if (QoSResult != TRUE){
					ortp_error("QOSCreateHandle failed to create handle with error %d\n",
						GetLastError());
					retval=-1;
				}
			}
			if (session->rtp.QoSHandle!=NULL) {
				BOOL QoSResult;
				QoSResult = QOSAddSocketToFlow(
					session->rtp.QoSHandle,
					session->rtp.socket,
					(struct sockaddr*)&session->rtp.rem_addr,
					tos,
					QOS_NON_ADAPTIVE_FLOW,
					&session->rtp.QoSFlowID);

				if (QoSResult != TRUE){
					ortp_error("QOSAddSocketToFlow failed to add a flow with error %d\n",
						GetLastError());
					retval=-1;
				}
			}
		}
	} else {
#endif
		// DSCP value is in the upper six bits of the TOS field
		tos = (session->dscp << 2) & 0xFC;
		switch (session->rtp.sockfamily) {
			case AF_INET:
				proto=IPPROTO_IP;
				value_type=IP_TOS;
			break;
		case AF_INET6:
			proto=IPPROTO_IPV6;
#	ifdef IPV6_TCLASS /*seems not defined by my libc*/
			value_type=IPV6_TCLASS;
#	else
			value_type=IP_TOS;
#	endif
			break;
		default:
			ortp_error("Cannot set DSCP because socket family is unspecified.");
			return -1;
		}
		retval = setsockopt(session->rtp.socket, proto, value_type, (SOCKET_OPTION_VALUE)&tos, sizeof(tos));
		if (retval==-1)
			ortp_error("Fail to set DSCP value on rtp socket: %s",getSocketError());
		if (session->rtcp.socket != (ortp_socket_t)-1){
			if (setsockopt(session->rtcp.socket, proto, value_type, (SOCKET_OPTION_VALUE)&tos, sizeof(tos))==-1){
				ortp_error("Fail to set DSCP value on rtcp socket: %s",getSocketError());
			}
		}
#if (_WIN32_WINNT >= 0x0600) && !defined(WINAPI_FAMILY_PHONE_APP)
	}
#endif
	return retval;
}


/**
 *rtp_session_get_dscp:
 *@session: a rtp session
 *
 * Returns the DSCP (Differentiated Services Code Point) for outgoing RTP packets.
 *
**/
int rtp_session_get_dscp(const RtpSession *session)
{
	return session->dscp;
}


/**
 *rtp_session_get_local_port:
 *@session:	a rtp session for which rtp_session_set_local_addr() or rtp_session_set_remote_addr() has been called
 *
 *	This function can be useful to retrieve the local port that was randomly choosen by
 *	rtp_session_set_remote_addr() when rtp_session_set_local_addr() was not called.
 *
 *	Returns: the local port used to listen for rtp packets, -1 if not set.
**/

int rtp_session_get_local_port(const RtpSession *session){
	return (session->rtp.loc_port>0) ? session->rtp.loc_port : -1;
}

int rtp_session_get_local_rtcp_port(const RtpSession *session){
	return (session->rtcp.loc_port>0) ? session->rtcp.loc_port : -1;
}


static char * ortp_inet_ntoa(struct sockaddr *addr, int addrlen, char *dest, int destlen){
	int err;
	dest[0]=0;
	err=getnameinfo(addr,addrlen,dest,destlen,NULL,0,NI_NUMERICHOST);
	if (err!=0){
		ortp_warning("getnameinfo error: %s",gai_strerror(err));
	}
	return dest;
}

/**
 *rtp_session_set_remote_addr:
 *@session:		a rtp session freshly created.
 *@addr:		a local IP address in the xxx.xxx.xxx.xxx form.
 *@port:		a local port.
 *
 *	Sets the remote address of the rtp session, ie the destination address where rtp packet
 *	are sent. If the session is recv-only or duplex, it also sets the origin of incoming RTP
 *	packets. Rtp packets that don't come from addr:port are discarded.
 *
 *	Returns: 0 on success.
**/
int
rtp_session_set_remote_addr (RtpSession * session, const char * addr, int port){
	return rtp_session_set_remote_addr_full (session, addr, port, addr, port+1);
}

/**
 *rtp_session_set_remote_addr_full:
 *@session:		a rtp session freshly created.
 *@rtp_addr:		a local IP address in the xxx.xxx.xxx.xxx form.
 *@rtp_port:		a local rtp port.
 *@rtcp_addr:		a local IP address in the xxx.xxx.xxx.xxx form.
 *@rtcp_port:		a local rtcp port.
 *
 *	Sets the remote address of the rtp session, ie the destination address where rtp packet
 *	are sent. If the session is recv-only or duplex, it also sets the origin of incoming RTP
 *	packets. Rtp packets that don't come from addr:port are discarded.
 *
 *	Returns: 0 on success.
**/

int
rtp_session_set_remote_addr_full (RtpSession * session, const char * rtp_addr, int rtp_port, const char * rtcp_addr, int rtcp_port)
{
	int err;
	struct addrinfo hints, *res0, *res;
	char num[8];

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = (session->rtp.socket == -1) ? AF_UNSPEC : session->rtp.sockfamily;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = hints.ai_family==AF_INET6 ? AI_V4MAPPED : 0;

	snprintf(num, sizeof(num), "%d", rtp_port);
	err = getaddrinfo(rtp_addr, num, &hints, &res0);
	if (err) {
		ortp_warning ("Error in socket address: %s", gai_strerror(err));
		return -1;
	}
	if (session->rtp.socket == -1){
		/* the session has not its socket bound, do it */
		ortp_message ("Setting random local addresses.");
		/* bind to an address type that matches the destination address */
		if (res0->ai_addr->sa_family==AF_INET6)
			err = rtp_session_set_local_addr (session, "::", -1, -1);
		else err=rtp_session_set_local_addr (session, "0.0.0.0", -1, -1);
		if (err<0) return -1;
	}


	err=1;
	for (res = res0; res; res = res->ai_next) {
		/* set a destination address that has the same type as the local address */
		if (res->ai_family==session->rtp.sockfamily ) {
			memcpy( &session->rtp.rem_addr, res->ai_addr, res->ai_addrlen);
			session->rtp.rem_addrlen=res->ai_addrlen;
			err=0;
			break;
		}
	}
	freeaddrinfo(res0);
	if (err) {
		ortp_warning("Could not set destination for RTP socket to %s:%i.",rtp_addr,rtp_port);
		return -1;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = (session->rtp.socket == -1) ? AF_UNSPEC : session->rtp.sockfamily;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = hints.ai_family==AF_INET6 ? AI_V4MAPPED : 0;
	snprintf(num, sizeof(num), "%d", rtcp_port);
	err = getaddrinfo(rtcp_addr, num, &hints, &res0);
	if (err) {
		ortp_warning ("Error: %s", gai_strerror(err));
		return err;
	}
	err=1;
	for (res = res0; res; res = res->ai_next) {
		/* set a destination address that has the same type as the local address */
		if (res->ai_family==session->rtp.sockfamily ) {
			err=0;
			memcpy( &session->rtcp.rem_addr, res->ai_addr, res->ai_addrlen);
			session->rtcp.rem_addrlen=res->ai_addrlen;
			break;
		}
	}
	freeaddrinfo(res0);
	if (err) {
		ortp_warning("Could not set destination for RCTP socket to %s:%i.",rtcp_addr,rtcp_port);
		return -1;
	}

	if (can_connect(session)){
		if (try_connect(session->rtp.socket,(struct sockaddr*)&session->rtp.rem_addr,session->rtp.rem_addrlen))
			session->flags|=RTP_SOCKET_CONNECTED;
		if (session->rtcp.socket!=(ortp_socket_t)-1){
			if (try_connect(session->rtcp.socket,(struct sockaddr*)&session->rtcp.rem_addr,session->rtcp.rem_addrlen))
				session->flags|=RTCP_SOCKET_CONNECTED;
		}
	}else if (session->flags & RTP_SOCKET_CONNECTED){
		/*must dissolve association done by connect().
		See connect(2) manpage*/
		struct sockaddr sa;
		sa.sa_family=AF_UNSPEC;
		if (connect(session->rtp.socket,&sa,sizeof(sa))<0){
			ortp_error("Cannot dissolve connect() association for rtp socket: %s", getSocketError());
		}
		if (connect(session->rtcp.socket,&sa,sizeof(sa))<0){
			ortp_error("Cannot dissolve connect() association for rtcp socket: %s", getSocketError());
		}
		session->flags&=~RTP_SOCKET_CONNECTED;
		session->flags&=~RTCP_SOCKET_CONNECTED;
	}
	ortp_message("rtp session [%p] set to rtp [%s:%i] rtcp [%s:%i]"	,session
																	,rtp_addr
																	,rtp_port
																	,rtcp_addr
																	,rtcp_port);
	return 0;
}

int rtp_session_set_remote_addr_and_port(RtpSession * session, const char * addr, int rtp_port, int rtcp_port){
	return rtp_session_set_remote_addr_full(session,addr,rtp_port,addr,rtcp_port);
}

void rtp_session_set_sockets(RtpSession *session, int rtpfd, int rtcpfd)
{
	if (rtpfd!=-1) set_non_blocking_socket(rtpfd);
	if (rtcpfd!=-1) set_non_blocking_socket(rtcpfd);
	session->rtp.socket=rtpfd;
	session->rtcp.socket=rtcpfd;
	if (rtpfd!=-1 || rtcpfd!=-1 )
		session->flags|=(RTP_SESSION_USING_EXT_SOCKETS|RTP_SOCKET_CONNECTED|RTCP_SOCKET_CONNECTED);
	else session->flags&=~(RTP_SESSION_USING_EXT_SOCKETS|RTP_SOCKET_CONNECTED|RTCP_SOCKET_CONNECTED);
}

void rtp_session_set_transports(RtpSession *session, struct _RtpTransport *rtptr, struct _RtpTransport *rtcptr)
{
	session->rtp.tr = rtptr;
	session->rtcp.tr = rtcptr;
	if (rtptr)
		rtptr->session=session;
	if (rtcptr)
		rtcptr->session=session;

	if (rtptr || rtcptr )
		session->flags|=(RTP_SESSION_USING_TRANSPORT);
	else session->flags&=~(RTP_SESSION_USING_TRANSPORT);
}

void rtp_session_get_transports(RtpSession *session, RtpTransport **rtptr, RtpTransport **rtcptr){
	if (rtptr) *rtptr=session->rtp.tr;
	if (rtcptr) *rtcptr=session->rtcp.tr;
}


/**
 *rtp_session_flush_sockets:
 *@session: a rtp session
 *
 * Flushes the sockets for all pending incoming packets.
 * This can be usefull if you did not listen to the stream for a while
 * and wishes to start to receive again. During the time no receive is made
 * packets get bufferised into the internal kernel socket structure.
 *
**/
void rtp_session_flush_sockets(RtpSession *session){
	unsigned char trash[4096];
	struct sockaddr_storage from;

	socklen_t fromlen=sizeof(from);
	if (rtp_session_using_transport(session, rtp))
		{
		mblk_t *trashmp=esballoc(trash,sizeof(trash),0,NULL);

		while (session->rtp.tr->t_recvfrom(session->rtp.tr,trashmp,0,(struct sockaddr *)&from,&fromlen)>0){};

		if (session->rtcp.tr)
			while (session->rtcp.tr->t_recvfrom(session->rtcp.tr,trashmp,0,(struct sockaddr *)&from,&fromlen)>0){};
			freemsg(trashmp);
			return;
		}

	if (session->rtp.socket!=(ortp_socket_t)-1){
		while (recvfrom(session->rtp.socket,(char*)trash,sizeof(trash),0,(struct sockaddr *)&from,&fromlen)>0){};
	}
	if (session->rtcp.socket!=(ortp_socket_t)-1){
		while (recvfrom(session->rtcp.socket,(char*)trash,sizeof(trash),0,(struct sockaddr*)&from,&fromlen)>0){};
	}
}


#ifdef USE_SENDMSG
#define MAX_IOV 30
static int rtp_sendmsg(int sock,mblk_t *m, struct sockaddr *rem_addr, int addr_len){
	int error;
	struct msghdr msg;
	struct iovec iov[MAX_IOV];
	int iovlen;
	for(iovlen=0; iovlen<MAX_IOV && m!=NULL; m=m->b_cont,iovlen++){
		iov[iovlen].iov_base=m->b_rptr;
		iov[iovlen].iov_len=m->b_wptr-m->b_rptr;
	}
	if (iovlen==MAX_IOV){
		ortp_error("Too long msgb, didn't fit into iov, end discarded.");
	}
	msg.msg_name=(void*)rem_addr;
	msg.msg_namelen=addr_len;
	msg.msg_iov=&iov[0];
	msg.msg_iovlen=iovlen;
	msg.msg_control=NULL;
	msg.msg_controllen=0;
	msg.msg_flags=0;
	error=sendmsg(sock,&msg,0);
	return error;
}
#endif

#define IP_UDP_OVERHEAD (20+8)
#define IP6_UDP_OVERHEAD (40+8)

static bool_t is_ipv6(RtpSession *s){
	if (s->rtp.sockfamily==AF_INET6){
		struct sockaddr_in6 *in6=(struct sockaddr_in6*)&s->rtp.rem_addr;
		return !IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr);
	}
	return FALSE;
}

static void update_sent_bytes(RtpSession*s, int nbytes){
	int overhead=is_ipv6(s) ? IP6_UDP_OVERHEAD : IP_UDP_OVERHEAD;
	if (s->rtp.sent_bytes==0){
		ortp_gettimeofday(&s->rtp.send_bw_start,NULL);
	}
	s->rtp.sent_bytes+=nbytes+overhead;
}

static void update_recv_bytes(RtpSession*s, int nbytes){
	int overhead=is_ipv6(s) ? IP6_UDP_OVERHEAD : IP_UDP_OVERHEAD;

	if (s->rtp.recv_bytes==0){
		ortp_gettimeofday(&s->rtp.recv_bw_start,NULL);
	}
	s->rtp.recv_bytes+=nbytes+overhead;
}

int
rtp_session_rtp_send (RtpSession * session, mblk_t * m)
{
	int error;
	int i;
	rtp_header_t *hdr;
	struct sockaddr *destaddr=(struct sockaddr*)&session->rtp.rem_addr;
	socklen_t destlen=session->rtp.rem_addrlen;
	ortp_socket_t sockfd=session->rtp.socket;

	hdr = (rtp_header_t *) m->b_rptr;
	if (hdr->version == 0) {
		/* We are probably trying to send a STUN packet so don't change its content. */
	} else {
		/* perform host to network conversions */
		hdr->ssrc = htonl (hdr->ssrc);
		hdr->timestamp = htonl (hdr->timestamp);
		hdr->seq_number = htons (hdr->seq_number);
		for (i = 0; i < hdr->cc; i++)
			hdr->csrc[i] = htonl (hdr->csrc[i]);
	}

	if (session->flags & RTP_SOCKET_CONNECTED) {
		destaddr=NULL;
		destlen=0;
	}

	if (rtp_session_using_transport(session, rtp)){
		error = (session->rtp.tr->t_sendto) (session->rtp.tr,m,0,destaddr,destlen);
	}else{
#ifdef USE_SENDMSG
		error=rtp_sendmsg(sockfd,m,destaddr,destlen);
#else
		if (m->b_cont!=NULL)
			msgpullup(m,-1);
		error = sendto (sockfd, (char*)m->b_rptr, (int) (m->b_wptr - m->b_rptr),
			 0,destaddr,destlen);
#endif
	}
	if (error < 0){
		if (session->on_network_error.count>0){
			rtp_signal_table_emit3(&session->on_network_error,(long)"Error sending RTP packet",INT_TO_POINTER(getSocketErrorCode()));
		}else ortp_warning ("[%p] Error sending rtp packet: %s ; socket=%i", session, getSocketError(), sockfd);
		session->rtp.send_errno=getSocketErrorCode();
	}else{
		update_sent_bytes(session,error);
	}
	freemsg (m);
	return error;
}

int
rtp_session_rtcp_send (RtpSession * session, mblk_t * m)
{
	int error=0;
	ortp_socket_t sockfd=session->rtcp.socket;
	struct sockaddr *destaddr=(struct sockaddr*)&session->rtcp.rem_addr;
	socklen_t destlen=session->rtcp.rem_addrlen;
	bool_t using_connected_socket=(session->flags & RTCP_SOCKET_CONNECTED)!=0;

	if (using_connected_socket) {
		destaddr=NULL;
		destlen=0;
	}

	if (session->rtcp.enabled &&
		( (sockfd!=(ortp_socket_t)-1 && (session->rtcp.rem_addrlen>0 ||using_connected_socket))
			|| rtp_session_using_transport(session, rtcp) ) ){
		if (rtp_session_using_transport(session, rtcp)){
			error = (session->rtcp.tr->t_sendto) (session->rtcp.tr, m, 0,
			destaddr, destlen);
		}
		else{
#ifdef USE_SENDMSG
			error=rtp_sendmsg(sockfd,m,destaddr, destlen);
#else
			if (m->b_cont!=NULL){
				msgpullup(m,-1);
			}
			error = sendto (sockfd, (char*)m->b_rptr,
			(int) (m->b_wptr - m->b_rptr), 0,
			destaddr, destlen);
#endif
		}
		if (error < 0){
			char host[65];
			if (session->on_network_error.count>0){
				rtp_signal_table_emit3(&session->on_network_error,(long)"Error sending RTCP packet",INT_TO_POINTER(getSocketErrorCode()));
			}else{
				ortp_warning ("[%p] Error sending rtcp packet %p: %s ; socket=%i; addr=%s", session, m, getSocketError(), session->rtcp.socket, ortp_inet_ntoa((struct sockaddr*)&session->rtcp.rem_addr,session->rtcp.rem_addrlen,host,sizeof(host)) );
			}
		}
	}else ortp_message("Not sending rtcp report: sockfd=%i, rem_addrlen=%i, connected=%i",sockfd,session->rtcp.rem_addrlen,using_connected_socket);
	freemsg (m);
	return error;
}

int rtp_session_rtp_recv_abstract(ortp_socket_t socket, mblk_t *msg, int flags, struct sockaddr *from, socklen_t *fromlen) {
	int ret;
	int bufsz = (int) (msg->b_datap->db_lim - msg->b_datap->db_base);
#ifndef _WIN32
	struct iovec   iov;
	struct msghdr  msghdr;
	struct cmsghdr *cmsghdr;
	struct {
			struct cmsghdr cm;
			char control[512];
		} control;
	memset(&msghdr, 0, sizeof(msghdr));
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = msg->b_wptr;
	iov.iov_len  = bufsz;
	if(from != NULL && fromlen != NULL) {
		msghdr.msg_name = from;
		msghdr.msg_namelen = *fromlen;
	}
	msghdr.msg_iov     = &iov;
	msghdr.msg_iovlen  = 1;
	msghdr.msg_control = &control;
	msghdr.msg_controllen = sizeof(control);
	ret = recvmsg(socket, &msghdr, flags);
	if(fromlen != NULL)
		*fromlen = msghdr.msg_namelen;
	if(ret >= 0) {
#else
	char control[512];
	WSAMSG msghdr;
	WSACMSGHDR *cmsghdr;
	WSABUF data_buf;
	DWORD bytes_received;

	if (ortp_WSARecvMsg == NULL) {
		return recvfrom(socket, (char *)msg->b_wptr, bufsz, flags, from, fromlen);
	}

	memset(&msghdr, 0, sizeof(msghdr));
	memset(control, 0, sizeof(control));
	if(from != NULL && fromlen != NULL) {
		msghdr.name = from;
		msghdr.namelen = *fromlen;
	}
	data_buf.buf = (char *)msg->b_wptr;
	data_buf.len = bufsz;
	msghdr.lpBuffers = &data_buf;
	msghdr.dwBufferCount = 1;
	msghdr.Control.buf = control;
	msghdr.Control.len = sizeof(control);
	msghdr.dwFlags = flags;
	ret = ortp_WSARecvMsg(socket, &msghdr, &bytes_received, NULL, NULL);
	if(fromlen != NULL)
		*fromlen = msghdr.namelen;
	if(ret >= 0) {
		ret = bytes_received;
#endif
		for (cmsghdr = CMSG_FIRSTHDR(&msghdr); cmsghdr != NULL ; cmsghdr = CMSG_NXTHDR(&msghdr, cmsghdr)) {
#if defined(ORTP_TIMESTAMP)
			if (cmsghdr->cmsg_level == SOL_SOCKET && cmsghdr->cmsg_type == SO_TIMESTAMP) {
				memcpy(&msg->timestamp, (struct timeval *)CMSG_DATA(cmsghdr), sizeof(struct timeval));
			}
#endif
#ifdef IP_PKTINFO
			if ((cmsghdr->cmsg_level == IPPROTO_IP) && (cmsghdr->cmsg_type == IP_PKTINFO)) {
				struct in_pktinfo *pi = (struct in_pktinfo *)CMSG_DATA(cmsghdr);
				memcpy(&msg->recv_addr.addr.ipi_addr, &pi->ipi_addr, sizeof(msg->recv_addr.addr.ipi_addr));
				msg->recv_addr.family = AF_INET;
			}
#endif
#ifdef IPV6_PKTINFO
			if ((cmsghdr->cmsg_level == IPPROTO_IPV6) && (cmsghdr->cmsg_type == IPV6_PKTINFO)) {
				struct in6_pktinfo *pi = (struct in6_pktinfo *)CMSG_DATA(cmsghdr);
				memcpy(&msg->recv_addr.addr.ipi6_addr, &pi->ipi6_addr, sizeof(msg->recv_addr.addr.ipi6_addr));
				msg->recv_addr.family = AF_INET6;
			}
#endif
#ifdef IP_RECVDSTADDR
			if ((cmsghdr->cmsg_level == IPPROTO_IP) && (cmsghdr->cmsg_type == IP_RECVDSTADDR)) {
				struct in_addr *ia = (struct in_addr *)CMSG_DATA(cmsghdr);
				memcpy(&msg->recv_addr.addr.ipi_addr, ia, sizeof(msg->recv_addr.addr.ipi_addr));
				msg->recv_addr.family = AF_INET;
			}
#endif
#ifdef IPV6_RECVDSTADDR
			if ((cmsghdr->cmsg_level == IPPROTO_IPV6) && (cmsghdr->cmsg_type == IPV6_RECVDSTADDR)) {
				struct in6_addr *ia = (struct in6_addr *)CMSG_DATA(cmsghdr);
				memcpy(&msg->recv_addr.addr.ipi6_addr, ia, sizeof(msg->recv_addr.addr.ipi6_addr));
				msg->recv_addr.family = AF_INET6;
			}
#endif
#ifdef IP_RECVTTL
			if ((cmsghdr->cmsg_level == IPPROTO_IP) && (cmsghdr->cmsg_type == IP_TTL)) {
				uint32_t *ptr = (uint32_t *)CMSG_DATA(cmsghdr);
				msg->ttl_or_hl = (*ptr & 0xFF);
			}
#endif
#ifdef IPV6_RECVHOPLIMIT
			if ((cmsghdr->cmsg_level == IPPROTO_IPV6) && (cmsghdr->cmsg_type == IPV6_HOPLIMIT)) {
				uint32_t *ptr = (uint32_t *)CMSG_DATA(cmsghdr);
				msg->ttl_or_hl = (*ptr & 0xFF);
			}
#endif
		}
	}
	return ret;
}

void rtp_session_notify_inc_rtcp(RtpSession *session, mblk_t *m){
	if (session->eventqs!=NULL){
		OrtpEvent *ev=ortp_event_new(ORTP_EVENT_RTCP_PACKET_RECEIVED);
		OrtpEventData *d=ortp_event_get_data(ev);
		d->packet=m;
		rtp_session_dispatch_event(session,ev);
	}
	else freemsg(m);  /* avoid memory leak */
}

static void compute_rtt(RtpSession *session, const struct timeval *now, uint32_t lrr, uint32_t dlrr){
	uint64_t curntp=ortp_timeval_to_ntp(now);
	uint32_t approx_ntp=(curntp>>16) & 0xFFFFFFFF;
	/*ortp_message("rtt approx_ntp=%u, lrr=%u, dlrr=%u",approx_ntp,lrr,dlrr);*/
	if (lrr!=0 && dlrr!=0){
		double rtt_frac=approx_ntp-lrr-dlrr;
		if (rtt_frac>=0){
			rtt_frac/=65536.0;

			session->rtt=rtt_frac;
			/*ortp_message("rtt estimated to %f s",session->rtt);*/
		}else ortp_warning("Negative RTT computation, maybe due to clock adjustments.");
	}
}

static void compute_rtt_from_report_block(RtpSession *session, const struct timeval *now, const report_block_t *rb) {
	uint32_t last_sr_time = report_block_get_last_SR_time(rb);
	uint32_t sr_delay = report_block_get_last_SR_delay(rb);
	compute_rtt(session, now, last_sr_time, sr_delay);
}

static void compute_rtcp_xr_statistics(RtpSession *session, mblk_t *block, const struct timeval *now) {
	uint64_t ntp_timestamp;
	OrtpRtcpXrStats *stats = &session->rtcp_xr_stats;

	switch (rtcp_XR_get_block_type(block)) {
		case RTCP_XR_RCVR_RTT:
			ntp_timestamp = rtcp_XR_rcvr_rtt_get_ntp_timestamp(block);
			stats->last_rcvr_rtt_ts = (ntp_timestamp >> 16) & 0xffffffff;
			stats->last_rcvr_rtt_time.tv_sec = now->tv_sec;
			stats->last_rcvr_rtt_time.tv_usec = now->tv_usec;
			break;
		case RTCP_XR_DLRR:
			compute_rtt(session, now, rtcp_XR_dlrr_get_lrr(block), rtcp_XR_dlrr_get_dlrr(block));
			break;
		default:
			break;
	}
}

/*
 * @brief : for SR packets, retrieves their timestamp, gets the date, and stores these information into the session descriptor. The date values may be used for setting some fields of the report block of the next RTCP packet to be sent.
 * @param session : the current session descriptor.
 * @param block : the block descriptor that may contain a SR RTCP message.
 * @return -1 if we detect that the packet is in fact a STUN packet, otherwise 0.
 * @note a basic parsing is done on the block structure. However, if it fails, no error is returned, and the session descriptor is left as is, so it does not induce any change in the caller procedure behaviour.
 */
static int process_rtcp_packet( RtpSession *session, mblk_t *block, struct sockaddr *addr, socklen_t addrlen ) {
	rtcp_common_header_t *rtcp;
	RtpStream * rtpstream = &session->rtp;

	int msgsize = (int) ( block->b_wptr - block->b_rptr );
	if ( msgsize < RTCP_COMMON_HEADER_SIZE ) {
		ortp_debug( "Receiving a too short RTCP packet" );
		return 0;
	}

	rtcp = (rtcp_common_header_t *)block->b_rptr;

	if (rtcp->version != 2)
	{
		/* try to see if it is a STUN packet */
		uint16_t stunlen = *((uint16_t *)(block->b_rptr + sizeof(uint16_t)));
		stunlen = ntohs(stunlen);
		if (stunlen + 20 == block->b_wptr - block->b_rptr) {
			/* this looks like a stun packet */
			if (session->eventqs != NULL) {
				OrtpEvent *ev = ortp_event_new(ORTP_EVENT_STUN_PACKET_RECEIVED);
				OrtpEventData *ed = ortp_event_get_data(ev);
				ed->packet = block;
				ed->source_addrlen=addrlen;
				memcpy(&ed->source_addr,addr,addrlen);
				ed->info.socket_type = OrtpRTCPSocket;
				rtp_session_dispatch_event(session, ev);
				return -1;
			}
		}
		/* discard in two case: the packet is not stun OR nobody is interested by STUN (no eventqs) */
		ortp_debug("Receiving rtcp packet with version number !=2...discarded");
		return 0;
	}
	/* compound rtcp packet can be composed by more than one rtcp message */
	do{
		struct timeval reception_date;
		const report_block_t *rb;

		/* Getting the reception date from the main clock */
		ortp_gettimeofday( &reception_date, NULL );

		if (rtcp_is_SR(block) ) {
			rtcp_sr_t *sr = (rtcp_sr_t *) rtcp;

			/* The session descriptor values are reset in case there is an error in the SR block parsing */
			rtpstream->last_rcv_SR_ts = 0;
			rtpstream->last_rcv_SR_time.tv_usec = 0;
			rtpstream->last_rcv_SR_time.tv_sec = 0;

			if ( ntohl( sr->ssrc ) != session->rcv.ssrc ) {
				ortp_debug( "Receiving a RTCP SR packet from an unknown ssrc" );
				return 0;
			}

			if ( msgsize < RTCP_COMMON_HEADER_SIZE + RTCP_SSRC_FIELD_SIZE + RTCP_SENDER_INFO_SIZE + ( RTCP_REPORT_BLOCK_SIZE * sr->ch.rc ) ) {
				ortp_debug( "Receiving a too short RTCP SR packet" );
				return 0;
			}

			/* Saving the data to fill LSR and DLSR field in next RTCP report to be transmitted */
			/* This value will be the LSR field of the next RTCP report (only the central 32 bits are kept, as described in par.4 of RC3550) */
			rtpstream->last_rcv_SR_ts = ( ntohl( sr->si.ntp_timestamp_msw ) << 16 ) | ( ntohl( sr->si.ntp_timestamp_lsw ) >> 16 );
			/* This value will help in processing the DLSR of the next RTCP report ( see report_block_init() in rtcp.cc ) */
			rtpstream->last_rcv_SR_time.tv_usec = reception_date.tv_usec;
			rtpstream->last_rcv_SR_time.tv_sec = reception_date.tv_sec;
			rb=rtcp_SR_get_report_block(block,0);
			if (rb) compute_rtt_from_report_block(session,&reception_date,rb);
		}else if ( rtcp_is_RR(block)){
			rb=rtcp_RR_get_report_block(block,0);
			if (rb) compute_rtt_from_report_block(session,&reception_date,rb);
		} else if (rtcp_is_XR(block)) {
			compute_rtcp_xr_statistics(session, block, &reception_date);
		}
	}while (rtcp_next_packet(block));
	rtcp_rewind(block);
	return 0;
}

static void reply_to_collaborative_rtcp_xr_packet(RtpSession *session, mblk_t *block) {
	if (rtcp_is_XR(block) && (rtcp_XR_get_block_type(block) == RTCP_XR_RCVR_RTT)) {
		rtp_session_send_rtcp_xr_dlrr(session);
	}
}

static void rtp_process_incoming_packet(RtpSession * session, mblk_t * mp, bool_t is_rtp_packet,uint32_t user_ts) {
	bool_t sock_connected=(is_rtp_packet && !!(session->flags & RTP_SOCKET_CONNECTED))
		|| (!is_rtp_packet && !!(session->flags & RTCP_SOCKET_CONNECTED));

	struct sockaddr *remaddr = NULL;
	socklen_t addrlen;

	if (!mp){
		return;
	}
	remaddr = (struct sockaddr *)&mp->src_addr;
	addrlen = mp->src_addrlen;

	if (is_rtp_packet){
		/* then parse the message and put on jitter buffer queue */
		update_recv_bytes(session,mp->b_wptr-mp->b_rptr);
		rtp_session_rtp_parse(session, mp, user_ts, remaddr,addrlen);
		/*for bandwidth measurements:*/
	}else {
		if (process_rtcp_packet(session, mp, remaddr, addrlen) >= 0){
			/* post an event to notify the application*/
			rtp_session_notify_inc_rtcp(session,mp);
			/* reply to collaborative RTCP XR packets if needed. */
			if (session->rtcp.xr_conf.enabled == TRUE){
				reply_to_collaborative_rtcp_xr_packet(session, mp);
			}
		}
		if (session->symmetric_rtp && !sock_connected){
			/* store the sender RTP address to do symmetric RTP */
			memcpy(&session->rtcp.rem_addr,remaddr,addrlen);
			session->rtcp.rem_addrlen=addrlen;
			if (session->use_connect){
				if (try_connect(session->rtcp.socket,remaddr,addrlen))
					session->flags|=RTCP_SOCKET_CONNECTED;
			}
		}
	}
}

int rtp_session_rtp_recv (RtpSession * session, uint32_t user_ts) {
	int error;
	ortp_socket_t sockfd=session->rtp.socket;
	struct sockaddr_storage remaddr;

	socklen_t addrlen = sizeof (remaddr);
	mblk_t *mp;

	if ((sockfd==(ortp_socket_t)-1) && !rtp_session_using_transport(session, rtp)) return -1;  /*session has no sockets for the moment*/

	while (1)
	{
		bool_t sock_connected=!!(session->flags & RTP_SOCKET_CONNECTED);
		bool_t is_rtp_packet = TRUE;

		if (session->rtp.cached_mp==NULL){
			 session->rtp.cached_mp = msgb_allocator_alloc(&session->allocator,session->recv_buf_size);
		}
		mp=session->rtp.cached_mp;
		if (sock_connected){
			error=rtp_session_rtp_recv_abstract(sockfd, mp, 0, NULL, NULL);
		}else if (rtp_session_using_transport(session, rtp)) {
			error = (session->rtp.tr->t_recvfrom)(session->rtp.tr, mp, 0,
				  (struct sockaddr *) &remaddr,
				  &addrlen);
		} else { error = rtp_session_rtp_recv_abstract(sockfd, mp, 0,
				  (struct sockaddr *) &remaddr,
				  &addrlen);
		}
		if (error > 0){
			if (session->use_connect){
				/* In the case where use_connect is false, symmetric RTP is handled in rtp_session_rtp_parse() */
				if (session->symmetric_rtp && !sock_connected){
					/* store the sender RTP address to do symmetric RTP */
					memcpy(&session->rtp.rem_addr,&remaddr,addrlen);
					session->rtp.rem_addrlen=addrlen;
					if (try_connect(sockfd,(struct sockaddr*)&remaddr,addrlen))
						session->flags|=RTP_SOCKET_CONNECTED;
				}
			}
			mp->b_wptr+=error;

			/*if we use the network simulator, store packet source address for later use(otherwise it will be used immediately)*/
			memcpy(&mp->src_addr,&remaddr,addrlen);
			mp->src_addrlen = addrlen;
			if (session->net_sim_ctx){
				mp=rtp_session_network_simulate(session,mp,&is_rtp_packet);
			}
			rtp_process_incoming_packet(session,mp,is_rtp_packet,user_ts);

			session->rtp.cached_mp=NULL;
		}
		else
		{
			int errnum;
			if (error==-1 && !is_would_block_error((errnum=getSocketErrorCode())) )
			{
				if (session->on_network_error.count>0){
					rtp_signal_table_emit3(&session->on_network_error,(long)"Error receiving RTP packet",INT_TO_POINTER(getSocketErrorCode()));
				}else ortp_warning("Error receiving RTP packet: %s, err num  [%i],error [%i]",getSocketError(),errnum,error);
#ifdef __ios
				/*hack for iOS and non-working socket because of background mode*/
				if (errnum==ENOTCONN){
					/*re-create new sockets */
					rtp_session_set_local_addr(session,session->rtp.sockfamily==AF_INET ? "0.0.0.0" : "::0",session->rtp.loc_port,session->rtcp.loc_port);
				}
#endif
			}else{
				/*EWOULDBLOCK errors or transports returning 0 are ignored.*/
				if (session->net_sim_ctx){
					/*drain possible packets queued in the network simulator*/
					mp=rtp_session_network_simulate(session,NULL,&is_rtp_packet);
					rtp_process_incoming_packet(session,mp,is_rtp_packet,user_ts);
				}
			}
			/* don't free the cached_mp, it will be reused next time */
			return -1;
		}
	}
	return error;
}

int rtp_session_rtcp_recv (RtpSession * session) {
	int error;
	struct sockaddr_storage remaddr;

	socklen_t addrlen=0;
	mblk_t *mp;

	if (session->rtcp.socket==(ortp_socket_t)-1 && !rtp_session_using_transport(session, rtcp)) return -1;  /*session has no RTCP sockets for the moment*/


	while (1)
	{
		bool_t sock_connected=!!(session->flags & RTCP_SOCKET_CONNECTED);
		bool_t is_rtp_packet = FALSE;
		if (session->rtcp.cached_mp==NULL){
			 session->rtcp.cached_mp = allocb (RTCP_MAX_RECV_BUFSIZE, 0);
		}

		mp=session->rtcp.cached_mp;
		if (sock_connected){
			error=rtp_session_rtp_recv_abstract(session->rtcp.socket, mp, 0, NULL, NULL);
		}else {
			addrlen=sizeof (remaddr);

			if (rtp_session_using_transport(session, rtcp))
			  error=(session->rtcp.tr->t_recvfrom)(session->rtcp.tr, mp, 0,
				  (struct sockaddr *) &remaddr,
				  &addrlen);
			else
			  error=rtp_session_rtp_recv_abstract (session->rtcp.socket,mp, 0,
				  (struct sockaddr *) &remaddr,
				  &addrlen);
		}
		if (error > 0)
		{
			mp->b_wptr += error;

			memcpy(&mp->src_addr,&remaddr,addrlen);
			mp->src_addrlen = addrlen;

			if (session->net_sim_ctx)
				mp=rtp_session_network_simulate(session,mp,&is_rtp_packet);
			rtp_process_incoming_packet(session,mp,is_rtp_packet,session->rtp.rcv_last_app_ts);
			session->rtcp.cached_mp=NULL;
		}
		else
		{
			int errnum;
			if (error==-1 && !is_would_block_error((errnum=getSocketErrorCode())) )
			{
				if (session->on_network_error.count>0){
					rtp_signal_table_emit3(&session->on_network_error,(long)"Error receiving RTCP packet",INT_TO_POINTER(getSocketErrorCode()));
				}else ortp_warning("Error receiving RTCP packet: %s, err num  [%i],error [%i]",getSocketError(),errnum,error);
#ifdef __ios
				/*hack for iOS and non-working socket because of background mode*/
				if (errnum==ENOTCONN){
					/*re-create new sockets */
					rtp_session_set_local_addr(session,session->rtcp.sockfamily==AF_INET ? "0.0.0.0" : "::0",session->rtcp.loc_port,session->rtcp.loc_port);
				}
#endif
				session->rtp.recv_errno=errnum;
			}else{
				/*EWOULDBLOCK errors or transports returning 0 are ignored.*/
				if (session->net_sim_ctx){
					/*drain possible packets queued in the network simulator*/
					mp=rtp_session_network_simulate(session,NULL,&is_rtp_packet);
					rtp_process_incoming_packet(session,mp,is_rtp_packet,session->rtp.rcv_last_app_ts);
				}
			}
			/* don't free the cached_mp, it will be reused next time */
			return -1; /* avoids an infinite loop ! */
		}
	}
	return error;
}
