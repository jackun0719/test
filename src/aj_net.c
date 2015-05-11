/**
 * @file
 */
/******************************************************************************
* Copyright 2013, Qualcomm Connected Experiences, Inc.
*
******************************************************************************/
#define AJ_MODULE NET

#include "aj_target.h"
#include "aj_debug.h"
#include "aj_bufio.h"
#include "aj_net.h"
#include "aj_util.h"

#include <qcom/qcom_time.h>
#include <qcom/qcom_network.h>
#include <qcom/socket_api.h>
#include <qcom/select_api.h>
#include <qcom/qcom_wlan.h>

#ifndef NDEBUG
AJ_EXPORT uint8_t dbgNET = 0;
#endif

extern A_UINT8 qcom_DeviceId;

#define INVALID_SOCKET -1

/*
 * IANA assigned IPv4 multicast group for AllJoyn.
 */
static const char AJ_IPV4_MULTICAST_GROUP[] = "224.0.0.113";
/*
 * IANA assigned IPv4 multicast group for AllJoyn, in binary form
 */
#define AJ_IPV4_MCAST_GROUP     0xe0000071

/*
 * IANA assigned IPv6 multicast group for AllJoyn.
 */
//static const char AJ_IPV6_MULTICAST_GROUP[] = "ff02::13a";
static const uint8_t AJ_IPV6_MCAST_GROUP[16] = {0xFF, 0x02, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x01, 0x3A};

/*
 * IANA assigned UDP multicast port for AllJoyn
 */
#define AJ_UDP_PORT 9956

/**
 * Target-specific context for network I/O
 */
typedef struct {
    int tcpSock;
    int udpSock;
    int udp6Sock;
} NetContext;

static NetContext netContext;

/**
 *  control the linger behavior for sockets
 */
typedef struct {
    int l_onoff;
    int l_linger;
} AJ_Linger;

/*
 * Need enough RX buffer space to receive a complete name service packet when
 * used in UDP mode.  NS expects MTU of 1500 subtracts UDP, IP and Ethernet
 * Type II overhead.  1500 - 8 -20 - 18 = 1454.  txData buffer size needs to
 * be big enough to hold a NS WHO-HAS for one name (4 + 2 + 256 = 262) in UDP
 * mode.  TCP buffer size dominates in that case.
 */
static uint8_t rxData[1454];
static uint8_t txData[1452];


uint16_t AJ_EphemeralPort(void)
{
    uint8_t bytes[2];
    AJ_RandBytes(bytes, 2);
    /*
     * Return a random port number in the IANA-suggested range
     */
    return 49152 + *((uint16_t*)bytes) % (65535 - 49152);
}

static AJ_Status ConfigNetSock(AJ_NetSocket* netSock, AJ_RxFunc rxFunc, uint32_t rxLen, AJ_TxFunc txFunc, uint32_t txLen)
{

    memset(netSock, 0, sizeof(AJ_NetSocket));
    memset(&netContext, 0, sizeof(NetContext));

    if (rxLen) {
        if (rxLen > sizeof(rxData)) {
            AJ_ErrPrintf(("ConfigNetSock(): Asking for %u bytes but buffer is only %u", rxLen, sizeof(rxData)));
            return AJ_ERR_RESOURCES;
        }
    }
    AJ_IOBufInit(&netSock->rx, rxData, rxLen, AJ_IO_BUF_RX, (void*)&netContext);
    netSock->rx.recv = rxFunc;
    AJ_IOBufInit(&netSock->tx, txData, txLen, AJ_IO_BUF_TX, (void*)&netContext);
    netSock->tx.send = txFunc;
    return AJ_OK;
}

static AJ_Status CloseNetSock(AJ_NetSocket* netSock)
{
    NetContext* context = (NetContext*)netSock->rx.context;
    if (context) {
        if (context->tcpSock) {
            qcom_socket_close(context->tcpSock);
        }
        if (context->udpSock) {
            qcom_socket_close(context->udpSock);
        }
        if (context->udp6Sock) {
            qcom_socket_close(context->udp6Sock);
        }
        memset(context, 0, sizeof(NetContext));
        memset(netSock, 0, sizeof(AJ_NetSocket));
    }
    return AJ_OK;
}



