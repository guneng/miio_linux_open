/* * Simple client, send data and quit
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "miio_agent.h"

#define DATA_MAX	1024

int main(int argc, char**argv)
{
    int sockfd, n;
    struct sockaddr_un servaddr;
    char buf[DATA_MAX];

    if (argc != 2) {
        printf("Usage: %s msg\n", argv[0]);
        return 1;
    }

    sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sun_family = AF_UNIX;
    strcpy(servaddr.sun_path, MIIO_AGENT_SERVER_PATH);

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        printf("Connect to server error: %s\n", MIIO_AGENT_SERVER_PATH);
        return -1;
    }

    send(sockfd, argv[1], strlen(argv[1]), 0);
    sleep(1);
    n = recv(sockfd, buf, DATA_MAX, MSG_DONTWAIT);
    if (n > 0) {
        buf[n] = '\0';
        printf("recv: %d %s\n", n, buf);
    }

    return 0;
}
