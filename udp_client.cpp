#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h> 
#include <iostream>
#include <errno.h>
#include <math.h>
#include <chrono>
#include <time.h>

#include "buff_size.h"

#define LOCAL_IP ("4.4.0.1")
#define OUT_PORT_LOCAL ("0")

#define USEC_RESOLUTION (200) /* in usec */
#define USER_PACKET_PER_BURST (672)
#define TOTAL_PACKET_PER_BURST (800.006696)
#define PACKETS_PER_BURST (800)

#define CHUNK_SIZE (1474)
#define NUM_LOOPS (100000000L)

#define DUMMY_SEND_FLAG (0x400) /* equals to MSG_SYN */
#ifndef SO_MAX_PACING_RATE
#define SO_MAX_PACING_RATE (47)
#endif

#define NOW() std::chrono::system_clock::now()
#define SUB_MILI(X, Y) std::chrono::duration_cast<std::chrono::milliseconds>(X - Y).count()

void error(const char *msg) {
	perror(msg);
	exit(0);
}

static int my_getaddrinfo(const char *host, const char *port, int *sockfd,
	struct sockaddr **sa, socklen_t *salen)
{
	struct addrinfo hints, *res, *ressave;
	int n, ret = -1;

	/*initilize addrinfo structure*/
	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	if ((n = getaddrinfo(host, port, &hints, &res)) != 0) {
		printf("udpclient error for %s, %s: %s", host, port,
			gai_strerror(n));
		return -1;
	}

	ressave = res;
	if (sockfd) {
		do {/* each of the returned IP address is tried*/
			int tmp;

			tmp = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
			if (tmp >= 0) {
				printf("inet_ntoa(((struct sockaddr_in*)res->ai_addr)->sin_addr): %s\n",
						inet_ntoa(((struct sockaddr_in*)res->ai_addr)->sin_addr));
				*sockfd = tmp;
				break; /*success*/
			}
		} while ((res = res->ai_next) != NULL);

		if (!res) {
			printf("Could not open a socket for %s:%s\n", host, port);
			goto Exit;
		}
	}

	*sa = (struct sockaddr*)malloc(res->ai_addrlen);
	memcpy(*sa, res->ai_addr, res->ai_addrlen);
	*salen = res->ai_addrlen;

	ret = 0;
Exit:
	freeaddrinfo(ressave);
	return ret;
}

static int run(int sockfd, struct sockaddr *sa, struct sockaddr *sa_local, socklen_t salen, socklen_t salen_local)
{
	char *buffer;
	uint64_t total_bytes = 0L;
	int ret = -1;
	float dummy_ratio = (TOTAL_PACKET_PER_BURST - USER_PACKET_PER_BURST) / TOTAL_PACKET_PER_BURST;
	int dummies_sent = 0;
	int next_dummies = 0;
	std::chrono::time_point<std::chrono::high_resolution_clock> start, end;
	std::chrono::milliseconds resolution(USEC_RESOLUTION);

	if (!(buffer = (char*)malloc(CHUNK_SIZE))) {
		printf("Could not allocate data buffer\n");
		return -1;
	}

	for (int i = 0; i < NUM_LOOPS; ++i) {
		start = NOW();
		for (int j = 0; j < USER_PACKET_PER_BURST; ++j) {
			if (sendto(sockfd, buffer, CHUNK_SIZE, 0, sa, salen) < 0) {
				/* buffers aren't available locally at the moment */
				if (errno == ENOBUFS) {
					printf("sendto error ENOBYFS");
					fflush(stdout);
					continue;
				}
				perror("error sending datagram");
				ret = -1;
				goto Exit;
			}

			/* inject dummy packets 10 times per burst */
			if (!(j % (PACKETS_PER_BURST/10))) {
				next_dummies = floor((i * TOTAL_PACKET_PER_BURST + j) * dummy_ratio - dummies_sent);
				for (int k = 0; k <= next_dummies; ++k) {
					if (sendto(sockfd, buffer, CHUNK_SIZE, 0, sa_local, salen_local) < 0) {
						/* buffers aren't available locally at the moment*/
						if (errno == ENOBUFS){
							printf("sendto error ENOBYFS");
							fflush(stdout);
							continue;
						}
						perror("error sending dummy datagram");
						ret = -1;
						goto Exit;
					}
				}
				dummies_sent += next_dummies;
			}
			total_bytes += CHUNK_SIZE;
		}
		end = NOW();
		usleep(USEC_RESOLUTION - SUB_MILI(end, start) - 10 /* slightly reduce sleep time */);

		while (start + resolution > NOW()); /* busy wait for the correct time */
	}
	printf("\n %lu bytes sent \n", total_bytes);
	ret = 0;

Exit:
	free(buffer);
	return ret;
}

int main(int argc, char *argv[]) {
	int sockfd = -1, rc;
	socklen_t salen, salen_local;
	struct sockaddr *sa = NULL, *sa_local = NULL;
	char *host, *port;
	int ret = -1;

	if (argc < 3 || argc > 4) {
		printf("usage: %s <hostname/IPaddress> <portnumber> [rate] \n", argv[0]);
		exit(-1);
	}

	host = argv[1];
	port = argv[2];

	if (my_getaddrinfo(host, port, &sockfd, &sa, &salen))
		goto Exit;
	if (my_getaddrinfo(LOCAL_IP, OUT_PORT_LOCAL, NULL, &sa_local, &salen_local))
		goto Exit;

	if (argc == 4) {
		uint64_t rate = atol(argv[3]);

		printf("settings rate to: %lu\n", rate);
		if (setsockopt(sockfd, SOL_SOCKET, SO_MAX_PACING_RATE, &rate, sizeof(rate))) {
			printf("setsockopt() failed \n");
			goto Exit;
		}
	}

	if (run(sockfd, sa, sa_local, salen, salen_local) < 0) {
		printf("program exit on error.\n");
		goto Exit;
	}

	ret = 0;

Exit:
	free(sa);
	free(sa_local);
	if (-1 < sockfd)
		close(sockfd);
	return ret;
}