AJ_Status AJ_Net_Send(AJ_IOBuffer* buf)
{
    NetContext* context = (NetContext*)buf->context;
    int ret;
    int tx = AJ_IO_BUF_AVAIL(buf);

    AJ_ASSERT(buf->direction == AJ_IO_BUF_TX);

    while (tx) {
        ret = qcom_send((int) context->tcpSock, (char*) buf->readPtr, tx, 0);
        if (ret < 0) {
            if( ret == -100 ) {
                // there weren't enough buffers, try again
                AJ_Sleep(100);
                continue;
            }

            AJ_ErrPrintf(("AJ_Net_Send: send() failed: %d\n", ret));
            return AJ_ERR_WRITE;
        }
        buf->readPtr += ret;
        tx -= ret;
    }
    if (AJ_IO_BUF_AVAIL(buf) == 0) {
        AJ_IO_BUF_RESET(buf);
    }
    return AJ_OK;
}

void AJ_EntropyBits(uint8_t bits);

AJ_Status AJ_Net_Recv(AJ_IOBuffer* buf, uint32_t len, uint32_t timeout)
{
    q_fd_set recvFds;
    NetContext* context = (NetContext*)buf->context;
    AJ_Status status = AJ_OK;
    size_t rx = AJ_IO_BUF_SPACE(buf);
    int rc = 0;
    int sockFd = (int) context->tcpSock;
    struct timeval tv = { timeout / 1000, 1000 * (timeout % 1000) };


    AJ_ASSERT(buf->direction == AJ_IO_BUF_RX);
    rx = min(rx, len);

    FD_ZERO(&recvFds);
    FD_SET(sockFd, &recvFds);
    rc = qcom_select(sockFd + 1, &recvFds, NULL, NULL, &tv);
    if (rc == 0) {
        return AJ_ERR_TIMEOUT;
    }

    AJ_EntropyBits((uint8_t)AJ_GetElapsedTime(NULL, 0));

    rx = min(rx, len);
    if (rx) {
        rc = qcom_recv(sockFd, (char*) buf->writePtr, rx, 0);
        if (rc <= 0) {
            AJ_ErrPrintf(("AJ_Net_Recv: recv() failed: %d\n", rc));
            status = AJ_ERR_READ;
        } else {
            buf->writePtr += rc;
        }
    }

    return status;
}


AJ_Status AJ_Net_Connect(AJ_NetSocket* netSock, uint16_t port, uint8_t addrType, const uint32_t* addr)
{
    AJ_Status status = AJ_ERR_CONNECT;
    int ret;
    struct sockaddr_storage addrBuf;
    int tcpSock = INVALID_SOCKET;
    int addrSize;
    AJ_Linger ling;

    memset(&addrBuf, 0, sizeof(addrBuf));

    if (addrType == AJ_ADDR_IPV4) {
        tcpSock = qcom_socket(AF_INET, SOCK_STREAM, 0);
    } else {
        tcpSock = qcom_socket(AF_INET6, SOCK_STREAM, 0);
    }

    if (tcpSock == INVALID_SOCKET) {
        return AJ_ERR_CONNECT;
    }

    // TODO: select until the socket is connected; use some kind of timeout
    // if POSIX-compliant, it will come back *writable* when it's connected

    if (addrType == AJ_ADDR_IPV4) {
        struct sockaddr_in* sa = (struct sockaddr_in*) &addrBuf;
        sa->sin_family = AF_INET;
        /*
         * These might appear to be mismatched, but addr is provided in network-byte order,
         * and port is in host-byte order (assignd in ParseIsAt).
         * The API expects them in network order.
         */
        sa->sin_port = htons(port);
        sa->sin_addr.s_addr = *addr;
        addrSize = sizeof(struct sockaddr_in);
    } else {
        struct sockaddr_in6* sa = (struct sockaddr_in6*) &addrBuf;
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(port);
        memcpy(sa->sin6_addr.addr, addr, sizeof(sa->sin6_addr.addr));
        addrSize = sizeof(struct sockaddr_in6);
    }

    ret = qcom_connect(tcpSock, (struct sockaddr*) &addrBuf, addrSize);
    if (ret < 0) {
        AJ_ErrPrintf(("qcom_connect() failed: %d\n", ret));
        qcom_socket_close(tcpSock);
        return AJ_ERR_CONNECT;
    } else {
        status = ConfigNetSock(netSock, AJ_Net_Recv, sizeof(rxData), AJ_Net_Send, sizeof(txData));
        if (status == AJ_OK) {
            ((NetContext*)netSock->rx.context)->tcpSock = tcpSock;
        }
    }

    /*
     *  linger behavior
     */
    ling.l_onoff = 1;
    ling.l_linger = 0;
    ret = qcom_setsockopt(tcpSock, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
    if (ret < 0) {
        AJ_WarnPrintf(("AJ_Net_Connect(): qcom_setsockopt(%d) failed %d\n", tcpSock, ret));
    }

    return status;
}

void AJ_Net_Disconnect(AJ_NetSocket* netSock)
{
    CloseNetSock(netSock);
}

static AJ_Status RecvFrom(AJ_IOBuffer* buf, uint32_t len, uint32_t timeout)
{
    q_fd_set recvfromFds;
    AJ_Status status = AJ_OK;
    NetContext* context = (NetContext*)buf->context;

    int rc;
    size_t rx = AJ_IO_BUF_SPACE(buf);
    int sockFd = (int) context->udpSock;
    int sock6Fd = (int) context->udp6Sock;
    struct timeval tv = { timeout / 1000, 1000 * (timeout % 1000) };
    int maxFd = INVALID_SOCKET;


    AJ_ASSERT(buf->direction == AJ_IO_BUF_RX);

    FD_ZERO(&recvfromFds);

    if (context->udpSock != INVALID_SOCKET) {
        FD_SET(context->udpSock, &recvfromFds);
        maxFd = context->udpSock;
    }

    if (context->udp6Sock != INVALID_SOCKET) {
        FD_SET(context->udp6Sock, &recvfromFds);
        if (context->udp6Sock > context->udpSock) {
            maxFd = context->udp6Sock;
        }
    }

    if (maxFd == INVALID_SOCKET) {
        AJ_ErrPrintf(("AJ_Net_RecvFrom: no valid sockets\n"));
        return AJ_ERR_READ;
    }

    rc = qcom_select(maxFd + 1, &recvfromFds, NULL, NULL, &tv);
    if (rc == 0) {
        AJ_InfoPrintf(("AJ_Net_RecvFrom(): select() timed out. status=AJ_ERR_TIMEOUT\n"));
        return AJ_ERR_TIMEOUT;
    } else if (rc < 0) {
        AJ_ErrPrintf(("AJ_Net_RecvFrom: select returned error AJ_ERR_READ\n"));
        return AJ_ERR_READ;
    }

    AJ_EntropyBits((uint8_t)AJ_GetElapsedTime(NULL, 0));

    // we need to read from whichever socket has data availble.
    // if both sockets are ready, read from both in order to
    // reset the state

    rx = AJ_IO_BUF_SPACE(buf);
    if (context->udp6Sock != INVALID_SOCKET && FD_ISSET(context->udp6Sock, &recvfromFds)) {
        rx = min(rx, len);
        if (rx) {
            struct sockaddr_in6 fromAddr6;
            int fromAddrSize  = sizeof(fromAddr6);
            memset(&fromAddr6, 0, sizeof (fromAddr6));
            rc = qcom_recvfrom(context->udp6Sock, (char*) buf->writePtr, rx, 0, (struct sockaddr*)&fromAddr6, &fromAddrSize);
            if (rc <= 0) {
                AJ_ErrPrintf(("AJ_Net_RecvFrom(): qcom_recvfrom() failed, status=AJ_ERR_READ\n"));
                status = AJ_ERR_READ;
            } else {
                buf->writePtr += rc;
                status = AJ_OK;
            }
        }
    }


    rx = AJ_IO_BUF_SPACE(buf);
    if (context->udpSock != INVALID_SOCKET && FD_ISSET(context->udpSock, &recvfromFds)) {
        rx = min(rx, len);
        if (rx) {
            struct sockaddr_in fromAddr;
            int fromAddrSize  = sizeof(fromAddr);
            memset(&fromAddr, 0, sizeof (fromAddr));
            rc = qcom_recvfrom(context->udpSock, (char*) buf->writePtr, rx, 0, (struct sockaddr*)&fromAddr, &fromAddrSize);
            if (rc <= 0) {
                AJ_ErrPrintf(("AJ_Net_RecvFrom(): qcom_recvfrom() failed, status=AJ_ERR_READ\n"));
                status = AJ_ERR_READ;
            } else {
                buf->writePtr += rc;
                status = AJ_OK;
            }
        }
    }

    return status;
}

static AJ_Status SendTo(AJ_IOBuffer* buf)
{
    NetContext* context = (NetContext*)buf->context;
    int ret;
    int tx = AJ_IO_BUF_AVAIL(buf);
    AJ_ASSERT(buf->direction == AJ_IO_BUF_TX);
    uint8_t sendSucceeded = FALSE;

    if (tx > 0) {
        /*
         * Multicast send over IPv4
         */

        if (context->udpSock != INVALID_SOCKET) {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(AJ_UDP_PORT);
            sin.sin_addr.s_addr = htonl(AJ_IPV4_MCAST_GROUP);

            ret = qcom_sendto((int)context->udpSock, (char*) buf->bufStart, tx, 0, (struct sockaddr*) &sin, sizeof(sin));
            if (tx == ret) {
                sendSucceeded = TRUE;
            } else {
                AJ_ErrPrintf(("AJ_Net_SendTo() failed 1 %d \n", ret));
            }
        }

        /*
         * Broadcast send over IPv4
         */
        if (context->udpSock != INVALID_SOCKET) {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(AJ_UDP_PORT);
            sin.sin_addr.s_addr = 0xFFFFFFFF;
            ret = qcom_sendto((int)context->udpSock, (char*) buf->bufStart, tx, 0, (struct sockaddr*) &sin, sizeof(sin));
            if (tx == ret) {
                sendSucceeded = TRUE;
            } else {
                AJ_ErrPrintf(("AJ_Net_SendTo() failed 2 %d \n", ret));
            }
        }

        /*
         * Multicast send over IPv6
         */

        if (context->udp6Sock != INVALID_SOCKET) {
            struct sockaddr_in6 sin6;
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = htons(AJ_UDP_PORT);
            memcpy(&sin6.sin6_addr, AJ_IPV6_MCAST_GROUP, sizeof(AJ_IPV6_MCAST_GROUP));
            ret = qcom_sendto((int)context->udp6Sock, (char*) buf->bufStart, tx, 0, (struct sockaddr*) &sin6, sizeof(sin6));
            if (tx == ret) {
                sendSucceeded = TRUE;
            } else {
                AJ_ErrPrintf(("AJ_Net_SendTo() failed 3 %d \n", ret));
            }
        }

        if (!sendSucceeded) {
            AJ_ErrPrintf(("AJ_Net_SendTo() failed\n"));
            return AJ_ERR_WRITE;
        }
    }
    AJ_IO_BUF_RESET(buf);
    return AJ_OK;
}


static int MCastUp4()
{
    int ret;
    struct sockaddr_in sin;
    struct ip_mreq {
        uint32_t imr_multiaddr;   /* IP multicast address of group */
        uint32_t imr_interface;   /* local IP address of interface */
    } mreq;

    int mcastSock = qcom_socket(PF_INET, SOCK_DGRAM, 0);
    if (mcastSock == INVALID_SOCKET) {
    	AJ_ErrPrintf(("MCastUp4: problem creating socket\n"));
        return INVALID_SOCKET;
    }

    /*
     * Bind our multicast port
     */
    sin.sin_family = AF_INET;
    sin.sin_port = htons(AJ_EphemeralPort());
    sin.sin_addr.s_addr = INADDR_ANY;
    ret = qcom_bind(mcastSock, (struct sockaddr*) &sin, sizeof(sin));
    if (ret < 0) {
        qcom_socket_close(mcastSock);
        return INVALID_SOCKET;
    }


    /*
     * Join the AllJoyn IPv4 multicast group
     */
    mreq.imr_multiaddr = htonl(AJ_IPV4_MCAST_GROUP);
    mreq.imr_interface = INADDR_ANY;
    ret = qcom_setsockopt(mcastSock, SOL_SOCKET, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));
    if (ret < 0) {
        AJ_WarnPrintf(("MCastUp4(): qcom_setsockopt(%d) failed\n", ret));
//        qcom_qocket_close(mcastSock);
//        return INVALID_SOCKET;
    }

    return mcastSock;
}


static int MCastUp6()
{
    int ret;
    struct ip_mreq {
        uint8_t ipv6mr_multiaddr[16];   /* IP multicast address of group */
        uint8_t ipv6mr_interface[16];   /* local IP address of interface */
    } group6;
    struct sockaddr_in6 sin6;
    int sock;
    uint8_t gblAddr[16];
    uint8_t locAddr[16];
    uint8_t gwAddr[16];
    uint8_t gblExtAddr[16];
    uint32_t linkPrefix;
    uint32_t glbPrefix;
    uint32_t gwPrefix;
    uint32_t glbExtPrefix;

    /*
     * We pass the current global IPv6 address into the sockopt for joining the multicast group.
     */
    ret = qcom_ip6_address_get(qcom_DeviceId, gblAddr, locAddr, gwAddr, gblExtAddr, &linkPrefix, &glbPrefix, &gwPrefix, &glbExtPrefix);
    if (ret != A_OK) {
        AJ_ErrPrintf(("MCastUp6(): problem getting IPv6 address info\n"));
        return INVALID_SOCKET;
    }
    /*
     * Create the IPv6 socket
     */
    sock = qcom_socket(PF_INET6, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        AJ_ErrPrintf(("MCastUp6(): qcom_socket() failed\n"));
        return INVALID_SOCKET;
    }
    /*
     * Bind to an emphemeral port
     */
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(AJ_EphemeralPort());
    ret = qcom_bind(sock,  (struct sockaddr*) &sin6, sizeof(sin6));
    if (ret < 0 ) {
        AJ_ErrPrintf(("MCastUp6(): qcom_bind(%d) failed %d\n", sock, ret));
        qcom_socket_close(sock);
        return INVALID_SOCKET;
    }
    /*
     * Join the AllJoyn IPv6 multicast group
     */
    memset(&group6, 0, sizeof(group6));
    memcpy(&group6.ipv6mr_multiaddr, AJ_IPV6_MCAST_GROUP, sizeof(AJ_IPV6_MCAST_GROUP));
    memcpy(&group6.ipv6mr_interface, locAddr, sizeof(locAddr));

    ret = qcom_setsockopt(sock, SOL_SOCKET, IPV6_JOIN_GROUP, (char*)&group6, sizeof(group6));
    if (ret < 0) {
        AJ_WarnPrintf(("MCastUp6(): qcom_setsockopt(%d) failed %d\n", sock, ret));
    }
    return sock;
}


AJ_Status AJ_Net_MCastUp(AJ_NetSocket* netSock)
{
    AJ_Status status = AJ_ERR_READ;
    int udpSock = MCastUp4();
    int udp6Sock = MCastUp6();

    if ((udpSock != INVALID_SOCKET) || (udp6Sock != INVALID_SOCKET)) {
        status = ConfigNetSock(netSock, RecvFrom, sizeof(rxData), SendTo, sizeof(txData));
        if (status == AJ_OK) {
            ((NetContext*)netSock->rx.context)->udpSock = udpSock;
            ((NetContext*)netSock->rx.context)->udp6Sock = udp6Sock;
        }
    }
    if (status != AJ_OK) {
        CloseNetSock(netSock);
    }
    return status;
}

void AJ_Net_MCastDown(AJ_NetSocket* netSock)
{
    CloseNetSock(netSock);
}

